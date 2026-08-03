#include <vector>

#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/constevalvisitor.h"
#include "sema/passes/typecheckvisitor.h"
#include "source/sourcerange.h"
#include "symbol/constvalue.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "symbol/symbolutils.h"
#include "type/typeformat.h"
#include "type/typekind.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/numeric.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

bool TypeCheckVisitor::visitIdentifier(IdentifierNode* node)
{
    Symbol* s = node->mSymbol;

    // Make sure the symbol's type is resolved if it hasn't been yet.
    if (resolveType(s) == false)
    {
        return false;
    }

    // Set the resolved type.
    node->mResolvedType = s->mType;

    // Set the flags based on symbol flags & kind.
    if (s->mFlags.test(SymbolFlags::cMutable))
    {
        node->mFlags.set(cExprIsMutable, true);
    }

    if (s->mFlags.test(SymbolFlags::cInOut))
    {
        node->mFlags.set(cExprIsMutable, true);
    }

    // If the symbol is a constexpr, mark the identifier node as such.
    node->mFlags.set(cExprIsConstExpr, s->mFlags.test(SymbolFlags::cConstExpr));

    switch (s->mSymbolType)
    {
        case SymbolType::cGlobalVariable:
        case SymbolType::cMemberVariable:
        case SymbolType::cStackVariable:
        case SymbolType::cParameter:
        {
            node->mFlags.set(cExprIsLValue, true);
            break;
        }
        default:
        {
            break;
        }
    }

    return true;
}

bool TypeCheckVisitor::visitCast(CastNode* node)
{
    // Visit the type spec.
    if (visit(node->mTypeSpecifier) == false)
    {
        return false;
    }

    // The target type cannot be null.
    Type* targetType =
        requireNonNullType(node->mTypeSpecifier->mType, node->mTypeSpecifier->mSourceRange, "a cast target type");
    node->mTypeSpecifier->mType = targetType;

    // If we get an error type, we're done.
    if (isErrorType(targetType))
    {
        markError(node);
        return true;
    }

    // Resolve the target expression.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // If the target expression is an error, we're also done.
    if (hasValidResolvedType(node->mTarget) == false)
    {
        markError(node);
        return true;
    }

    // Finally, see if we are allowed to perform the cast.
    if (allowExplicitCast(node->mTarget->mResolvedType, targetType) == false)
    {
        mCtx.report<cInvalidCast>(node->mSourceRange,
                                  typeToString(node->mTarget->mResolvedType),
                                  typeToString(targetType));
        markError(node);
        return true;
    }

    // If we have a cast from float to int, make sure the float isn't too large (or small).
    PrimitiveTypeKind fromKind = getPrimitiveKind(node->mTarget->mResolvedType);
    PrimitiveTypeKind toKind = getPrimitiveKind(targetType);
    if (fromKind == PrimitiveTypeKind::cFloat && toKind == PrimitiveTypeKind::cInt &&
        node->mTarget->mFlags.test(cExprIsConstExpr))
    {
        // Do const eval stuff.
        ConstValue value;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        if (evaluator.evaluate(node->mTarget, value) && value.mKind == ConstValueKind::cPrimitive &&
            value.mPrimitiveKind == PrimitiveTypeKind::cFloat)
        {
            // Do the check and report on failure.
            i32 intValue = 0;
            if (checkedFloatToInt(value.as.mFloat, intValue) == false)
            {
                mCtx.report<cInvalidFloatToIntCast>(node->mSourceRange);
                markError(node);
                return true;
            }
        }
    }

    node->mResolvedType = targetType;

    // If the target is a constexpr, evaluate it.
    if (node->mTarget->mFlags.test(cExprIsConstExpr))
    {
        ConstValue value;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        node->mFlags.set(cExprIsConstExpr, evaluator.evaluate(node, value));
    }

    return true;
}

bool TypeCheckVisitor::visitThis(ThisNode* node)
{
    node->mResolvedType = mCurrentTypeSymbol->mType;

    // For now, "this" is always mutable.
    node->mFlags.set(cExprIsLValue, true);
    node->mFlags.set(cExprIsMutable, true);

    return true;
}

bool TypeCheckVisitor::visitIntLiteral(IntLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
    node->mFlags.set(cExprIsConstExpr, true);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitFloatLiteral(FloatLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cFloat);
    node->mFlags.set(cExprIsConstExpr, true);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitBoolLiteral(BoolLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
    node->mFlags.set(cExprIsConstExpr, true);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitStringLiteral(StringLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cString);
    node->mFlags.set(cExprIsConstExpr, true);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitNullLiteral(NullLiteralNode* node)
{
    Type* t = mCtx.mTypes.getNullType();
    node->mFlags.set(cExprIsConstExpr, true);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitFormatString(FormatStringNode* node)
{
    bool argsOk = true;

    // Go over all of our args.
    for (ExpressionNode* arg : node->mArgs)
    {
        // Resolve them first.
        if (visit(arg) == false)
        {
            return false;
        }

        if (hasValidResolvedType(arg) == false)
        {
            argsOk = false;
            continue;
        }

        if (requireNonVoidPrimitive(arg) == false)
        {
            argsOk = false;
            continue;
        }
    }

    if (argsOk == false)
    {
        markError(node);
        return true;
    }

    // Set the resolved type of the entire node to string.
    node->mResolvedType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cString);

    return true;
}

bool TypeCheckVisitor::visitNewObject(NewObjectNode* node)
{
    // Visit the type specifier first.
    if (visit(node->mTypeSpecifier) == false)
    {
        return false;
    }

    // Get the resolved type, which has to match the construction keyword.
    Type* baseType = node->mTypeSpecifier->mType;
    if (isErrorType(baseType))
    {
        markError(node);
        return true;
    }

    // Handle list/map stuff here.
    if (baseType->mKind == TypeKind::cList || baseType->mKind == TypeKind::cMap)
    {
        // Require new list/map.
        if (node->mConstructionKind != ConstructionKind::cReference)
        {
            mCtx.report<cInvalidMakeExpression>(node->mSourceRange, typeToString(baseType));
            markError(node);
            return true;
        }

        // Disallow initializer args.
        if (node->mInitializerArguments.empty() == false)
        {
            mCtx.report<cWrongArgumentCount>(node->mSourceRange, 0, node->mInitializerArguments.size());
            markError(node);
            return true;
        }

        // Set the resolved type as the type we have on the specifier.
        node->mResolvedType = baseType;

        return true;
    }

    if (node->mConstructionKind == ConstructionKind::cValue)
    {
        // If we expected a value and this is not a struct, fail.
        if (baseType->mKind != TypeKind::cStruct)
        {
            mCtx.report<cInvalidMakeExpression>(node->mSourceRange, typeToString(baseType));
            markError(node);
            return true;
        }
    }
    else
    {
        // If we expected a reference type and this is not a class, fail.
        if (baseType->mKind != TypeKind::cClass)
        {
            mCtx.report<cInvalidNewExpression>(node->mSourceRange, typeToString(baseType));
            markError(node);
            return true;
        }
    }

    // Set the resolved type as the type we have on the specifier.
    node->mResolvedType = baseType;

    // Now the two big cases we treat here.
    auto* aggType = static_cast<AggregateType*>(baseType);
    if (baseType->mKind == TypeKind::cClass)
    {
        // Use the cached class initializer.
        Symbol* initializerSymbol = aggType->mInitMethodSymbol;
        if (initializerSymbol == nullptr)
        {
            mCtx.report<cMissingClassInitializer>(node->mSourceRange, typeToString(aggType));
            markError(node);
            return true;
        }

        // Resolve that if it wasn't already.
        if (resolveType(initializerSymbol) == false)
        {
            return false;
        }

        // Check if we can access the initializer.
        if (checkMemberAccess(initializerSymbol, aggType->mSymbol) == false)
        {
            mCtx.report<cPrivateMemberAccess>(node->mSourceRange,
                                              initializerSymbol->mIdentifier,
                                              typeToString(aggType));
            markError(node);
            return true;
        }

        auto* funcType = static_cast<FunctionType*>(initializerSymbol->mType);

        // Resolve the arguments.
        for (ExpressionNode*& argument : node->mInitializerArguments)
        {
            if (visit(argument) == false)
            {
                return false;
            }
        }

        // Set the initializer symbol.
        node->mInitMethodSymbol = initializerSymbol;

        // Validate the initializer call.
        if (checkCallArguments(node->mSourceRange, node->mInitializerArguments, funcType) == false)
        {
            markError(node);
        }

        return true;
    }

    // Structs work differently as they have no init function.
    std::vector<Symbol*> fields;
    fields.reserve(aggType->mSymbol->mMembers.size());

    for (Symbol* member : aggType->mSymbol->mMembers)
    {
        if (member->mSymbolType != SymbolType::cMemberVariable)
        {
            continue;
        }

        // Resolve all member types if they weren't already.
        if (resolveType(member) == false)
        {
            return false;
        }

        fields.push_back(member);
    }

    // Now validate the field initializers.
    std::vector initialized(fields.size(), false);
    bool initializersOk = true;

    for (auto init : node->mFieldInitializers)
    {
        // Resolve the initializer value.
        if (visit(init->mValue) == false)
        {
            return false;
        }

        // Check whether the specified initializer actually exists on the aggregate.
        usize fieldIndex = fields.size();
        for (usize i = 0; i < fields.size(); ++i)
        {
            if (fields[i]->mIdentifier == init->mIdentifier)
            {
                fieldIndex = i;
                break;
            }
        }

        if (fieldIndex == fields.size())
        {
            mCtx.report<cMemberNotFound>(init->mIdentifierRange, typeToString(baseType), init->mIdentifier);
            initializersOk = false;
            continue;
        }

        // Get the relevant field symbol.
        Symbol* field = fields[fieldIndex];
        if (checkMemberAccess(field, aggType->mSymbol) == false)
        {
            mCtx.report<cPrivateMemberAccess>(init->mIdentifierRange, field->mIdentifier, typeToString(baseType));
            initializersOk = false;
            continue;
        }

        // If the field is already initialized, this is an error.
        if (initialized[fieldIndex])
        {
            mCtx.report<cDuplicateFieldInitializer>(init->mIdentifierRange, init->mIdentifier);
            initializersOk = false;
            continue;
        }

        initialized[fieldIndex] = true;
        init->mResolvedField = field;

        // If something went wrong earlier for the field or initializer, skip it.
        if (hasValidResolvedType(init->mValue) == false || isErrorType(field->mType))
        {
            initializersOk = false;
            continue;
        }

        // Otherwise, do conversion stuff.
        if (convertExpressionToType(init->mValue, field->mType) == false)
        {
            initializersOk = false;
        }
    }

    // Then, go over all field symbols to see if we initialized all of them.
    for (usize i = 0; i < fields.size(); ++i)
    {
        if (initialized[i] == false)
        {
            //  If the field declaration has no default initializer that can be used instead, report an error.
            Symbol* field = fields[i];
            auto* fieldDecl = static_cast<VariableDeclarationStatementNode*>(field->mDeclNode);
            if (fieldDecl->mInit == nullptr)
            {
                auto diag = mCtx.report<cMissingFieldInitializer>(node->mSourceRange, field->mIdentifier);
                SourceRange fieldRange = getSymbolSourceRange(field);
                if (fieldRange.isValid())
                {
                    diag.note<cFieldDeclaredHere>(fieldRange, field->mIdentifier);
                }

                // Keep going over all fields even if one failed.
                initializersOk = false;
            }
        }
    }

    if (initializersOk == false)
    {
        markError(node);
    }

    ConstValue cv;
    ConstEvalVisitor cev{mCtx.mAllocator};
    node->mFlags.set(cExprIsConstExpr, cev.evaluate(node, cv));

    return true;
}

bool TypeCheckVisitor::checkCallArguments(SourceRange range,
                                          ArrayView<ExpressionNode*>& args,
                                          const FunctionType* funcType)
{
    // If the number of arguments doesn't match, this won't work.
    if (funcType->mParamTypes.size() != args.size())
    {
        mCtx.report<cWrongArgumentCount>(range, funcType->mParamTypes.size(), args.size());
        return false;
    }

    bool argsOk = true;

    // Check each argument.
    for (usize i = 0; i < args.size(); ++i)
    {
        ExpressionNode*& arg = args[i];

        if (hasValidResolvedType(arg) == false)
        {
            // This already should have errored somewhere else, so continue here.
            argsOk = false;
            continue;
        }

        const FunctionParam& param = funcType->mParamTypes[i];
        bool isInOutArgument = arg->mFlags.test(cExprIsInOutArgument);
        if (isErrorType(param.mType))
        {
            argsOk = false;
            continue;
        }

        if (param.mIsInOut)
        {
            if (isInOutArgument == false)
            {
                mCtx.report<cMissingInOutArgument>(arg->mSourceRange, i + 1);
                argsOk = false;
                continue;
            }

            // Inout params require an exact match.
            if (arg->mResolvedType != param.mType)
            {
                mCtx.report<cInvalidArgumentType>(arg->mSourceRange,
                                                  i + 1,
                                                  typeToString(param.mType),
                                                  typeToString(arg->mResolvedType));
                argsOk = false;
                continue;
            }

            // The arg has to be an assignable lvalue.
            if (requireAssignableLValue(arg) == false)
            {
                argsOk = false;
                continue;
            }

            if (isAddressableExpression(arg) == false)
            {
                mCtx.report<cInOutBindingFailed>(arg->mSourceRange);
                argsOk = false;
                continue;
            }

            // It's actually used as an lvalue.
            markExpressionUsedAsLValue(arg);
        }
        else
        {
            if (isInOutArgument)
            {
                mCtx.report<cUnexpectedInOutArgument>(arg->mSourceRange, i + 1);
                argsOk = false;
                continue;
            }

            // For normal params, allow casts.
            if (convertArgumentToParameterType(arg, param.mType, i) == false)
            {
                argsOk = false;
                continue;
            }
        }
    }

    return argsOk;
}

bool TypeCheckVisitor::visitFunctionCall(FunctionCallNode* node)
{
    {
        // We're now in a call (relevant for member access).
        ScopedValueBinder cs{mInCall, true};

        // Resolve the receiver.
        if (visit(node->mReceiver) == false)
        {
            return false;
        }

        // We have resolved the receiver and can leave the scope.
    }

    if (hasValidResolvedType(node->mReceiver) == false)
    {
        markError(node);
        return true;
    }

    if (node->mReceiver->mResolvedType->mKind != TypeKind::cFunction)
    {
        mCtx.report<cNotAFunction>(node->mReceiver->mSourceRange);
        markError(node);
        return true;
    }

    // For class/struct methods, we allow function calls as temporary lvalues (but not as refs!).
    // We want to prevent the const folder from replacing the entire thing since we need the receiver address for the
    // method call.
    if (node->mReceiver->mNodeType == NodeType::cMemberAccess)
    {
        auto* memberAccess = static_cast<MemberAccessNode*>(node->mReceiver);
        if (memberAccess->mSymbol != nullptr && memberAccess->mSymbol->mSymbolType == SymbolType::cMemberFunction)
        {
            // Since classes are refs anyway, we only need to consider structs here.
            Type* receiverType = memberAccess->mReceiver->mResolvedType;
            if (receiverType->mKind == TypeKind::cStruct)
            {
                markExpressionUsedAsLValue(memberAccess->mReceiver);
            }
        }
    }

    // Compare the function type against the params.
    auto* funcType = static_cast<FunctionType*>(node->mReceiver->mResolvedType);

    // Resolve the args.
    for (usize i = 0; i < node->mArgs.size(); ++i)
    {
        if (visit(node->mArgs[i]) == false)
        {
            return false;
        }
    }

    if (checkCallArguments(node->mSourceRange, node->mArgs, funcType) == false)
    {
        markError(node);
        return true;
    }

    // Set the resolved type to the function's return type.
    node->mResolvedType = funcType->mReturnType;
    if (hasValidResolvedType(node) == false)
    {
        markError(node);
    }

    return true;
}

bool TypeCheckVisitor::visitIndexCall(IndexCallNode* node)
{
    // Resolve the receiver.
    if (visit(node->mReceiver) == false)
    {
        return false;
    }

    // Resolve the index.
    if (visit(node->mIndex) == false)
    {
        return false;
    }

    // If we have none or an unresolved type, bail.
    if (hasValidResolvedType(node->mReceiver) == false || hasValidResolvedType(node->mIndex) == false)
    {
        markError(node);
        return true;
    }

    Type* receiverType = node->mReceiver->mResolvedType;
    if (receiverType->mKind == TypeKind::cList)
    {
        // Check if the index is an int.
        PrimitiveTypeKind indexKind = getPrimitiveKind(node->mIndex->mResolvedType);
        if (indexKind != PrimitiveTypeKind::cInt)
        {
            mCtx.report<cInvalidIndexType>(node->mIndex->mSourceRange, typeToString(node->mIndex->mResolvedType));
            markError(node);
            return true;
        }

        auto* listType = static_cast<ListType*>(receiverType);
        node->mResolvedType = listType->mElement;

        // Index expressions are assignable lvalues. List elements are loaded/stored through list opcodes.
        node->mFlags.set(cExprIsLValue, true);
        node->mFlags.set(cExprIsMutable, true);

        return true;
    }

    if (receiverType->mKind == TypeKind::cMap)
    {
        auto* mapType = static_cast<MapType*>(receiverType);
        if (convertExpressionToType(node->mIndex, mapType->mKey) == false)
        {
            markError(node);
            return true;
        }

        node->mResolvedType = mapType->mValue;

        // Map index expressions are assignable lvalues lowered through MapGet/MapSet.
        node->mFlags.set(cExprIsLValue, true);
        node->mFlags.set(cExprIsMutable, true);

        return true;
    }

    mCtx.report<cNotIndexable>(node->mReceiver->mSourceRange, typeToString(receiverType));
    markError(node);
    return true;
}

bool TypeCheckVisitor::visitMemberAccess(MemberAccessNode* node)
{
    // Always resolve the receiver.
    if (visit(node->mReceiver) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mReceiver) == false)
    {
        markError(node);
        return true;
    }

    // The receiver has to be a struct, class, interface, or built-in container type.
    Type* receiverType = node->mReceiver->mResolvedType;

    // Handle list member access.
    if (receiverType->mKind == TypeKind::cList)
    {
        auto* listType = static_cast<ListType*>(receiverType);
        Type* voidType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cVoid);
        Type* intType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
        Type* boolType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
        Type* returnType = nullptr;
        std::vector<FunctionParam> params;

        if (identifierEquals(node->mMember, "size"))
        {
            returnType = intType;
        }
        else if (identifierEquals(node->mMember, "isEmpty"))
        {
            returnType = boolType;
        }
        else if (identifierEquals(node->mMember, "add") || identifierEquals(node->mMember, "push"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{listType->mElement, false});
        }
        else if (identifierEquals(node->mMember, "addList"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{receiverType, false});
        }
        else if (identifierEquals(node->mMember, "pop"))
        {
            returnType = listType->mElement;
        }
        else if (identifierEquals(node->mMember, "back"))
        {
            returnType = listType->mElement;
        }
        else if (identifierEquals(node->mMember, "insertAt"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{intType, false});
            params.push_back(FunctionParam{listType->mElement, false});
        }
        else if (identifierEquals(node->mMember, "removeAt"))
        {
            returnType = listType->mElement;
            params.push_back(FunctionParam{intType, false});
        }
        else if (identifierEquals(node->mMember, "indexOf"))
        {
            returnType = intType;
            params.push_back(FunctionParam{listType->mElement, false});
        }
        else if (identifierEquals(node->mMember, "contains"))
        {
            returnType = boolType;
            params.push_back(FunctionParam{listType->mElement, false});
        }
        else if (identifierEquals(node->mMember, "clear"))
        {
            returnType = voidType;
        }
        else if (identifierEquals(node->mMember, "reserve"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{intType, false});
        }
        else
        {
            mCtx.report<cMemberNotFound>(node->mReceiver->mSourceRange, typeToString(receiverType), node->mMember);
            markError(node);
            return true;
        }

        // All of these are calls.
        if (mInCall == false)
        {
            mCtx.report<cMethodAccessWithoutCall>(node->mSourceRange, node->mMember, typeToString(receiverType));
            markError(node);
            return true;
        }

        node->mResolvedType = mCtx.mTypes.getOrAddFunction(returnType, params);

        return true;
    }

    // Handle map member access.
    if (receiverType->mKind == TypeKind::cMap)
    {
        auto* mapType = static_cast<MapType*>(receiverType);
        Type* voidType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cVoid);
        Type* intType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
        Type* boolType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
        Type* returnType = nullptr;
        std::vector<FunctionParam> params;

        if (identifierEquals(node->mMember, "size"))
        {
            returnType = intType;
        }
        else if (identifierEquals(node->mMember, "isEmpty"))
        {
            returnType = boolType;
        }
        else if (identifierEquals(node->mMember, "clear"))
        {
            returnType = voidType;
        }
        else if (identifierEquals(node->mMember, "containsKey"))
        {
            returnType = boolType;
            params.push_back(FunctionParam{mapType->mKey, false});
        }
        else if (identifierEquals(node->mMember, "get"))
        {
            returnType = mapType->mValue;
            params.push_back(FunctionParam{mapType->mKey, false});
        }
        else if (identifierEquals(node->mMember, "set"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{mapType->mKey, false});
            params.push_back(FunctionParam{mapType->mValue, false});
        }
        else if (identifierEquals(node->mMember, "remove"))
        {
            returnType = boolType;
            params.push_back(FunctionParam{mapType->mKey, false});
        }
        else if (identifierEquals(node->mMember, "reserve"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{intType, false});
        }
        else
        {
            mCtx.report<cMemberNotFound>(node->mReceiver->mSourceRange, typeToString(receiverType), node->mMember);
            markError(node);
            return true;
        }

        // All of these are calls.
        if (mInCall == false)
        {
            mCtx.report<cMethodAccessWithoutCall>(node->mSourceRange, node->mMember, typeToString(receiverType));
            markError(node);
            return true;
        }

        node->mResolvedType = mCtx.mTypes.getOrAddFunction(returnType, params);

        return true;
    }

    if (receiverType->mKind != TypeKind::cStruct && receiverType->mKind != TypeKind::cClass &&
        receiverType->mKind != TypeKind::cInterface)
    {
        mCtx.report<cInvalidMemberReceiver>(node->mReceiver->mSourceRange, typeToString(receiverType));
        markError(node);
        return true;
    }

    Symbol* memberOwnerSymbol = nullptr;
    if (receiverType->mKind == TypeKind::cInterface)
    {
        memberOwnerSymbol = static_cast<InterfaceType*>(receiverType)->mSymbol;
    }
    else
    {
        memberOwnerSymbol = static_cast<AggregateType*>(receiverType)->mSymbol;
    }

    // Find out if the member exists.
    Symbol* memberSymbol = nullptr;
    for (Symbol* member : memberOwnerSymbol->mMembers)
    {
        if (member->mIdentifier == node->mMember)
        {
            memberSymbol = member;
            break;
        }
    }

    if (memberSymbol == nullptr)
    {
        mCtx.report<cMemberNotFound>(node->mReceiver->mSourceRange,
                                     typeToString(node->mReceiver->mResolvedType),
                                     node->mMember);
        markError(node);
        return true;
    }

    // check whether we can access the member.
    if (checkMemberAccess(memberSymbol, memberOwnerSymbol) == false)
    {
        mCtx.report<cPrivateMemberAccess>(node->mSourceRange,
                                          memberSymbol->mIdentifier,
                                          typeToString(node->mReceiver->mResolvedType));
        markError(node);
        return true;
    }

    // Resolve the type of the member.
    if (resolveType(memberSymbol) == false)
    {
        return false;
    }

    // Disallow access to class initializers.
    if (receiverType->mKind == TypeKind::cClass &&
        static_cast<AggregateType*>(receiverType)->mInitMethodSymbol == memberSymbol)
    {
        mCtx.report<cInitializerAccess>(node->mSourceRange);
        markError(node);
        return true;
    }

    node->mSymbol = memberSymbol;
    node->mResolvedType = memberSymbol->mType;

    // Make sure this is not something like a.b where b is a method.
    if (memberSymbol->mSymbolType == SymbolType::cMemberFunction && mInCall == false)
    {
        // We're accessing a method without calling it.
        mCtx.report<cMethodAccessWithoutCall>(node->mSourceRange,
                                              memberSymbol->mIdentifier,
                                              typeToString(node->mReceiver->mResolvedType));
        markError(node);
        return true;
    }

    if (memberSymbol->mSymbolType == SymbolType::cMemberVariable)
    {
        bool receiverCanMutate = false;
        bool memberIsLValue = false;

        if (receiverType->mKind == TypeKind::cClass)
        {
            // Class receivers are handles, so their fields are addressable through the object.
            receiverCanMutate = true;
            memberIsLValue = true;
        }
        else if (receiverType->mKind == TypeKind::cStruct)
        {
            bool receiverIsAddressable = isAddressableExpression(node->mReceiver);

            memberIsLValue = receiverIsAddressable;
            receiverCanMutate = receiverIsAddressable && node->mReceiver->mFlags.test(cExprIsMutable);
        }

        if (memberIsLValue)
        {
            node->mFlags.set(cExprIsLValue, true);
        }

        // The member access is only mutable if the receiver can mutate and the member is actually mutable.
        if (receiverCanMutate && memberSymbol->mFlags.test(SymbolFlags::cMutable))
        {
            node->mFlags.set(cExprIsMutable, true);
        }

        // For structs, check constexpr-ness.
        if (receiverType->mKind == TypeKind::cStruct && node->mReceiver->mFlags.test(cExprIsConstExpr))
        {
            ConstValue cv;
            ConstEvalVisitor cev{mCtx.mAllocator};
            node->mFlags.set(cExprIsConstExpr, cev.evaluate(node, cv));
        }
    }

    return true;
}

bool TypeCheckVisitor::visitModuleAccess(ModuleAccessNode* node)
{
    // For modules, the type only depends on the RHS after resolution.
    if (visit(node->mRight) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mRight) == false)
    {
        markError(node);
        return true;
    }

    // The resolved type is the one of the RHS.
    node->mResolvedType = node->mRight->mResolvedType;

    // For now we simply copy the flags.
    node->mFlags = node->mRight->mFlags;

    return true;
}

} // namespace simlang
