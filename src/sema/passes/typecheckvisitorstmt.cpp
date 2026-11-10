#include <string_view>
#include <utility>

#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/typecheckvisitor.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
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

static constexpr std::string_view cListTypeName = "list";
static constexpr std::string_view cMapTypeName = "map";

static bool bodyEndsWithReturn(StatementNode* body)
{
    if (body->mNodeType == NodeType::cReturnStatement)
    {
        return true;
    }

    if (body->mNodeType != NodeType::cBlockStatement)
    {
        return false;
    }

    auto* block = static_cast<BlockStatementNode*>(body);
    return (block->mStatements.empty() == false) &&
           (block->mStatements.back()->mNodeType == NodeType::cReturnStatement);
}

static bool isSupportedCollectionValueType(Type* type)
{
    if (type == nullptr)
    {
        return false;
    }

    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        case TypeKind::cStruct:
        case TypeKind::cClass:
        case TypeKind::cInterface:
        case TypeKind::cList:
        case TypeKind::cMap:
        case TypeKind::cFunction:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

static bool isValidConstType(Type* type)
{
    switch (getPrimitiveKind(type))
    {
        case PrimitiveTypeKind::cInt:
        case PrimitiveTypeKind::cFloat:
        case PrimitiveTypeKind::cBool:
        case PrimitiveTypeKind::cString:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool TypeCheckVisitor::visitMultiReturnTypeSpecifier(MultiReturnTypeSpecifierNode* node)
{
    std::vector<Type*> types;
    bool valid = true;

    // Resolve all the types.
    for (TypeSpecifierNode* spec : node->mTypes)
    {
        // Visit the specifier.
        if (visit(spec) == false)
        {
            return false;
        }

        // We need a valid type.
        Type* type = requireValueType(spec->mType, spec->mSourceRange, "a return component");

        // All have to be valid for this node to be valid.
        valid = valid && isMissingOrErrorType(type) == false;

        types.push_back(type);
    }

    // Register the type if we have one.
    node->mType = valid ? mCtx.mTypes.getOrAddMultiReturn(types) : getErrorType();

    return true;
}

bool TypeCheckVisitor::visitMultiVariableDeclarationStatement(MultiVariableDeclarationStatementNode* node)
{
    // Resolve the initializer.
    if (visit(node->mInit) == false)
    {
        return false;
    }

    Type* type = node->mInit->mResolvedType;
    ArrayView results{&type, 1};

    if (type->mKind == TypeKind::cMultiReturn)
    {
        // If we have multiple return types, use that instead.
        results = static_cast<MultiReturnType*>(type)->mTypes;
    }
    else if (type->mKind == TypeKind::cVoid)
    {
        // If we have void, we have none.
        results = {};
    }

    // If the size doesn't match and we didn't error on the type already, complain.
    bool matches = results.size() == node->mBindings.size();
    if (matches == false && isMissingOrErrorType(type) == false)
    {
        mCtx.report<cResultCountMismatch>(node->mSourceRange, node->mBindings.size(), results.size());
    }

    // Set the symbol type for every binding that we have.
    for (usize i = 0; i < node->mBindings.size(); ++i)
    {
        if (auto* binding = node->mBindings[i])
        {
            Symbol* symbol = binding->mSymbol;
            symbol->mType = matches ? results[i] : getErrorType();

            if (symbol->mFlags.test(SymbolFlags::cMutable) == false && isMissingOrErrorType(symbol->mType) == false &&
                isValidConstType(symbol->mType) == false)
            {
                mCtx.report<cInvalidConstType>(binding->mIdentifierRange, typeToString(symbol->mType));
            }
        }
    }

    return true;
}

bool TypeCheckVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    Symbol* s = node->mSymbol;

    // Visit the type specifier if present.
    if (node->mTypeSpec != nullptr && visit(node->mTypeSpec) == false)
    {
        return false;
    }

    // Resolve the init expression.
    if (node->mInit != nullptr)
    {
        if (visit(node->mInit) == false)
        {
            return false;
        }
    }

    Type* initType = node->mInit ? node->mInit->mResolvedType : nullptr;

    // Check whether our own type is implicit or explicit.
    if (node->mTypeSpec == nullptr)
    {
        // Null cannot be implicit.
        if (initType->mKind == TypeKind::cNull)
        {
            mCtx.report<cCannotInferType>(node->mInit->mSourceRange, node->mIdentifier);
            node->mSymbol->mType = getErrorType();
            return true;
        }

        Type* inferredType = requireValueType(initType, node->mInit->mSourceRange, "a variable type");
        if (isMissingOrErrorType(inferredType))
        {
            node->mSymbol->mType = getErrorType();
            return true;
        }

        // Otherwise we can bind the type.
        node->mSymbol->mType = inferredType;
    }
    else
    {
        // Variables need a value type.
        Type* declaredType = requireValueType(node->mTypeSpec->mType, node->mTypeSpec->mSourceRange, "a variable type");

        // If the type is explicit, make sure it's the same as the initializer if we have one.
        if (initType != nullptr)
        {
            convertExpressionToType(node->mInit, declaredType);
        }

        // Use the type from the type spec.
        node->mSymbol->mType = declaredType;
    }

    // Const declarations are restricted to scalar and string types.
    if (s->mFlags.test(SymbolFlags::cMutable) == false && isMissingOrErrorType(s->mType) == false &&
        isValidConstType(s->mType) == false)
    {
        mCtx.report<cInvalidConstType>(node->mIdentifierRange, typeToString(s->mType));
    }

    return true;
}

bool TypeCheckVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // Resolve the signature.
    if (resolveFunctionSignature(node) == false)
    {
        return false;
    }

    auto* funcType = static_cast<FunctionType*>(node->mSymbol->mType);
    Type* functionReturnType = funcType->mReturnType;

    // Set the current return type for the function.
    ScopedValueBinder returnTypeScope{mCurrentReturnType, functionReturnType};
    ScopedValueBinder functionScope{mCurrentFunction, node};

    // Visit the body if this function has one. Interface methods only declare a signature.
    if (node->mBody != nullptr && visit(node->mBody) == false)
    {
        return false;
    }

    if (node->mBody != nullptr && isMissingOrErrorType(functionReturnType) == false &&
        functionReturnType->mKind != TypeKind::cVoid)
    {
        if (bodyEndsWithReturn(node->mBody) == false)
        {
            mCtx.report<cMissingReturnStatement>(node->mIdentifierRange,
                                                 node->mIdentifier,
                                                 typeToString(functionReturnType));
        }
    }

    return true;
}

bool TypeCheckVisitor::visitLambda(LambdaNode* node)
{
    // Resolve the return type if we have one.
    Type* returnType = mCtx.mTypes.getVoidType();
    if (node->mReturnTypeSpec != nullptr)
    {
        if (visit(node->mReturnTypeSpec) == false)
        {
            return false;
        }

        returnType = requireNonNullType(node->mReturnTypeSpec->mType,
                                        node->mReturnTypeSpec->mSourceRange,
                                        "a lambda return type");
        node->mReturnTypeSpec->mType = returnType;
    }

    // Resolve the params.
    std::vector<FunctionParam> paramTypes;
    paramTypes.reserve(node->mParams.size());

    for (ParamNode* param : node->mParams)
    {
        if (visit(param) == false)
        {
            return false;
        }

        auto* paramDecl = static_cast<ParamDeclarationNode*>(param);
        paramTypes.push_back(FunctionParam{paramDecl->mSymbol->mType, paramDecl->mIsInOut});
    }

    // Create the function (lambda) type based on the return and param types we obtained.
    node->mSymbol->mType = mCtx.mTypes.getOrAddFunction(returnType, paramTypes);
    // Resolve the type.
    node->mResolvedType = node->mSymbol->mType;
    // If we are in a method, also register the type of the object so "this" can easily be typed.
    node->mLexicalThisType = mCurrentTypeSymbol == nullptr ? nullptr : mCurrentTypeSymbol->mType;

    // Create all the scopes before processing the body.
    ScopedValueBinder returnTypeScope{mCurrentReturnType, returnType};
    ScopedValueBinder breakScope{mBreakContextDepth, 0};
    ScopedValueBinder continueScope{mContinueContextDepth, 0};
    ScopedValueBinder<FunctionDeclarationStatementNode*> functionScope{mCurrentFunction, nullptr};

    // Process the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    // Like for functions, we also expect a return here.
    if (isMissingOrErrorType(returnType) == false && returnType->mKind != TypeKind::cVoid &&
        bodyEndsWithReturn(node->mBody) == false)
    {
        mCtx.report<cMissingLambdaReturn>(node->mSourceRange, typeToString(returnType));
    }

    return true;
}

bool TypeCheckVisitor::visitParamDeclaration(ParamDeclarationNode* node)
{
    // Visit the type specifier.
    if (visit(node->mTypeSpec) == false)
    {
        return false;
    }

    // Parameters need a value type.
    node->mTypeSpec->mType =
        requireValueType(node->mTypeSpec->mType, node->mTypeSpec->mSourceRange, "a parameter type");
    node->mSymbol->mType = node->mTypeSpec->mType;

    return true;
}

bool TypeCheckVisitor::visitNamedTypeSpecifier(NamedTypeSpecifierNode* node)
{
    // Resolve all the type args.
    for (TypeSpecifierNode* arg : node->mTypeArgs)
    {
        if (visit(arg) == false)
        {
            return false;
        }
    }

    // Check whether this is a list.
    if (node->mType == nullptr && isIdentifierExpressionNamed(node->mNameExpression, cListTypeName))
    {
        if (node->mTypeArgs.size() != 1)
        {
            return true;
        }

        Type* elementType = node->mTypeArgs[0]->mType;
        if (elementType == nullptr)
        {
            node->mType = getErrorType();
            return true;
        }

        node->mType = mCtx.mTypes.getOrAddList(elementType);
    }

    // Check whether this is a map.
    if (node->mType == nullptr && isIdentifierExpressionNamed(node->mNameExpression, cMapTypeName))
    {
        if (node->mTypeArgs.size() != 2)
        {
            return true;
        }

        Type* keyType = node->mTypeArgs[0]->mType;
        Type* valueType = node->mTypeArgs[1]->mType;
        if (keyType == nullptr || valueType == nullptr)
        {
            node->mType = getErrorType();
            return true;
        }

        node->mType = mCtx.mTypes.getOrAddMap(keyType, valueType);
    }

    if (node->mType == nullptr)
    {
        return true;
    }

    if (node->mType->mKind == TypeKind::cList)
    {
        // Check that the element type can be stored as a value.
        auto* listType = static_cast<ListType*>(node->mType);
        Type* elementType =
            requireValueType(listType->mElement, node->mTypeArgs[0]->mSourceRange, "a list element type");
        if (isMissingOrErrorType(elementType))
        {
            node->mType = getErrorType();
            return true;
        }

        // It also has to be usable as a list element type.
        if (isSupportedCollectionValueType(elementType) == false)
        {
            mCtx.report<cInvalidTypeInContext>(node->mTypeArgs[0]->mSourceRange,
                                               typeToString(elementType),
                                               "a list element type");
            node->mType = getErrorType();
            return true;
        }

        return true;
    }

    if (node->mType->mKind == TypeKind::cMap)
    {
        auto* mapType = static_cast<MapType*>(node->mType);
        Type* keyType = requireValueType(mapType->mKey, node->mTypeArgs[0]->mSourceRange, "a map key type");
        Type* valueType = requireValueType(mapType->mValue, node->mTypeArgs[1]->mSourceRange, "a map value type");
        if (isMissingOrErrorType(keyType) || isMissingOrErrorType(valueType))
        {
            node->mType = getErrorType();
            return true;
        }

        if (isSupportedCollectionValueType(keyType) == false)
        {
            mCtx.report<cInvalidTypeInContext>(node->mTypeArgs[0]->mSourceRange,
                                               typeToString(keyType),
                                               "a map key type");
            node->mType = getErrorType();
            return true;
        }

        if (isSupportedCollectionValueType(valueType) == false)
        {
            mCtx.report<cInvalidTypeInContext>(node->mTypeArgs[1]->mSourceRange,
                                               typeToString(valueType),
                                               "a map value type");
            node->mType = getErrorType();
            return true;
        }

        return true;
    }

    return true;
}

bool TypeCheckVisitor::visitFunctionTypeSpecifier(FunctionTypeSpecifierNode* node)
{
    std::vector<FunctionParam> paramTypes;
    paramTypes.reserve(node->mParams.size());

    bool valid = true;

    // Check all params.
    for (FunctionTypeParameterSpecifier& param : node->mParams)
    {
        TypeSpecifierNode* typeSpecifier = param.mTypeSpecifier;
        if (visit(typeSpecifier) == false)
        {
            return false;
        }

        // This cannot be null or void.
        Type* paramType =
            requireValueType(typeSpecifier->mType, typeSpecifier->mSourceRange, "a function-type parameter");
        typeSpecifier->mType = paramType;

        if (isMissingOrErrorType(paramType))
        {
            valid = false;
        }

        paramTypes.push_back(FunctionParam{paramType, param.mIsInOut});
    }

    // Resolve and validate the return type.
    Type* returnType = mCtx.mTypes.getVoidType();
    TypeSpecifierNode* returnTypeSpecifier = node->mReturnTypeSpecifier;
    if (returnTypeSpecifier != nullptr)
    {
        if (visit(returnTypeSpecifier) == false)
        {
            return false;
        }

        // The return type cannot be null or empty.
        returnType = requireNonNullType(returnTypeSpecifier->mType,
                                        returnTypeSpecifier->mSourceRange,
                                        "a function-type return type");
        returnTypeSpecifier->mType = returnType;
    }

    if (isMissingOrErrorType(returnType))
    {
        valid = false;
    }

    if (valid == false)
    {
        // If we failed, don't add a new function type.
        node->mType = getErrorType();
    }
    else
    {
        node->mType = mCtx.mTypes.getOrAddFunction(returnType, paramTypes);
    }

    return true;
}

bool TypeCheckVisitor::visitIfBranchStatement(IfBranchStatementNode* node)
{
    // Resolve the condition.
    if (visit(node->mCondition) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mCondition))
    {
        // Check if the condition is a bool.
        requireBoolExpression(node->mCondition);
    }

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool TypeCheckVisitor::visitForStatement(ForStatementNode* node)
{
    auto breakScope = enterBreakContext();
    auto continueScope = enterContinueContext();

    // Visit the init statement.
    if (node->mInit != nullptr && visit(node->mInit) == false)
    {
        return false;
    }

    // Resolve the condition expression.
    if (node->mCondition != nullptr)
    {
        if (visit(node->mCondition) == false)
        {
            return false;
        }

        // Check if the condition is a bool.
        requireBoolExpression(node->mCondition);
    }

    // Visit the increment statement.
    if (node->mIncrement != nullptr && visit(node->mIncrement) == false)
    {
        return false;
    }

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool TypeCheckVisitor::visitWhileStatement(WhileStatementNode* node)
{
    // Resolve the condition.
    if (visit(node->mCondition) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mCondition))
    {
        // Check if the condition is a bool.
        requireBoolExpression(node->mCondition);
    }

    auto breakScope = enterBreakContext();
    auto continueScope = enterContinueContext();

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool TypeCheckVisitor::visitSwitchStatement(SwitchStatementNode* node)
{
    // Resolve the expression to switch over.
    if (visit(node->mExpression) == false)
    {
        return false;
    }

    // Require it to be int.
    Type* intType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
    if (hasValidResolvedType(node->mExpression) && node->mExpression->mResolvedType != intType)
    {
        mCtx.report<cTypeMismatch>(node->mExpression->mSourceRange,
                                   typeToString(intType),
                                   typeToString(node->mExpression->mResolvedType));
    }

    // Enter a new context where we can break out of.
    auto breakScope = enterBreakContext();

    bool hasDefault = false;

    for (SwitchSectionStatementNode* section : node->mSections)
    {
        // No case expression means this is the (or a) default case.
        if (section->mCaseExpression == nullptr)
        {
            if (hasDefault)
            {
                mCtx.report<cDuplicateDefaultLabel>(section->mSourceRange);
            }

            hasDefault = true;
        }
        else
        {
            // Resolve the case expression.
            if (visit(section->mCaseExpression) == false)
            {
                return false;
            }

            if (hasValidResolvedType(section->mCaseExpression))
            {
                // The resolved type has to be int.
                if (section->mCaseExpression->mResolvedType != intType)
                {
                    mCtx.report<cTypeMismatch>(section->mCaseExpression->mSourceRange,
                                               typeToString(intType),
                                               typeToString(section->mCaseExpression->mResolvedType));
                }
            }
        }

        if (visit(section->mBody) == false)
        {
            return false;
        }
    }

    return true;
}

bool TypeCheckVisitor::visitReturnStatement(ReturnStatementNode* node)
{
    // Resolve all return expressions that we may have.
    for (ExpressionNode* expr : node->mExpressions)
    {
        if (visit(expr) == false)
        {
            return false;
        }
    }

    // If our current (enclosing) return type isn't valid, bail here.
    if (isMissingOrErrorType(mCurrentReturnType))
    {
        return true;
    }

    if (node->mExpressions.size() == 1)
    {
        // A single expression has one value or forwards an entire result list.
        // E.g., return 3 or return pair().
        Type* actual = node->mExpressions[0]->mResolvedType;
        if (isMissingOrErrorType(actual))
        {
            return true;
        }

        if (mCurrentReturnType->mKind == TypeKind::cMultiReturn && actual->mKind != TypeKind::cMultiReturn)
        {
            // If a multi-return is expected and we don't have one, that's an error.
            mCtx.report<cResultCountMismatch>(node->mSourceRange,
                                              static_cast<MultiReturnType*>(mCurrentReturnType)->mTypes.size(),
                                              node->mExpressions.size());
        }
        else
        {
            // Otherwise, check if we're allowed to return this type.
            convertExpressionToType(node->mExpressions[0], mCurrentReturnType);
        }

        return true;
    }

    // If we're here, we have something like return or return 3, 4.
    ArrayView expected{&mCurrentReturnType, 1};
    // Note that if we're not taking either of these branches, this is going to be an error.
    // (Having 1 valid return expression would have been handled in the branch above.)
    if (mCurrentReturnType->mKind == TypeKind::cMultiReturn)
    {
        // If we want multiple return types, use these instead.
        expected = static_cast<MultiReturnType*>(mCurrentReturnType)->mTypes;
    }
    else if (mCurrentReturnType->mKind == TypeKind::cVoid)
    {
        // A no-result function expects no return expressions.
        expected = {};
    }

    // Check for size match.
    if (node->mExpressions.size() != expected.size())
    {
        mCtx.report<cResultCountMismatch>(node->mSourceRange, expected.size(), node->mExpressions.size());
        return true;
    }

    // Make sure all the expression types match the expected types.
    for (usize i = 0; i < expected.size(); ++i)
    {
        convertExpressionToType(node->mExpressions[i], expected[i]);
    }

    return true;
}

bool TypeCheckVisitor::visitBreakStatement(BreakStatementNode* node)
{
    if (mBreakContextDepth <= 0)
    {
        mCtx.report<cBreakOutsideLoopOrSwitch>(node->mSourceRange);
        return true;
    }

    return true;
}

bool TypeCheckVisitor::visitContinueStatement(ContinueStatementNode* node)
{
    if (mContinueContextDepth <= 0)
    {
        mCtx.report<cContinueOutsideLoop>(node->mSourceRange);
        return true;
    }

    return true;
}

bool TypeCheckVisitor::visitPrintStatement(PrintStatementNode* node)
{
    // Resolve the expression.
    if (visit(node->mExpression) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mExpression) == false)
    {
        return true;
    }

    // Check if the expression is printable.
    if (requirePrintablePrimitive(node->mExpression) == false)
    {
        return true;
    }

    return true;
}

} // namespace simlang
