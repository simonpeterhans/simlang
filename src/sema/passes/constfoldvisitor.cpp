#include "sema/passes/constfoldvisitor.h"

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "diag/diagnosticmanager.h"
#include "driver/compilercontext.h"
#include "sema/constevaluator.h"
#include "symbol/constvalue.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/flags.h"

namespace simlang
{

ConstFoldVisitor::ConstFoldVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool ConstFoldVisitor::run(ASTNode* n)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(n);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool ConstFoldVisitor::visitImplicitCast(ImplicitCastNode* node)
{
    foldExpr(node->mTarget);
    return true;
}

bool ConstFoldVisitor::visitCast(CastNode* node)
{
    foldExpr(node->mTarget);
    return true;
}

bool ConstFoldVisitor::visitFormatString(FormatStringNode* node)
{
    for (auto*& arg : node->mArgs)
    {
        foldExpr(arg);
    }
    return true;
}

bool ConstFoldVisitor::visitNewObject(NewObjectNode* node)
{
    for (CallArgument& arg : node->mArgs)
    {
        foldExpr(arg.mValue);
    }

    return true;
}

bool ConstFoldVisitor::visitLambda(LambdaNode* node)
{
    return visit(node->mBody);
}

bool ConstFoldVisitor::visitFunctionCall(FunctionCallNode* node)
{
    foldExpr(node->mReceiver);

    for (CallArgument& arg : node->mArgs)
    {
        foldExpr(arg.mValue);
    }

    return true;
}

bool ConstFoldVisitor::visitIndexCall(IndexCallNode* node)
{
    foldExpr(node->mReceiver);
    foldExpr(node->mIndex);
    return true;
}

bool ConstFoldVisitor::visitMemberAccess(MemberAccessNode* node)
{
    foldExpr(node->mReceiver);
    return true;
}

bool ConstFoldVisitor::visitUnaryOp(UnaryOpNode* node)
{
    foldExpr(node->mExpr);
    return true;
}

bool ConstFoldVisitor::visitBinaryOp(BinaryOpNode* node)
{
    foldExpr(node->mLeft);
    foldExpr(node->mRight);
    return true;
}

bool ConstFoldVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    foldExpr(node->mCondition);
    foldExpr(node->mThenExpr);
    foldExpr(node->mElseExpr);
    return true;
}

bool ConstFoldVisitor::visitExpressionStatement(ExpressionStatementNode* node)
{
    foldExpr(node->mExpression);
    return true;
}

bool ConstFoldVisitor::visitMultiVariableDeclarationStatement(MultiVariableDeclarationStatementNode* node)
{
    foldExpr(node->mInit);
    return true;
}

bool ConstFoldVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    foldExpr(node->mInit);
    return true;
}

bool ConstFoldVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If the type declaration is a template, do nothing.
    // Derived types will be processed separately (flagged cStmtIsTemplateInstance).
    if (node->isTemplate())
    {
        return true;
    }

    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    return true;
}

bool ConstFoldVisitor::visitAssignmentStatement(AssignmentStatementNode* node)
{
    // Constant expressions for indices are folded here.
    foldExpr(node->mLHS);
    foldExpr(node->mRHS);
    return true;
}

bool ConstFoldVisitor::visitIfStatement(IfStatementNode* node)
{
    for (auto*& a : node->mBranches)
    {
        foldExpr(a->mCondition);
        visit(a->mBody);
    }
    visit(node->mElseBody);
    return true;
}

bool ConstFoldVisitor::visitForStatement(ForStatementNode* node)
{
    visit(node->mInit);
    foldExpr(node->mCondition);
    visit(node->mIncrement);
    visit(node->mBody);
    return true;
}

bool ConstFoldVisitor::visitWhileStatement(WhileStatementNode* node)
{
    foldExpr(node->mCondition);
    visit(node->mBody);
    return true;
}

bool ConstFoldVisitor::visitReturnStatement(ReturnStatementNode* node)
{
    for (ExpressionNode*& expr : node->mExpressions)
    {
        foldExpr(expr);
    }
    return true;
}

bool ConstFoldVisitor::visitPrintStatement(PrintStatementNode* node)
{
    foldExpr(node->mExpression);
    return true;
}

void ConstFoldVisitor::foldExpr(ExpressionNode*& e)
{
    if (e == nullptr)
    {
        return;
    }

    if (e->mNodeType == NodeType::cTernaryExpr)
    {
        e = foldTernaryExpr(static_cast<TernaryExprNode*>(e));
        return;
    }

    // Never replace the exact occurrence that denotes a storage location.
    // E.g., s.m = 1 or a[1 + 2] = 3;
    if (e->mFlags.test(cExprIsUsedAsLValue))
    {
        // But still recurse, since indices/receivers can fold.
        visit(e);
        return;
    }

    // If this already is a literal, do nothing (duh).
    switch (e->mNodeType)
    {
        case NodeType::cIntLiteral:
        case NodeType::cFloatLiteral:
        case NodeType::cBoolLiteral:
        case NodeType::cStringLiteral:
        case NodeType::cNullLiteral:
        {
            return;
        }
        default:
        {
            break;
        }
    }

    // Only compute values this pass can replace with a literal, otherwise visit their children below.
    if (e->mFlags.test(cExprIsConstExpr) && canMakeLiteral(e->mResolvedType))
    {
        ConstValue cv;
        if (mCtx.mConstEvaluator.evaluate(e, cv))
        {
            ExpressionNode* lit = makeLiteral(e, cv);
            if (lit != nullptr)
            {
                // Assign the type and replace the node in the tree.
                lit->mResolvedType = e->mResolvedType;
                lit->mFlags.set(cExprIsConstExpr, true);

                e = lit;

                return;
            }
        }
    }

    // If we didn't replace anything, recurse normally.
    visit(e);
}

ExpressionNode* ConstFoldVisitor::foldTernaryExpr(TernaryExprNode* node)
{
    // If the condition is constant, replace the ternary with the selected branch.
    // The branch itself doesn't have to be constant or representable as a literal.
    foldExpr(node->mCondition);
    if (node->mCondition->mFlags.test(cExprIsConstExpr))
    {
        ConstValue condition;
        mCtx.mConstEvaluator.evaluate(node->mCondition, condition);

        // The other branch is dropped, so we only evaluate one.
        ExpressionNode* selected = condition.as.mBool ? node->mThenExpr : node->mElseExpr;
        foldExpr(selected);

        if (node->mResolvedType->mKind == TypeKind::cStruct)
        {
            // If we have something like (true) ? a : b for structs:
            // We need to "fake" a temporary for that so that (true ? a : b).inc() operates on a temporary.
            // We do that by inserting an implicit cast, which then treats that as temporary expressions.
            auto* value = mCtx.allocate<ImplicitCastNode>(node->mSourceRange, selected);
            value->mResolvedType = node->mResolvedType;
            value->mFlags = node->mFlags;
            value->mFlags.set(cExprIsConstExpr, selected->mFlags.test(cExprIsConstExpr));
            return value;
        }

        return selected;
    }

    // Otherwise, either branch may execute, so fold both.
    foldExpr(node->mThenExpr);
    foldExpr(node->mElseExpr);

    return node;
}

bool ConstFoldVisitor::canMakeLiteral(const Type* type)
{
    // We currently have no literal representation of structs.
    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        case TypeKind::cNull:
        case TypeKind::cClass:
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

ExpressionNode* ConstFoldVisitor::makeLiteral(ExpressionNode* src, const ConstValue& cv) const
{
    if (cv.mKind == ConstValueKind::cNull)
    {
        // We cannot replace interfaces with null as they currently need an object handle and dispatch info.
        if (src->mResolvedType != nullptr && src->mResolvedType->mKind == TypeKind::cInterface)
        {
            return nullptr;
        }

        return mCtx.allocate<NullLiteralNode>(src->mSourceRange);
    }

    // If this is not a primitive (and wasn't null), we cannot replace it with a literal.
    if (cv.mKind != ConstValueKind::cPrimitive)
    {
        return nullptr;
    }

    switch (cv.mPrimitiveKind)
    {
        case PrimitiveTypeKind::cInt:
        {
            return mCtx.allocate<IntLiteralNode>(src->mSourceRange, cv.as.mInteger);
        }
        case PrimitiveTypeKind::cFloat:
        {
            return mCtx.allocate<FloatLiteralNode>(src->mSourceRange, cv.as.mFloat);
        }
        case PrimitiveTypeKind::cBool:
        {
            return mCtx.allocate<BoolLiteralNode>(src->mSourceRange, cv.as.mBool);
        }
        case PrimitiveTypeKind::cString:
        {
            return mCtx.allocate<StringLiteralNode>(src->mSourceRange, cv.as.mString);
        }
        default:
        {
            return nullptr;
        }
    }
}

} // namespace simlang
