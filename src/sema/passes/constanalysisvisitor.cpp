#include "sema/passes/constanalysisvisitor.h"

#include <unordered_set>
#include <vector>

#include "ast/assignmentop.h"
#include "ast/binaryop.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/constructoranalysis.h"
#include "sema/constevaluator.h"
#include "symbol/constvalue.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"

namespace simlang
{

namespace
{

class CallableBodyCollector : public ASTWalker<CallableBodyCollector>
{
public:
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
    {
        mBodies.push_back(node->mBody);
        return ASTWalker::visitFunctionDeclarationStatement(node);
    }

    bool visitLambda(LambdaNode* node)
    {
        mBodies.push_back(node->mBody);
        return ASTWalker::visitLambda(node);
    }

    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
    {
        // Only instantiated template bodies have been type checked.
        if (node->isTemplate())
        {
            return true;
        }

        return ASTWalker::visitTypeDeclarationStatement(node);
    }

    const std::vector<StatementNode*>& getBodies() const { return mBodies; }

private:
    std::vector<StatementNode*> mBodies;
};

} // namespace

ConstAnalysisVisitor::ConstAnalysisVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool ConstAnalysisVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();

    // Collect bodies independently of expression evaluation, with outer bodies before nested lambdas.
    CallableBodyCollector collector;
    collector.visit(node);

    // Analyze module initializers and type declarations before the independent callable bodies.
    visit(node);

    // Analyze all the bodies.
    for (StatementNode* body : collector.getBodies())
    {
        visit(body);
    }

    return mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

void ConstAnalysisVisitor::leaveNode(ASTNode* node)
{
    // When we leave a node, finalize its constexpr status after analyzing the relevant children.
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
            expr->mFlags.set(cExprIsConstExpr, mCtx.mConstEvaluator.evaluate(expr, value));
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

    // Let the evaluator perform the checked conversion.
    if (isConstExpr && getPrimitiveKind(node->mTarget->mResolvedType) == PrimitiveTypeKind::cFloat &&
        getPrimitiveKind(node->mResolvedType) == PrimitiveTypeKind::cInt)
    {
        ConstValue value;
        ConstEvalFailure failure;
        isConstExpr = mCtx.mConstEvaluator.evaluate(node, value, &failure);
        reportFailure(failure);
    }

    node->mFlags.set(cExprIsConstExpr, isConstExpr);

    return true;
}

bool ConstAnalysisVisitor::visitNewObject(NewObjectNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();

    ASTWalker::visitNewObject(node);

    // Argument diagnostics have already identified an invalid operation.
    if (mCtx.mDiag.hasErrorsSince(checkpoint))
    {
        node->mFlags.set(cExprIsConstExpr, false);
        return true;
    }

    // Evaluate the expression to see if it is const-expr eligible.
    ConstValue value;
    ConstEvalFailure failure;
    node->mFlags.set(cExprIsConstExpr, mCtx.mConstEvaluator.evaluate(node, value, &failure));
    reportFailure(failure);

    return true;
}

void ConstAnalysisVisitor::reportFailure(const ConstEvalFailure& failure)
{
    switch (failure.mKind)
    {
        case ConstEvalFailureKind::cDivisionByZero:
        {
            auto* binary = static_cast<BinaryOpNode*>(failure.mExpression);
            mCtx.report<cDivisionByZero>(binary->mRight->mSourceRange, binaryOpToString(binary->mOp));
            break;
        }
        case ConstEvalFailureKind::cShiftCountOutOfRange:
        {
            auto* binary = static_cast<BinaryOpNode*>(failure.mExpression);
            mCtx.report<cShiftCountOutOfRange>(binary->mRight->mSourceRange,
                                               binaryOpToString(binary->mOp),
                                               failure.mShiftCount);
            break;
        }
        case ConstEvalFailureKind::cInvalidFloatToIntCast:
        {
            mCtx.report<cInvalidFloatToIntCast>(failure.mExpression->mSourceRange);
            break;
        }
        case ConstEvalFailureKind::cNone:
        {
            break;
        }
    }
}

bool ConstAnalysisVisitor::visitLambda(LambdaNode*)
{
    // The function value is classified in leaveNode().
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
    if (mCtx.mConstEvaluator.evaluate(expr, value) == false)
    {
        return true;
    }

    if (isValidConstDivisor(value) == false)
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
    if (mCtx.mConstEvaluator.evaluate(expr, value) == false)
    {
        return true;
    }

    if (isValidConstShiftCount(value.as.mInteger) == false)
    {
        mCtx.report<cShiftCountOutOfRange>(expr->mSourceRange, op, value.as.mInteger);
        return false;
    }

    return true;
}

bool ConstAnalysisVisitor::visitBinaryOp(BinaryOpNode* node)
{
    // Visit our child nodes.
    visit(node->mLeft);

    bool leftIsConstExpr = node->mLeft->mFlags.test(cExprIsConstExpr);
    bool skipsRight = false;
    if (leftIsConstExpr && (node->mOp == BinaryOp::cAnd || node->mOp == BinaryOp::cOr))
    {
        ConstValue left;
        mCtx.mConstEvaluator.evaluate(node->mLeft, left);
        skipsRight = left.as.mBool == (node->mOp == BinaryOp::cOr);
    }

    if (skipsRight == false)
    {
        visit(node->mRight);
    }

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

    // A short-circuited expression needs only the left value; otherwise both operands must be const expr.
    node->mFlags.set(cExprIsConstExpr,
                     valid && leftIsConstExpr && (skipsRight || node->mRight->mFlags.test(cExprIsConstExpr)));
    return true;
}

bool ConstAnalysisVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    // A ternary is const expr if the condition and the subsequently selected expression is.
    visit(node->mCondition);
    bool isConstExpr = node->mCondition->mFlags.test(cExprIsConstExpr);
    if (isConstExpr)
    {
        // If it is, evaluate it so we know which branch is taken.
        ConstValue condition;
        mCtx.mConstEvaluator.evaluate(node->mCondition, condition);

        // Once we know that, select and test that branch.
        ExpressionNode* selected = condition.as.mBool ? node->mThenExpr : node->mElseExpr;
        visit(selected);
        isConstExpr = selected->mFlags.test(cExprIsConstExpr);
    }
    else
    {
        visit(node->mThenExpr);
        visit(node->mElseExpr);
    }
    node->mFlags.set(cExprIsConstExpr, isConstExpr);
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
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    visit(node->mInit);

    // If this is a global but not const expr, complain.
    bool isConstExpr = node->mInit->mFlags.test(cExprIsConstExpr);
    if (isConstExpr == false && symbol->mSymbolType == SymbolType::cGlobalVariable &&
        mCtx.mDiag.hasNoErrorsSince(checkpoint))
    {
        mCtx.report<cGlobalInitializerNotConstExpr>(node->mInit->mSourceRange, node->mIdentifier);
    }

    // Declaration and identifier queries share the same symbol evaluation and cycle bookkeeping.
    if (symbol->mFlags.test(SymbolFlags::cMutable) == false)
    {
        ConstValue value;
        mCtx.mConstEvaluator.evaluateSymbol(symbol, value);
    }

    return true;
}

bool ConstAnalysisVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode*)
{
    // Callable bodies are analyzed independently by run().
    return true;
}

bool ConstAnalysisVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // Never do this for template declarations, instances are visited separately.
    if (node->isTemplate() == false)
    {
        if (node->isStruct() || node->isClass())
        {
            auto* type = static_cast<AggregateType*>(node->mSymbol->mType);

            // Analyze the constructor before constant folding changes its body.
            ConstructorAnalyzer analyzer{mCtx.mConstEvaluator};
            analyzer.analyze(type, type->mConstructorSymbol);
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
        mCtx.mConstEvaluator.evaluate(section->mCaseExpression, value);

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
