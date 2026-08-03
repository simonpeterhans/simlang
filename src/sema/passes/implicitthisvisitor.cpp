#include "sema/passes/implicitthisvisitor.h"

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "diag/diagnosticmanager.h"
#include "driver/compilercontext.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "util/arrayview.h"
#include "util/scoping.h"

namespace simlang
{

ImplicitThisVisitor::ImplicitThisVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool ImplicitThisVisitor::run(ASTNode* n)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(n);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool ImplicitThisVisitor::visitFormatString(FormatStringNode* node)
{
    for (auto*& arg : node->mArgs)
    {
        checkExpression(arg);
    }
    return true;
}

bool ImplicitThisVisitor::visitCast(CastNode* node)
{
    checkExpression(node->mTarget);
    return true;
}

bool ImplicitThisVisitor::visitUnaryOp(UnaryOpNode* node)
{
    checkExpression(node->mExpr);
    return true;
}

bool ImplicitThisVisitor::visitBinaryOp(BinaryOpNode* node)
{
    checkExpression(node->mLeft);
    checkExpression(node->mRight);
    return true;
}

bool ImplicitThisVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    checkExpression(node->mCondition);
    checkExpression(node->mThenExpr);
    checkExpression(node->mElseExpr);
    return true;
}

bool ImplicitThisVisitor::visitNewObject(NewObjectNode* node)
{
    for (ExpressionNode*& arg : node->mInitializerArguments)
    {
        checkExpression(arg);
    }
    for (FieldInitializer* init : node->mFieldInitializers)
    {
        checkExpression(init->mValue);
    }

    return true;
}

bool ImplicitThisVisitor::visitFunctionCall(FunctionCallNode* node)
{
    checkExpression(node->mReceiver);
    for (auto*& a : node->mArgs)
    {
        checkExpression(a);
    }
    return true;
}

bool ImplicitThisVisitor::visitIndexCall(IndexCallNode* node)
{
    checkExpression(node->mReceiver);
    checkExpression(node->mIndex);
    return true;
}

bool ImplicitThisVisitor::visitMemberAccess(MemberAccessNode* node)
{
    checkExpression(node->mReceiver);
    return true;
}

bool ImplicitThisVisitor::visitExpressionStatement(ExpressionStatementNode* node)
{
    checkExpression(node->mExpression);
    return true;
}

bool ImplicitThisVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    checkExpression(node->mInit);
    return true;
}

bool ImplicitThisVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // Track member function stuff.
    bool isMemberFunction = node->mSymbol != nullptr && node->mSymbol->mSymbolType == SymbolType::cMemberFunction;
    ScopedValueBinder svb{mInMemberFunction, isMemberFunction};

    // Visit all the params and the body (but not the return type spec because there can't be a this there).
    for (auto* p : node->mParams)
    {
        visit(p);
    }

    visit(node->mBody);

    return true;
}

bool ImplicitThisVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
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

bool ImplicitThisVisitor::visitAssignmentStatement(AssignmentStatementNode* node)
{
    checkExpression(node->mLHS);
    checkExpression(node->mRHS);
    return true;
}

bool ImplicitThisVisitor::visitIfStatement(IfStatementNode* node)
{
    for (auto*& a : node->mBranches)
    {
        checkExpression(a->mCondition);
        visit(a->mBody);
    }
    visit(node->mElseBody);
    return true;
}

bool ImplicitThisVisitor::visitForStatement(ForStatementNode* node)
{
    visit(node->mInit);
    checkExpression(node->mCondition);
    visit(node->mIncrement);
    visit(node->mBody);
    return true;
}

bool ImplicitThisVisitor::visitWhileStatement(WhileStatementNode* node)
{
    checkExpression(node->mCondition);
    visit(node->mBody);
    return true;
}

bool ImplicitThisVisitor::visitSwitchStatement(SwitchStatementNode* node)
{
    checkExpression(node->mExpression);

    for (SwitchSectionStatementNode* section : node->mSections)
    {
        checkExpression(section->mCaseExpression);

        for (StatementNode* stmt : section->mStatements)
        {
            if (visit(stmt) == false)
            {
                return false;
            }
        }
    }

    return true;
}

bool ImplicitThisVisitor::visitReturnStatement(ReturnStatementNode* node)
{
    checkExpression(node->mExpression);
    return true;
}

bool ImplicitThisVisitor::visitPrintStatement(PrintStatementNode* node)
{
    checkExpression(node->mExpression);
    return true;
}

bool ImplicitThisVisitor::visitParamDeclaration(ParamDeclarationNode* node)
{
    checkExpression(node->mDefaultValue);
    return true;
}

void ImplicitThisVisitor::checkExpression(ExpressionNode*& e)
{
    if (e == nullptr)
    {
        return;
    }

    // Resolve the node first, recursing if needed.
    visit(e);

    // We might have to do something if we're inside of a member function and have an identifier.
    if (mInMemberFunction == false || e->mNodeType != NodeType::cIdentifier)
    {
        return;
    }

    auto* id = static_cast<IdentifierNode*>(e);
    Symbol* symbol = id->mSymbol;

    // If we are a member variable inside a member function, make it an explicit member access node with this.
    // This works because we only need to desugar x to this.x if we are inside a member function and x is a member.
    // Anywhere else this is impossible since we'd have to access x via an object.
    if (symbol->mSymbolType == SymbolType::cMemberVariable || symbol->mSymbolType == SymbolType::cMemberFunction)
    {
        auto* thisNode = mCtx.allocate<ThisNode>(id->mSourceRange);
        e = mCtx.allocate<MemberAccessNode>(id->mSourceRange, thisNode, id->mIdentifier);
    }
}

} // namespace simlang
