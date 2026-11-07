#include "sema/passes/constanalysisvisitor.h"

#include <unordered_set>

#include "ast/assignmentop.h"
#include "ast/binaryop.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/constructoranalysis.h"
#include "sema/passes/constevalvisitor.h"
#include "symbol/constvalue.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/numeric.h"

namespace simlang
{

ConstAnalysisVisitor::ConstAnalysisVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool ConstAnalysisVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    visit(node);
    return mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

void ConstAnalysisVisitor::leaveNode(ASTNode* node)
{
    // When we leave a node, finalize its constexpr status since our children must have been visited.
    switch (node->mNodeType)
    {
        case NodeType::cIntLiteral:
        case NodeType::cFloatLiteral:
        case NodeType::cBoolLiteral:
        case NodeType::cStringLiteral:
        case NodeType::cNullLiteral:
        {
            // Literals are always constexpr.
            node->mFlags.set(cExprIsConstExpr, true);
            break;
        }
        case NodeType::cIdentifier:
        {
            // For identifiers, evaluate them to find out if they are.
            // This is because we may not have evaluated the expression that initializes the identifier.
            auto* expr = static_cast<ExpressionNode*>(node);
            ConstValue value;
            ConstEvalVisitor evaluator{mCtx.mAllocator};
            expr->mFlags.set(cExprIsConstExpr, evaluator.evaluate(expr, value));
            break;
        }
        case NodeType::cImplicitCast:
        {
            // Implicit casts are const expr if their target is.
            auto* cast = static_cast<ImplicitCastNode*>(node);
            node->mFlags.set(cExprIsConstExpr, cast->mTarget->mFlags.test(cExprIsConstExpr));
            break;
        }
        case NodeType::cUnaryOp:
        {
            // Unary ops are const expr if their target is.
            auto* unary = static_cast<UnaryOpNode*>(node);
            node->mFlags.set(cExprIsConstExpr, unary->mExpr->mFlags.test(cExprIsConstExpr));
            break;
        }
        case NodeType::cLambda:
        {
            // A lambda is const expr if it does not have a capture.
            auto* lambda = static_cast<LambdaNode*>(node);
            node->mFlags.set(cExprIsConstExpr, lambda->mCaptures.empty());
            break;
        }
        case NodeType::cModuleAccess:
        {
            // Module access is const expr if the rhs is.
            auto* access = static_cast<ModuleAccessNode*>(node);
            node->mFlags.set(cExprIsConstExpr, access->mRight->mFlags.test(cExprIsConstExpr));
            break;
        }
        case NodeType::cTernaryExpr:
        {
            // A ternary is const expr if the condition and the subsequently selected expression is.
            auto* ternary = static_cast<TernaryExprNode*>(node);
            // Check the condition.
            bool isConstExpr = ternary->mCondition->mFlags.test(cExprIsConstExpr);
            if (isConstExpr)
            {
                // If it is, evaluate it so we know which branch is taken.
                ConstValue condition;
                ConstEvalVisitor evaluator{mCtx.mAllocator};
                evaluator.evaluate(ternary->mCondition, condition);
                // Once we know that, select and test that branch.
                ExpressionNode* selected = condition.as.mBool ? ternary->mThenExpr : ternary->mElseExpr;
                isConstExpr = selected->mFlags.test(cExprIsConstExpr);
            }
            node->mFlags.set(cExprIsConstExpr, isConstExpr);
            break;
        }
        default:
        {
            break;
        }
    }
}

bool ConstAnalysisVisitor::visitCast(CastNode* node)
{
    // Analyze the target.
    visit(node->mTarget);

    // Check if the target is const expr.
    bool isConstExpr = node->mTarget->mFlags.test(cExprIsConstExpr);

    // If it's float to int, check if it fails or not.
    if (isConstExpr && getPrimitiveKind(node->mTarget->mResolvedType) == PrimitiveTypeKind::cFloat &&
        getPrimitiveKind(node->mResolvedType) == PrimitiveTypeKind::cInt)
    {
        // Evaluate the expression.
        ConstValue value;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        evaluator.evaluate(node->mTarget, value);

        // Convert the float to int and see if we allow that.
        i32 converted = 0;
        if (checkedFloatToInt(value.as.mFloat, converted) == false)
        {
            mCtx.report<cInvalidFloatToIntCast>(node->mSourceRange);
            isConstExpr = false;
        }
    }

    node->mFlags.set(cExprIsConstExpr, isConstExpr);

    return true;
}

bool ConstAnalysisVisitor::visitNewObject(NewObjectNode* node)
{
    ASTWalker::visitNewObject(node);

    // Evaluate the expression to see if it is const-expr eligible.
    ConstValue value;
    ConstEvalVisitor evaluator{mCtx.mAllocator};
    node->mFlags.set(cExprIsConstExpr, evaluator.evaluate(node, value));

    return true;
}

bool ConstAnalysisVisitor::visitMemberAccess(MemberAccessNode* node)
{
    // Analyze the receiver.
    visit(node->mReceiver);

    // We expect a variable, not a method (yet?).
    if (node->mSymbol != nullptr && node->mSymbol->mSymbolType == SymbolType::cMemberVariable)
    {
        // This only works if we have a struct and its member is also a const expr.
        node->mFlags.set(cExprIsConstExpr,
                         node->mReceiver->mResolvedType->mKind == TypeKind::cStruct &&
                             node->mReceiver->mFlags.test(cExprIsConstExpr));
    }

    return true;
}

bool ConstAnalysisVisitor::visitModuleAccess(ModuleAccessNode* node)
{
    // Analyze the rhs since the lhs is the module identifier.
    visit(node->mRight);
    return true;
}

bool ConstAnalysisVisitor::checkDivisor(ExpressionNode* expr, std::string_view op)
{
    // If the expression isn't const, we can't do anything with it.
    if (expr->mFlags.test(cExprIsConstExpr) == false)
    {
        return true;
    }

    // Otherwise, check if we would be dividing by 0.
    ConstValue value;
    ConstEvalVisitor evaluator{mCtx.mAllocator};
    evaluator.evaluate(expr, value);
    if ((value.mPrimitiveKind == PrimitiveTypeKind::cInt && value.as.mInteger == 0) ||
        (value.mPrimitiveKind == PrimitiveTypeKind::cFloat && value.as.mFloat == 0.0f))
    {
        mCtx.report<cDivisionByZero>(expr->mSourceRange, op);
        return false;
    }

    return true;
}

bool ConstAnalysisVisitor::checkShiftCount(ExpressionNode* expr, std::string_view op)
{
    // If the expression isn't const, we can't do anything with it.
    if (expr->mFlags.test(cExprIsConstExpr) == false)
    {
        return true;
    }

    // Otherwise, check if we would be shifting out of range.
    ConstValue value;
    ConstEvalVisitor evaluator{mCtx.mAllocator};
    evaluator.evaluate(expr, value);
    if (value.as.mInteger < 0 || value.as.mInteger >= 32)
    {
        mCtx.report<cShiftCountOutOfRange>(expr->mSourceRange, op, value.as.mInteger);
        return false;
    }

    return true;
}

bool ConstAnalysisVisitor::visitBinaryOp(BinaryOpNode* node)
{
    // Visit our child nodes.
    ASTWalker::visitBinaryOp(node);

    // Check the resolved nodes based on the op.
    bool valid = true;
    if (node->mOp == BinaryOp::cDiv || node->mOp == BinaryOp::cMod)
    {
        valid = checkDivisor(node->mRight, binaryOpToString(node->mOp));
    }
    else if (node->mOp == BinaryOp::cShiftL || node->mOp == BinaryOp::cShiftR)
    {
        valid = checkShiftCount(node->mRight, binaryOpToString(node->mOp));
    }

    // If the above and both lhs and rhs are const expr, set it.
    node->mFlags.set(cExprIsConstExpr,
                     valid && node->mLeft->mFlags.test(cExprIsConstExpr) &&
                         node->mRight->mFlags.test(cExprIsConstExpr));
    return true;
}

bool ConstAnalysisVisitor::visitAssignmentStatement(AssignmentStatementNode* node)
{
    // Visit our child nodes.
    ASTWalker::visitAssignmentStatement(node);

    // Check the resolved nodes based on the op.
    if (node->mOp == AssignmentOp::cAssDiv || node->mOp == AssignmentOp::cAssMod)
    {
        checkDivisor(node->mRHS, assignmentOpToString(node->mOp));
    }
    else if (node->mOp == AssignmentOp::cAssShl || node->mOp == AssignmentOp::cAssShr)
    {
        checkShiftCount(node->mRHS, assignmentOpToString(node->mOp));
    }

    // No need to set the node flag here because we currently don't treat an assignment as an expression.

    return true;
}

bool ConstAnalysisVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    // If this is a member variable declaration, it has no value and is thus not const expr.
    Symbol* symbol = node->mSymbol;
    if (symbol->mSymbolType == SymbolType::cMemberVariable)
    {
        return true;
    }

    // Otherwise, resolve the initializer.
    visit(node->mInit);

    // If this is a global but not const expr, complain.
    bool isConstExpr = node->mInit->mFlags.test(cExprIsConstExpr);
    if (isConstExpr == false && symbol->mSymbolType == SymbolType::cGlobalVariable)
    {
        mCtx.report<cGlobalInitializerNotConstExpr>(node->mInit->mSourceRange, node->mIdentifier);
    }

    // If this is a const and not yet evaluated, do that now.
    if (symbol->mFlags.test(SymbolFlags::cMutable) == false && symbol->mConstEvalState != ConstEvalState::cReady)
    {
        if (isConstExpr)
        {
            ConstEvalVisitor evaluator{mCtx.mAllocator};
            evaluator.evaluate(node->mInit, symbol->mConstValue);
        }
        symbol->mConstEvalState = isConstExpr ? ConstEvalState::cReady : ConstEvalState::cFailed;
    }

    return true;
}

bool ConstAnalysisVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // Never do this for template declarations, instances are visited separately.
    if (node->isTemplate() == false)
    {
        if (node->isStruct())
        {
            // Analyze the constructor's constexpr subset before constant folding changes its body.
            auto* type = static_cast<AggregateType*>(node->mSymbol->mType);
            getConstructorAnalysis(mCtx.mAllocator, type, type->mConstructorSymbol);
        }
        ASTWalker::visitTypeDeclarationStatement(node);
    }

    return true;
}

bool ConstAnalysisVisitor::visitSwitchStatement(SwitchStatementNode* node)
{
    // Visit our child nodes.
    ASTWalker::visitSwitchStatement(node);

    // Go over all of our sections.
    std::unordered_set<i32> cases;
    for (SwitchSectionStatementNode* section : node->mSections)
    {
        // If this is "default", ignore it.
        if (section->mCaseExpression == nullptr)
        {
            continue;
        }

        // If the case expression is not const expr, complain (duh).
        // (It was evaluated by the template visitor call from earlier.)
        if (section->mCaseExpression->mFlags.test(cExprIsConstExpr) == false)
        {
            mCtx.report<cCaseLabelNotConstexpr>(section->mCaseExpression->mSourceRange);
            continue;
        }

        // Make sure we don't have any duplicate case labels.
        ConstValue value;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        evaluator.evaluate(section->mCaseExpression, value);

        if (cases.insert(value.as.mInteger).second == false)
        {
            mCtx.report<cDuplicateCaseLabel>(section->mCaseExpression->mSourceRange, value.as.mInteger);
        }
        else
        {
            section->mCaseValue = value.as.mInteger;
        }
    }

    return true;
}

} // namespace simlang
