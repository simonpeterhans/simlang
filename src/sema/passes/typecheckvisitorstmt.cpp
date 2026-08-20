#include <string_view>
#include <unordered_set>
#include <utility>

#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/constevalvisitor.h"
#include "sema/passes/typecheckvisitor.h"
#include "source/sourcerange.h"
#include "symbol/constvalue.h"
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

static bool isSupportedListElementType(Type* type)
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
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

static bool isSupportedMapKeyType(Type* type)
{
    if (type == nullptr || type->mKind != TypeKind::cPrimitive)
    {
        return false;
    }

    PrimitiveTypeKind kind = getPrimitiveKind(type);
    return kind != PrimitiveTypeKind::cInvalid && kind != PrimitiveTypeKind::cVoid;
}

static bool isSupportedMapValueType(Type* type)
{
    // Currently the same as list types.
    return isSupportedListElementType(type);
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
        if (initType == nullptr)
        {
            mCtx.report<cCannotInferType>(node->mIdentifierRange, node->mIdentifier);
            node->mSymbol->mType = getErrorType();
            return true;
        }

        // Null cannot be implicit.
        if (initType->mKind == TypeKind::cNull)
        {
            mCtx.report<cCannotInferType>(node->mInit->mSourceRange, node->mIdentifier);
            node->mSymbol->mType = getErrorType();
            return true;
        }

        Type* inferredType = requireValueType(initType, node->mInit->mSourceRange, "a variable type");
        if (isErrorType(inferredType))
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

    // Const checks, currently we only allow primitives.
    bool isConstDeclaration = (s->mFlags.test(SymbolFlags::cMutable) == false);
    bool hasValidConstType = true;

    // If we have one, make sure the type is valid.
    if (isConstDeclaration && isErrorType(s->mType) == false)
    {
        hasValidConstType = isValidConstType(s->mType);
        if (hasValidConstType == false)
        {
            mCtx.report<cInvalidConstType>(node->mIdentifierRange, typeToString(s->mType));
        }
    }

    bool hasConstExprInit = node->mInit != nullptr && node->mInit->mFlags.test(cExprIsConstExpr);

    // Global stuff needs a const expr initializer.
    if (s->mSymbolType == SymbolType::cGlobalVariable && node->mInit != nullptr && hasValidResolvedType(node->mInit) &&
        hasConstExprInit == false)
    {
        mCtx.report<cGlobalInitializerNotConstExpr>(node->mInit->mSourceRange, node->mIdentifier);
    }

    // If the variable is const and the expression is const expr, mark the variable as such.
    if (isConstDeclaration && hasValidConstType &&
        (s->mSymbolType == SymbolType::cGlobalVariable || s->mSymbolType == SymbolType::cStackVariable ||
         s->mSymbolType == SymbolType::cParameter) &&
        hasConstExprInit)
    {
        s->mFlags.set(SymbolFlags::cConstExpr, true);
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

    if (node->mBody != nullptr && isErrorType(functionReturnType) == false &&
        getPrimitiveKind(functionReturnType) != PrimitiveTypeKind::cVoid)
    {
        bool endsWithReturn = (node->mBody->mNodeType == NodeType::cReturnStatement);

        // If this is a block, check whether the last statement is a return.
        // For now, this is good enough to enforce.
        if (node->mBody->mNodeType == NodeType::cBlockStatement)
        {
            auto* block = static_cast<BlockStatementNode*>(node->mBody);
            endsWithReturn = block->mStatements.empty() == false &&
                             block->mStatements.back()->mNodeType == NodeType::cReturnStatement;
        }

        if (endsWithReturn == false)
        {
            mCtx.report<cMissingReturnStatement>(node->mIdentifierRange,
                                                 node->mIdentifier,
                                                 typeToString(functionReturnType));
        }
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
        // Check the element type (disallow null/void).
        auto* listType = static_cast<ListType*>(node->mType);
        Type* elementType =
            requireValueType(listType->mElement, node->mTypeArgs[0]->mSourceRange, "a list element type");
        if (isErrorType(elementType))
        {
            node->mType = getErrorType();
            return true;
        }

        // It also has to be usable as a list element type.
        if (isSupportedListElementType(elementType) == false)
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
        if (isErrorType(keyType) || isErrorType(valueType))
        {
            node->mType = getErrorType();
            return true;
        }

        if (isSupportedMapKeyType(keyType) == false)
        {
            mCtx.report<cInvalidTypeInContext>(node->mTypeArgs[0]->mSourceRange,
                                               typeToString(keyType),
                                               "a map key type");
            node->mType = getErrorType();
            return true;
        }

        if (isSupportedMapValueType(valueType) == false)
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

    std::unordered_set<i32> cases;
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
                else
                {
                    ConstValue cv{};
                    ConstEvalVisitor cev{mCtx.mAllocator};

                    if (cev.evaluate(section->mCaseExpression, cv) == false ||
                        cv.mPrimitiveKind != PrimitiveTypeKind::cInt)
                    {
                        // It also has to be constexpr (and the constexpr value has to be int).
                        mCtx.report<cCaseLabelNotConstexpr>(section->mCaseExpression->mSourceRange);
                    }
                    else if (cases.insert(cv.as.mInteger).second == false)
                    {
                        // It also cannot exist already.
                        mCtx.report<cDuplicateCaseLabel>(section->mCaseExpression->mSourceRange, cv.as.mInteger);
                    }
                    else
                    {
                        section->mCaseValue = cv.as.mInteger;
                    }
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
    // Resolve the expression.
    if (node->mExpression != nullptr)
    {
        if (visit(node->mExpression) == false)
        {
            return false;
        }
    }

    // Find out what type we're expected to return.

    Type* returnType = nullptr;
    // If we have no expression, we're returning void.
    if (node->mExpression == nullptr)
    {
        returnType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cVoid);
    }
    else
    {
        returnType = node->mExpression->mResolvedType;
    }

    if (isErrorType(returnType) || isErrorType(mCurrentReturnType))
    {
        return true;
    }

    // Check if the expected type and the provided type match.
    if (returnType != mCurrentReturnType)
    {
        if (node->mExpression != nullptr)
        {
            if (convertExpressionToType(node->mExpression, mCurrentReturnType) == false)
            {
                return true;
            }

            return true;
        }

        mCtx.report<cTypeMismatch>(node->mSourceRange, typeToString(mCurrentReturnType), typeToString(returnType));
        return true;
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
    if (requireNonVoidPrimitive(node->mExpression) == false)
    {
        return true;
    }

    return true;
}

} // namespace simlang
