#include "ast/assignmentop.h"
#include "ast/binaryop.h"
#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/unaryop.h"
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
#include "util/types.h"

namespace simlang
{

class ArenaAllocator;

static bool canAssignNull(Type* type)
{
    if (type == nullptr)
    {
        return false;
    }

    // Ref types (class, list, map, interface) allow null, the rest doesn't.
    return type->mKind == TypeKind::cClass || type->mKind == TypeKind::cList || type->mKind == TypeKind::cMap ||
           type->mKind == TypeKind::cInterface;
}

static bool isNumericPrimitive(PrimitiveTypeKind kind)
{
    return kind == PrimitiveTypeKind::cInt || kind == PrimitiveTypeKind::cFloat;
}

static bool isConstExprZero(ExpressionNode* expr, ArenaAllocator& allocator)
{
    // Check for constexpr (duh).
    if (expr->mFlags.test(cExprIsConstExpr) == false)
    {
        return false;
    }

    // Evaluate it and check what we got.
    ConstValue value;
    ConstEvalVisitor evaluator{allocator};
    if (evaluator.evaluate(expr, value) == false || value.mKind != ConstValueKind::cPrimitive)
    {
        return false;
    }

    if (value.mPrimitiveKind == PrimitiveTypeKind::cInt)
    {
        return value.as.mInteger == 0;
    }

    if (value.mPrimitiveKind == PrimitiveTypeKind::cFloat)
    {
        return value.as.mFloat == 0.0f;
    }

    return false;
}

bool TypeCheckVisitor::allowImplicitCast(Type* fromType, Type* toType)
{
    if (isErrorType(fromType) || isErrorType(toType))
    {
        return false;
    }

    // Same type is always allowed.
    if (fromType == toType)
    {
        return true;
    }

    // Nullability.
    if (fromType->mKind == TypeKind::cNull && canAssignNull(toType))
    {
        return true;
    }

    // Class -> interface casts have to be checked more carefully.
    if (fromType->mKind == TypeKind::cClass && toType->mKind == TypeKind::cInterface)
    {
        return allowClassToInterfaceCast(fromType, static_cast<InterfaceType*>(toType));
    }

    // Other implicit casts require primitives.
    if (fromType->mKind != TypeKind::cPrimitive || toType->mKind != TypeKind::cPrimitive)
    {
        return false;
    }

    // Otherwise, we only allow implicit int to float.
    PrimitiveTypeKind fromKind = getPrimitiveKind(fromType);
    PrimitiveTypeKind toKind = getPrimitiveKind(toType);
    if (fromKind == PrimitiveTypeKind::cInt && toKind == PrimitiveTypeKind::cFloat)
    {
        return true;
    }

    return false;
}

bool TypeCheckVisitor::allowExplicitCast(Type* fromType, Type* toType)
{
    if (isErrorType(fromType) || isErrorType(toType))
    {
        return false;
    }

    // Same type is always allowed.
    if (fromType == toType || allowImplicitCast(fromType, toType))
    {
        return true;
    }

    // Interface -> class casts have to be checked more carefully.
    if (fromType->mKind == TypeKind::cInterface && toType->mKind == TypeKind::cClass)
    {
        return allowClassToInterfaceCast(toType, static_cast<InterfaceType*>(fromType));
    }

    // Otherwise, we need primitives.
    if (fromType->mKind != TypeKind::cPrimitive || toType->mKind != TypeKind::cPrimitive)
    {
        return false;
    }
    PrimitiveTypeKind fromKind = getPrimitiveKind(fromType);
    PrimitiveTypeKind toKind = getPrimitiveKind(toType);
    if (fromKind == toKind)
    {
        // Invalid stuff should always return false.
        return fromKind != PrimitiveTypeKind::cInvalid;
    }

    // int <-> float is always allowed.
    if (isNumericPrimitive(fromKind) && isNumericPrimitive(toKind))
    {
        return true;
    }

    // bool -> int/float.
    if (fromKind == PrimitiveTypeKind::cBool && isNumericPrimitive(toKind))
    {
        return true;
    }

    // int/float -> bool.
    if (isNumericPrimitive(fromKind) && toKind == PrimitiveTypeKind::cBool)
    {
        return true;
    }

    return false;
}

ImplicitCastNode* TypeCheckVisitor::createImplicitCast(ExpressionNode* node, Type* targetType) const
{
    bool isConstExpr = node->mFlags.test(cExprIsConstExpr);

    auto* castNode = mCtx.allocate<ImplicitCastNode>(node->mSourceRange, node);
    castNode->mResolvedType = targetType;
    castNode->mFlags.set(cExprIsConstExpr, isConstExpr);

    return castNode;
}

bool TypeCheckVisitor::convertExpressionToType(ExpressionNode*& expr, Type* targetType)
{
    if (isErrorType(expr->mResolvedType) || isErrorType(targetType))
    {
        return true;
    }

    // If we already have the target type, do nothing.
    if (expr->mResolvedType == targetType)
    {
        return true;
    }

    // Try an implicit cast.
    if (allowImplicitCast(expr->mResolvedType, targetType))
    {
        expr = createImplicitCast(expr, targetType);
        return true;
    }

    mCtx.report<cTypeMismatch>(expr->mSourceRange, typeToString(targetType), typeToString(expr->mResolvedType));
    return false;
}

bool TypeCheckVisitor::convertArgumentToParameterType(ExpressionNode*& expr, Type* targetType, usize argIndex)
{
    if (isErrorType(expr->mResolvedType) || isErrorType(targetType))
    {
        return true;
    }

    // If we already have the target type, do nothing.
    if (expr->mResolvedType == targetType)
    {
        return true;
    }

    // Try an implicit cast.
    if (allowImplicitCast(expr->mResolvedType, targetType))
    {
        expr = createImplicitCast(expr, targetType);
        return true;
    }

    mCtx.report<cInvalidArgumentType>(expr->mSourceRange,
                                      argIndex + 1,
                                      typeToString(targetType),
                                      typeToString(expr->mResolvedType));
    return false;
}

Symbol* TypeCheckVisitor::getInitializerAssignedField(ExpressionNode* lhs) const
{
    // This has to be a member access node.
    if (lhs->mNodeType != NodeType::cMemberAccess)
    {
        return nullptr;
    }

    // The receiver has to be a 'this' node.
    auto* memberAccess = static_cast<MemberAccessNode*>(lhs);
    if (memberAccess->mReceiver->mNodeType != NodeType::cThis)
    {
        return nullptr;
    }

    // Get the symbol for the member access.
    Symbol* symbol = memberAccess->mSymbol;
    if (symbol == nullptr || symbol->mSymbolType != SymbolType::cMemberVariable)
    {
        return nullptr;
    }

    return symbol;
}

bool TypeCheckVisitor::isInitializerFieldAssignment(AssignmentStatementNode* node) const
{
    // Here, we want to find out if an assignment statement comes before the first non-assignment in an initializer.
    // If we have no function or we are not in an initializer, bail.
    if (mCurrentFunction == nullptr || mCurrentFunction->mIsInitMethod == false)
    {
        return false;
    }

    // Get the symbol we're assigning to.
    Symbol* assignedField = getInitializerAssignedField(node->mLHS);
    if (assignedField == nullptr)
    {
        return false;
    }

    // That must be mutable.
    if (assignedField->mFlags.test(SymbolFlags::cMutable))
    {
        return false;
    }

    auto* body = static_cast<BlockStatementNode*>(mCurrentFunction->mBody);
    for (StatementNode* stmt : body->mStatements)
    {
        // On encountering a non-assignment statement, we're done.
        if (stmt->mNodeType != NodeType::cAssignmentStatement)
        {
            return false;
        }

        // Otherwise, make sure it's a '=' assignment and it assigns to a member.
        auto* leadingAssignment = static_cast<AssignmentStatementNode*>(stmt);
        if (leadingAssignment->mOp != AssignmentOp::cAss ||
            getInitializerAssignedField(leadingAssignment->mLHS) == nullptr)
        {
            return false;
        }

        // If we found our target and all the previous ones were valid, we're done.
        if (stmt == node)
        {
            return true;
        }
    }

    return false;
}

bool TypeCheckVisitor::requireAssignableLValue(ExpressionNode* expr, bool allowImmutable) const
{
    // Error types are always assignable (?).
    if (isErrorType(expr->mResolvedType))
    {
        return true;
    }

    // Check the lvalue flag.
    if (expr->mFlags.test(cExprIsLValue) == false)
    {
        mCtx.report<cNotAnLValue>(expr->mSourceRange);
        return false;
    }

    // Check for mutability if requested.
    if (expr->mFlags.test(cExprIsMutable) == false && allowImmutable == false)
    {
        mCtx.report<cNotMutable>(expr->mSourceRange);
        return false;
    }

    return true;
}

bool TypeCheckVisitor::checkMemberAccess(Symbol* symbol, Symbol* ownerTypeSymbol) const
{
    // If the member is not private, it is accessible.
    if (symbol->mFlags.test(SymbolFlags::cPrivate) == false)
    {
        return true;
    }

    // Otherwise, check whether we're in the same module when trying to access a private member.
    auto* ownerDecl = static_cast<TypeDeclarationStatementNode*>(ownerTypeSymbol->mDeclNode);
    return ownerDecl->mDeclModule == mCurrentModule;
}

bool TypeCheckVisitor::requireBoolExpression(ExpressionNode* expr) const
{
    if (isErrorType(expr->mResolvedType))
    {
        return true;
    }

    Type* boolType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
    if (expr->mResolvedType == boolType)
    {
        return true;
    }

    mCtx.report<cTypeMismatch>(expr->mSourceRange, typeToString(boolType), typeToString(expr->mResolvedType));

    return false;
}

bool TypeCheckVisitor::requireNonVoidPrimitive(ExpressionNode* expr) const
{
    if (isErrorType(expr->mResolvedType))
    {
        return true;
    }

    if (expr->mResolvedType->mKind == TypeKind::cPrimitive &&
        getPrimitiveKind(expr->mResolvedType) != PrimitiveTypeKind::cVoid)
    {
        return true;
    }

    mCtx.report<cNotPrintable>(expr->mSourceRange, typeToString(expr->mResolvedType));

    return false;
}

void TypeCheckVisitor::diagnoseInvalidBinaryOperands(BinaryOpNode* node) const
{
    Type* leftType = node->mLeft->mResolvedType;
    Type* rightType = node->mRight->mResolvedType;

    mCtx.report<cInvalidBinaryOperands>(node->mSourceRange,
                                        binaryOpToString(node->mOp),
                                        typeToString(leftType),
                                        typeToString(rightType));
    markError(node);
}

void TypeCheckVisitor::diagnoseInvalidAssignmentOperands(AssignmentStatementNode* node) const
{
    Type* leftType = node->mLHS->mResolvedType;
    Type* rightType = node->mRHS->mResolvedType;

    mCtx.report<cInvalidAssignmentOperands>(node->mSourceRange,
                                            assignmentOpToString(node->mOp),
                                            typeToString(rightType),
                                            typeToString(leftType));
}

Type* TypeCheckVisitor::promoteIntFloatOperands(BinaryOpNode* node)
{
    // Expect these to be checked.
    ExpressionNode* lhs = node->mLeft;
    ExpressionNode* rhs = node->mRight;

    if (hasValidResolvedType(lhs) == false || hasValidResolvedType(rhs) == false)
    {
        return nullptr;
    }

    PrimitiveTypeKind lhsKind = getPrimitiveKind(lhs->mResolvedType);
    PrimitiveTypeKind rhsKind = getPrimitiveKind(rhs->mResolvedType);

    // Only allow this for ints and floats.
    if (isNumericPrimitive(lhsKind) == false)
    {
        diagnoseInvalidBinaryOperands(node);
        return nullptr;
    }

    if (isNumericPrimitive(rhsKind) == false)
    {
        diagnoseInvalidBinaryOperands(node);
        return nullptr;
    }

    Type* resolvedType = lhs->mResolvedType;

    // lhs int, rhs float.
    if ((lhsKind == PrimitiveTypeKind::cInt) && (rhsKind == PrimitiveTypeKind::cFloat))
    {
        // Promote lhs to float and return that type.
        node->mLeft = createImplicitCast(lhs, rhs->mResolvedType);

        // Use the float type from rhs as resolved type.
        resolvedType = rhs->mResolvedType;
    }
    // lhs float, rhs int.
    else if ((lhsKind == PrimitiveTypeKind::cFloat) && (rhsKind == PrimitiveTypeKind::cInt))
    {
        // Promote rhs to float and return that type.
        node->mRight = createImplicitCast(rhs, lhs->mResolvedType);

        // Use the float type from lhs as resolved type.
        resolvedType = lhs->mResolvedType;
    }

    return resolvedType;
}

void TypeCheckVisitor::resolveBinaryMathOps(BinaryOpNode* node)
{
    node->mResolvedType = promoteIntFloatOperands(node);
    if (node->mResolvedType == nullptr)
    {
        markError(node);
        return;
    }

    if ((node->mOp == BinaryOp::cDiv || node->mOp == BinaryOp::cMod) && isConstExprZero(node->mRight, mCtx.mAllocator))
    {
        mCtx.report<cDivisionByZero>(node->mRight->mSourceRange, binaryOpToString(node->mOp));
        markError(node);
    }
}

void TypeCheckVisitor::resolveBinaryEqualityOps(BinaryOpNode* node)
{
    // Expect these to be checked.
    ExpressionNode* lhs = node->mLeft;
    ExpressionNode* rhs = node->mRight;

    if (hasValidResolvedType(lhs) == false || hasValidResolvedType(rhs) == false)
    {
        markError(node);
        return;
    }

    TypeKind lhsKind = lhs->mResolvedType->mKind;
    TypeKind rhsKind = rhs->mResolvedType->mKind;
    bool isEqualityOp = node->mOp == BinaryOp::cEQ || node->mOp == BinaryOp::cNE;

    if (lhsKind == TypeKind::cNull && rhsKind == TypeKind::cNull)
    {
        if (isEqualityOp)
        {
            node->mResolvedType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
            return;
        }

        diagnoseInvalidBinaryOperands(node);

        return;
    }

    // Null/reference comparisons only support == and !=.
    if ((lhsKind == TypeKind::cNull && canAssignNull(rhs->mResolvedType)) ||
        (rhsKind == TypeKind::cNull && canAssignNull(lhs->mResolvedType)))
    {
        if (isEqualityOp)
        {
            node->mResolvedType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
            return;
        }

        diagnoseInvalidBinaryOperands(node);

        return;
    }

    // Non-primitive stuff.
    if (lhsKind != TypeKind::cPrimitive || rhsKind != TypeKind::cPrimitive)
    {
        bool isComparableNonPrimitive = lhsKind == TypeKind::cClass || lhsKind == TypeKind::cList ||
                                        lhsKind == TypeKind::cMap || lhsKind == TypeKind::cStruct ||
                                        lhsKind == TypeKind::cInterface;
        if (lhs->mResolvedType != rhs->mResolvedType || isEqualityOp == false || isComparableNonPrimitive == false)
        {
            diagnoseInvalidBinaryOperands(node);
            return;
        }

        node->mResolvedType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);

        return;
    }

    PrimitiveTypeKind lhsPtk = getPrimitiveKind(lhs->mResolvedType);
    PrimitiveTypeKind rhsPtk = getPrimitiveKind(rhs->mResolvedType);

    // Invalid and void values are never comparable.
    if (lhsPtk == PrimitiveTypeKind::cInvalid || rhsPtk == PrimitiveTypeKind::cInvalid ||
        lhsPtk == PrimitiveTypeKind::cVoid || rhsPtk == PrimitiveTypeKind::cVoid)
    {
        diagnoseInvalidBinaryOperands(node);
        return;
    }

    if ((lhsPtk == PrimitiveTypeKind::cInt && rhsPtk == PrimitiveTypeKind::cFloat) ||
        (lhsPtk == PrimitiveTypeKind::cFloat && rhsPtk == PrimitiveTypeKind::cInt))
    {
        // Promote the int to float.
        Type* promotedType = promoteIntFloatOperands(node);
        if (promotedType == nullptr)
        {
            markError(node);
            return;
        }
    }
    else if (lhsPtk == rhsPtk)
    {
        if (isEqualityOp == false && isNumericPrimitive(lhsPtk) == false)
        {
            diagnoseInvalidBinaryOperands(node);
            return;
        }
    }
    else
    {
        diagnoseInvalidBinaryOperands(node);
        return;
    }

    // The resolved type is a bool.
    node->mResolvedType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
}

void TypeCheckVisitor::resolveBinaryBitOps(BinaryOpNode* node) const
{
    // Expect these to be checked.
    ExpressionNode* lhs = node->mLeft;
    ExpressionNode* rhs = node->mRight;

    if (hasValidResolvedType(lhs) == false || hasValidResolvedType(rhs) == false)
    {
        markError(node);
        return;
    }

    PrimitiveTypeKind lhsKind = getPrimitiveKind(lhs->mResolvedType);
    PrimitiveTypeKind rhsKind = getPrimitiveKind(rhs->mResolvedType);
    if (lhsKind != PrimitiveTypeKind::cInt || rhsKind != PrimitiveTypeKind::cInt)
    {
        diagnoseInvalidBinaryOperands(node);
        return;
    }

    // Make sure << and >> are within bounds if we have a constexpr.
    if (node->mOp == BinaryOp::cShiftL || node->mOp == BinaryOp::cShiftR)
    {
        ConstValue shiftCount;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        if (rhs->mFlags.test(cExprIsConstExpr) && evaluator.evaluate(rhs, shiftCount) &&
            (shiftCount.as.mInteger < 0 || shiftCount.as.mInteger >= 32))
        {
            mCtx.report<cShiftCountOutOfRange>(rhs->mSourceRange, binaryOpToString(node->mOp), shiftCount.as.mInteger);
            markError(node);
            return;
        }
    }

    // If we got here, both are ints and so is the result.
    node->mResolvedType = lhs->mResolvedType;
}

// Control-flow scopes.

bool TypeCheckVisitor::visitUnaryOp(UnaryOpNode* node)
{
    // Resolve the node.
    if (visit(node->mExpr) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mExpr) == false)
    {
        markError(node);
        return true;
    }

    Type* resolvedType = node->mExpr->mResolvedType;

    switch (node->mOp)
    {
        // Unary '-' for ints and floats, '~'
        case UnaryOp::cNeg:
        {
            if (isNumericPrimitive(getPrimitiveKind(resolvedType)))
            {
                node->mResolvedType = resolvedType;
            }
            else
            {
                mCtx.report<cInvalidOperandType>(node->mSourceRange, "-", typeToString(resolvedType));
                markError(node);
                return true;
            }
            break;
        }
        case UnaryOp::cBitNot:
        {
            if (getPrimitiveKind(resolvedType) == PrimitiveTypeKind::cInt)
            {
                node->mResolvedType = resolvedType;
            }
            else
            {
                mCtx.report<cInvalidOperandType>(node->mSourceRange, "~", typeToString(resolvedType));
                markError(node);
                return true;
            }
            break;
        }
        default:
        {
            markError(node);
            return true;
        }
    }

    // Constexpr check.
    bool isConstExpr = node->mExpr->mFlags.test(cExprIsConstExpr);
    node->mFlags.set(cExprIsConstExpr, isConstExpr);

    return true;
}

bool TypeCheckVisitor::visitBinaryOp(BinaryOpNode* node)
{
    // Resolve the lhs and rhs.
    if (visit(node->mLeft) == false || visit(node->mRight) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mLeft) == false || hasValidResolvedType(node->mRight) == false)
    {
        markError(node);
        return true;
    }

    switch (node->mOp)
    {
        case BinaryOp::cAdd:
        case BinaryOp::cSub:
        case BinaryOp::cMul:
        case BinaryOp::cDiv:
        case BinaryOp::cMod:
        {
            resolveBinaryMathOps(node);
            break;
        }
        case BinaryOp::cLT:
        case BinaryOp::cLE:
        case BinaryOp::cGT:
        case BinaryOp::cGE:
        case BinaryOp::cEQ:
        case BinaryOp::cNE:
        {
            resolveBinaryEqualityOps(node);
            break;
        }
        case BinaryOp::cBitAnd:
        case BinaryOp::cBitOr:
        case BinaryOp::cBitXor:
        case BinaryOp::cShiftR:
        case BinaryOp::cShiftL:
        {
            resolveBinaryBitOps(node);
            break;
        }
        case BinaryOp::cOr:
        case BinaryOp::cAnd:
        {
            PrimitiveTypeKind lhsKind = getPrimitiveKind(node->mLeft->mResolvedType);
            PrimitiveTypeKind rhsKind = getPrimitiveKind(node->mRight->mResolvedType);

            if (lhsKind != PrimitiveTypeKind::cBool || rhsKind != PrimitiveTypeKind::cBool)
            {
                diagnoseInvalidBinaryOperands(node);
                return true;
            }

            node->mResolvedType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);

            break;
        }
        default:
        {
            diagnoseInvalidBinaryOperands(node);
            return true;
        }
    }

    if (hasValidResolvedType(node) == false)
    {
        return true;
    }

    bool lhsConst = node->mLeft->mFlags.test(cExprIsConstExpr);
    bool rhsConst = node->mRight->mFlags.test(cExprIsConstExpr);
    node->mFlags.set(cExprIsConstExpr, lhsConst && rhsConst);

    return true;
}

bool TypeCheckVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    if (visit(node->mCondition) == false || visit(node->mThenExpr) == false || visit(node->mElseExpr) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mCondition) == false || hasValidResolvedType(node->mThenExpr) == false ||
        hasValidResolvedType(node->mElseExpr) == false)
    {
        markError(node);
        return true;
    }

    if (requireBoolExpression(node->mCondition) == false)
    {
        markError(node);
        return true;
    }

    ExpressionNode*& thenExpr = node->mThenExpr;
    ExpressionNode*& elseExpr = node->mElseExpr;

    Type* thenType = thenExpr->mResolvedType;
    Type* elseType = elseExpr->mResolvedType;

    if (thenType == elseType)
    {
        node->mResolvedType = thenType;
    }
    else
    {
        PrimitiveTypeKind thenKind = getPrimitiveKind(thenType);
        PrimitiveTypeKind elseKind = getPrimitiveKind(elseType);

        if ((thenKind == PrimitiveTypeKind::cInt && elseKind == PrimitiveTypeKind::cFloat) ||
            (thenKind == PrimitiveTypeKind::cFloat && elseKind == PrimitiveTypeKind::cInt))
        {
            Type* commonType = (thenKind == PrimitiveTypeKind::cInt) ? elseType : thenType;
            if (thenKind == PrimitiveTypeKind::cInt)
            {
                if (convertExpressionToType(thenExpr, commonType) == false)
                {
                    markError(node);
                    return true;
                }
            }
            else
            {
                if (convertExpressionToType(elseExpr, commonType) == false)
                {
                    markError(node);
                    return true;
                }
            }

            node->mResolvedType = commonType;
        }
        else if (allowImplicitCast(thenType, elseType) && allowImplicitCast(elseType, thenType) == false)
        {
            if (convertExpressionToType(thenExpr, elseType) == false)
            {
                markError(node);
                return true;
            }

            node->mResolvedType = elseType;
        }
        else if (allowImplicitCast(elseType, thenType) && allowImplicitCast(thenType, elseType) == false)
        {
            if (convertExpressionToType(elseExpr, thenType) == false)
            {
                markError(node);
                return true;
            }

            node->mResolvedType = thenType;
        }
        else
        {
            mCtx.report<cTypeMismatch>(elseExpr->mSourceRange, typeToString(thenType), typeToString(elseType));
            markError(node);
            return true;
        }
    }

    if (getPrimitiveKind(node->mResolvedType) == PrimitiveTypeKind::cVoid)
    {
        mCtx.report<cInvalidTypeInContext>(node->mSourceRange,
                                           typeToString(node->mResolvedType),
                                           "a ternary expression result");
        markError(node);
        return true;
    }

    ConstEvalVisitor cev{mCtx.mAllocator};
    ConstValue cv;
    node->mFlags.set(cExprIsConstExpr, cev.evaluate(node, cv));

    return true;
}

bool TypeCheckVisitor::visitAssignmentStatement(AssignmentStatementNode* node)
{
    // Resolve the LHS first.
    if (visit(node->mLHS) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mLHS) == false)
    {
        return true;
    }

    // We allow const values to be assigned to in the initializer.
    // But (for now) only, if they appear before the first non-assignment statement.
    // We allow immutability only if it is one an initializer assignment.
    bool allowInitializerFieldAssignment = isInitializerFieldAssignment(node);
    if (requireAssignableLValue(node->mLHS, allowInitializerFieldAssignment) == false)
    {
        return true;
    }

    // The LHS is actually used as LValue here.
    markExpressionUsedAsLValue(node->mLHS);

    // Resolve the RHS.
    if (visit(node->mRHS) == false)
    {
        return false;
    }

    if (hasValidResolvedType(node->mRHS) == false)
    {
        return true;
    }

    PrimitiveTypeKind lhsPTK = getPrimitiveKind(node->mLHS->mResolvedType);

    // If we have anything but '=', the LHS type has to be a primitive.
    if (node->mOp != AssignmentOp::cAss && lhsPTK == PrimitiveTypeKind::cInvalid)
    {
        diagnoseInvalidAssignmentOperands(node);
        return true;
    }

    // Check if the types match and add an implicit cast if needed.
    if (convertExpressionToType(node->mRHS, node->mLHS->mResolvedType) == false)
    {
        return true;
    }

    // If we have '=' and the types match, we're done.
    if (node->mOp == AssignmentOp::cAss)
    {
        return true;
    }

    // Otherwise,
    PrimitiveTypeKind rhsPTK = getPrimitiveKind(node->mRHS->mResolvedType);
    switch (node->mOp)
    {
        case AssignmentOp::cAssAdd:
        case AssignmentOp::cAssSub:
        case AssignmentOp::cAssMul:
        case AssignmentOp::cAssDiv:
        case AssignmentOp::cAssMod:
        {
            if (isNumericPrimitive(lhsPTK) && isNumericPrimitive(rhsPTK))
            {
                if ((node->mOp == AssignmentOp::cAssDiv || node->mOp == AssignmentOp::cAssMod) &&
                    isConstExprZero(node->mRHS, mCtx.mAllocator))
                {
                    mCtx.report<cDivisionByZero>(node->mRHS->mSourceRange, assignmentOpToString(node->mOp));
                }

                return true;
            }
            break;
        }
        case AssignmentOp::cAssAnd:
        case AssignmentOp::cAssOr:
        case AssignmentOp::cAssXor:
        case AssignmentOp::cAssShl:
        case AssignmentOp::cAssShr:
        {
            if (lhsPTK != PrimitiveTypeKind::cInt || rhsPTK != PrimitiveTypeKind::cInt)
            {
                break;
            }

            // If we have a shift, check whether we can (in)validate it at compile time.
            if ((node->mOp == AssignmentOp::cAssShl || node->mOp == AssignmentOp::cAssShr) &&
                node->mRHS->mFlags.test(cExprIsConstExpr))
            {
                ConstValue shiftCount;
                ConstEvalVisitor evaluator{mCtx.mAllocator};

                if (evaluator.evaluate(node->mRHS, shiftCount) &&
                    (shiftCount.as.mInteger < 0 || shiftCount.as.mInteger >= 32))
                {
                    mCtx.report<cShiftCountOutOfRange>(node->mRHS->mSourceRange,
                                                       assignmentOpToString(node->mOp),
                                                       shiftCount.as.mInteger);
                }
            }

            return true;
        }
        default:
        {
            break;
        }
    }

    diagnoseInvalidAssignmentOperands(node);

    return true;
}

} // namespace simlang
