#include <vector>

#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/typecheckvisitor.h"
#include "source/sourcerange.h"
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
    if (isMissingOrErrorType(targetType))
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

    node->mResolvedType = targetType;

    return true;
}

bool TypeCheckVisitor::visitThis(ThisNode* node)
{
    node->mResolvedType = mCurrentTypeSymbol->mType;

    // "this" cannot be rebound, but mutable fields can still be changed through it.
    node->mFlags.set(cExprIsMutable, true);

    return true;
}

bool TypeCheckVisitor::visitIntLiteral(IntLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitFloatLiteral(FloatLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cFloat);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitBoolLiteral(BoolLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitStringLiteral(StringLiteralNode* node)
{
    Type* t = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cString);
    node->mResolvedType = t;

    return true;
}

bool TypeCheckVisitor::visitNullLiteral(NullLiteralNode* node)
{
    Type* t = mCtx.mTypes.getNullType();
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

        if (requirePrintablePrimitive(arg) == false)
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
    // If this has already been resolved, we're done.
    if (node->mResolvedType != nullptr)
    {
        return true;
    }

    // Visit the type specifier first.
    if (visit(node->mTypeSpecifier) == false)
    {
        return false;
    }

    // Get the resolved type.
    Type* type = node->mTypeSpecifier->mType;
    if (type->mKind == TypeKind::cError)
    {
        markError(node);
        return true;
    }

    // We only accept structs, classes, lists, and maps here.
    if (type->mKind != TypeKind::cStruct && type->mKind != TypeKind::cClass && type->mKind != TypeKind::cList &&
        type->mKind != TypeKind::cMap)
    {
        mCtx.report<cInvalidNewExpression>(node->mSourceRange, typeToString(type));
        markError(node);
        return true;
    }

    // Resolve the args.
    for (CallArgument& arg : node->mArgs)
    {
        if (visit(arg.mValue) == false)
        {
            return false;
        }
    }

    // If we have a list or map, complain if we have any args.
    if (type->mKind == TypeKind::cList || type->mKind == TypeKind::cMap)
    {
        node->mResolvedType = type;

        if (node->mArgs.empty() == false)
        {
            mCtx.report<cWrongArgumentCount>(node->mSourceRange, 0, node->mArgs.size());
            markError(node);
        }

        return true;
    }

    // For structs and classes, get the constructor symbol and resolve that.
    auto* aggregateType = static_cast<AggregateType*>(type);
    Symbol* constructor = aggregateType->mConstructorSymbol;
    if (resolveType(constructor) == false)
    {
        return false;
    }

    // Make sure the constructor is accessible.
    if (checkMemberAccess(constructor, aggregateType->mSymbol) == false)
    {
        mCtx.report<cPrivateMemberAccess>(node->mSourceRange, constructor->mIdentifier, typeToString(type));
        markError(node);
        return true;
    }

    // Fill the node with the constructor and resolved type.
    node->mConstructorSymbol = constructor;
    node->mResolvedType = type;

    // Make sure the constructor stuff matches.
    auto* functionType = static_cast<FunctionType*>(constructor->mType);
    if (checkCallArguments(node->mSourceRange, node->mArgs, functionType) == false)
    {
        markError(node);
    }

    return true;
}

bool TypeCheckVisitor::checkCallArguments(SourceRange range,
                                          ArrayView<CallArgument>& args,
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
        CallArgument& callArgument = args[i];
        ExpressionNode*& arg = callArgument.mValue;

        if (hasValidResolvedType(arg) == false)
        {
            // This already should have errored somewhere else, so continue here.
            argsOk = false;
            continue;
        }

        const FunctionParam& param = funcType->mParamTypes[i];
        bool isInOutArgument = callArgument.mIsInOut;
        if (isMissingOrErrorType(param.mType))
        {
            argsOk = false;
            continue;
        }

        if (param.mIsInOut)
        {
            if (isInOutArgument == false)
            {
                mCtx.report<cMissingInOutArgument>(callArgument.mSourceRange, i + 1);
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
                mCtx.report<cUnexpectedInOutArgument>(callArgument.mSourceRange, i + 1);
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
        // Only the exact receiver may use call-only symbols such as methods. Tracking the entire
        // receiver subtree as "in a call" would accidentally admit method values in a ternary or
        // another compound expression.
        ScopedValueBinder cs{mDirectCallReceiver, node->mReceiver};

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
    for (CallArgument& argument : node->mArgs)
    {
        if (visit(argument.mValue) == false)
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

        // Map index expressions are assignable lvalues lowered through LoadMapValue/StoreMapValue.
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
        Type* voidType = mCtx.mTypes.getVoidType();
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
        else if (identifierEquals(node->mMember, "resize"))
        {
            returnType = voidType;
            params.push_back(FunctionParam{intType, false});
            params.push_back(FunctionParam{listType->mElement, false});
        }
        else
        {
            mCtx.report<cMemberNotFound>(node->mReceiver->mSourceRange, typeToString(receiverType), node->mMember);
            markError(node);
            return true;
        }

        // All of these are calls.
        if (mDirectCallReceiver != node)
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
        Type* voidType = mCtx.mTypes.getVoidType();
        Type* intType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
        Type* boolType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
        Type* returnType = nullptr;
        std::vector<FunctionParam> params;

        if (identifierEquals(node->mMember, "getKeys"))
        {
            returnType = mCtx.mTypes.getOrAddList(mapType->mKey);
        }
        else if (identifierEquals(node->mMember, "getValues"))
        {
            returnType = mCtx.mTypes.getOrAddList(mapType->mValue);
        }
        else if (identifierEquals(node->mMember, "size"))
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
        else if (identifierEquals(node->mMember, "tryGet"))
        {
            returnType = boolType;
            params.push_back(FunctionParam{mapType->mKey, false});
            params.push_back(FunctionParam{mapType->mValue, true});
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
        if (mDirectCallReceiver != node)
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

    // Disallow access to constructors.
    if ((receiverType->mKind == TypeKind::cStruct || receiverType->mKind == TypeKind::cClass) &&
        static_cast<AggregateType*>(receiverType)->mConstructorSymbol == memberSymbol)
    {
        mCtx.report<cConstructorAccess>(node->mSourceRange);
        markError(node);
        return true;
    }

    node->mSymbol = memberSymbol;
    node->mResolvedType = memberSymbol->mType;

    // Make sure this is not something like a.b where b is a method.
    if (memberSymbol->mSymbolType == SymbolType::cMemberFunction && mDirectCallReceiver != node)
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
            // Struct receivers are only allowed if they are non-temporary struct values.
            // That means we want to allow stuff like:
            // this.x = y ("this" access has already been rewritten to be explicit here)
            // a[i].x = y;
            // Thus, we either require the receiver to be an lvalue, or "this" (which is not an lvalue).
            bool receiverHasStorage =
                node->mReceiver->mNodeType == NodeType::cThis || node->mReceiver->mFlags.test(cExprIsLValue);

            memberIsLValue = receiverHasStorage;
            receiverCanMutate = receiverHasStorage && node->mReceiver->mFlags.test(cExprIsMutable);
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
