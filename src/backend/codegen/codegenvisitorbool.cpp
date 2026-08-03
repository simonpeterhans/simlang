#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "backend/codegen/codegenvisitor.h"
#include "runtime/op/opcode.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/types.h"

namespace simlang
{

static bool isComparisonOp(BinaryOp op)
{
    switch (op)
    {
        case BinaryOp::cEQ:
        case BinaryOp::cNE:
        case BinaryOp::cLT:
        case BinaryOp::cLE:
        case BinaryOp::cGT:
        case BinaryOp::cGE:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

static bool isPrimitiveConversion(ExpressionNode* expr, Type* resultType)
{
    // The expression to cast has to be a primitive type.
    if (getPrimitiveKind(expr->mResolvedType) == PrimitiveTypeKind::cInvalid)
    {
        return false;
    }

    // The resulting type as well.
    return getPrimitiveKind(resultType) != PrimitiveTypeKind::cInvalid;
}

static bool isTrivialOperand(ExpressionNode* operand)
{
    switch (operand->mNodeType)
    {
        case NodeType::cIdentifier:
        case NodeType::cThis:
        case NodeType::cIntLiteral:
        case NodeType::cFloatLiteral:
        case NodeType::cBoolLiteral:
        case NodeType::cStringLiteral:
        case NodeType::cNullLiteral:
        {
            return true;
        }
        case NodeType::cImplicitCast:
        {
            auto* cast = static_cast<ImplicitCastNode*>(operand);

            // If the cast is not primitive, it's not trivial.
            if (isPrimitiveConversion(cast->mTarget, cast->mResolvedType) == false)
            {
                return false;
            }

            // The expression has to be trivial (something like f() is not a trivial operand).
            return isTrivialOperand(cast->mTarget);
        }
        case NodeType::cCast:
        {
            auto* cast = static_cast<CastNode*>(operand);

            // Same as above.
            if (isPrimitiveConversion(cast->mTarget, cast->mResolvedType) == false)
            {
                return false;
            }

            return isTrivialOperand(cast->mTarget);
        }
        default:
        {
            // Everything else is not trivial (e.g. function calls, binary comparisons, etc.).
            return false;
        }
    }
}

static bool canSkipShortCircuiting(ExpressionNode* expr)
{
    if (expr->mNodeType == NodeType::cBinaryOp)
    {
        auto* binNode = static_cast<BinaryOpNode*>(expr);
        if (binNode->mOp == BinaryOp::cAnd || binNode->mOp == BinaryOp::cOr)
        {
            // Both sides need to be trivial for this subtree to skip short-circuiting.
            return canSkipShortCircuiting(binNode->mLeft) && canSkipShortCircuiting(binNode->mRight);
        }

        if (isComparisonOp(binNode->mOp))
        {
            // If we have a comparison, both sides must be trivial (no side effects) to skip short-circuiting.
            return isTrivialOperand(binNode->mLeft) && isTrivialOperand(binNode->mRight);
        }
    }

    // If we have bool literals or identifiers, these have no side-effects.
    if (expr->mNodeType == NodeType::cBoolLiteral ||
        (expr->mNodeType == NodeType::cIdentifier && getPrimitiveKind(expr->mResolvedType) == PrimitiveTypeKind::cBool))
    {
        return true;
    }

    return false;
}

bool CodeGenVisitor::emitBoolAsValue(ExpressionNode* expr)
{
    if (expr->mNodeType != NodeType::cBinaryOp)
    {
        // If we have no binary op, there's nothing to short-circuit, so emit normally.
        return visit(expr);
    }

    auto* binNode = static_cast<BinaryOpNode*>(expr);

    if (binNode->mOp != BinaryOp::cOr && binNode->mOp != BinaryOp::cAnd)
    {
        // If we don't have a || or &&, we can't short-circuit either.
        return visit(expr);
    }

    // Do not short circuit if the rhs is trivial to evaluate (which might be faster than the jumps).
    if (canSkipShortCircuiting(binNode->mRight))
    {
        // Push the lhs result.
        if (emitBoolAsValue(binNode->mLeft) == false)
        {
            return false;
        }

        // Push the rhs result.
        if (emitBoolAsValue(binNode->mRight) == false)
        {
            return false;
        }

        // Push the op.
        if (binNode->mOp == BinaryOp::cOr)
        {
            emit<OpCode::cIOr>();
        }
        else
        {
            emit<OpCode::cIAnd>();
        }

        return true;
    }

    // Otherwise, we do short-circuiting evaluation.
    BytecodeLabel joinLabel = makeLabel();

    // Push the lhs result.
    if (emitBoolAsValue(binNode->mLeft) == false)
    {
        return false;
    }

    // Push a jump based on the lhs result.
    if (binNode->mOp == BinaryOp::cOr)
    {
        // ||: if L != 0, keep it and jump to join; else drop L, evaluate R.
        emit<OpCode::cTestNZ>(joinLabel);
    }
    else
    {
        // &&: if L == 0, keep it and jump to join; else drop L, evaluate R.
        emit<OpCode::cTestZ>(joinLabel);
    }

    // The push the rhs result.
    if (emitBoolAsValue(binNode->mRight) == false)
    {
        return false;
    }

    // Emit the label where both branches end upa t.
    emit<OpCode::cLabel>(joinLabel);

    return true;
}

bool CodeGenVisitor::visitAsCondition(ExpressionNode* expr, u32 trueLabel, u32 falseLabel)
{
    // The idea here is to evaluate a condition and jump to a true or false label.
    // Accounts for possible short-circuiting for && and ||.
    if (expr->mNodeType == NodeType::cBinaryOp)
    {
        auto* binNode = static_cast<BinaryOpNode*>(expr);

        if ((binNode->mOp == BinaryOp::cOr || binNode->mOp == BinaryOp::cAnd) && canSkipShortCircuiting(expr))
        {
            // If we're skipping short-circuiting, do the whole thing normally.
            if (emitBoolAsValue(expr) == false)
            {
                return false;
            }

            // The (bool) result is on the stack, jump based on that.
            emit<OpCode::cJumpZ>(falseLabel);
            emit<OpCode::cJump>(trueLabel);

            return true;
        }

        // Full short-circuit (no explicit Pop needed, JumpZ/NZ consume L).
        if (binNode->mOp == BinaryOp::cOr || binNode->mOp == BinaryOp::cAnd)
        {
            // Evaluate the rhs to find out if we need to check the rhs.
            if (emitBoolAsValue(binNode->mLeft) == false)
            {
                return false;
            }

            // Push a jump based on the lhs result.
            if (binNode->mOp == BinaryOp::cOr)
            {
                // L || R: if L then true; else evaluate R.
                emit<OpCode::cJumpNZ>(trueLabel);
            }
            else
            {
                // L && R: if not L then false; else evaluate R.
                emit<OpCode::cJumpZ>(falseLabel);
            }

            // Evaluate the rhs recursively with our end labels (determining the final result).
            return visitAsCondition(binNode->mRight, trueLabel, falseLabel);
        }
    }

    // If this is not a binary op, resolving the expression will push 0 or 1 on the stack.
    if (emitBoolAsValue(expr) == false)
    {
        return false;
    }

    // Consume whatever was pushed and jump accordingly.
    emit<OpCode::cJumpZ>(falseLabel);
    emit<OpCode::cJump>(trueLabel);

    return true;
}

} // namespace simlang
