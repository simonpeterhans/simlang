#pragma once

#include "ast/astwalker.h"

namespace simlang
{

struct CompilerContext;

class ImplicitThisVisitor : public ASTWalker<ImplicitThisVisitor>
{
public:
    explicit ImplicitThisVisitor(CompilerContext& ctx);

    bool run(ASTNode* n);

    bool visitFormatString(FormatStringNode* node);
    bool visitCast(CastNode* node);
    bool visitNewObject(NewObjectNode* node);
    bool visitFunctionCall(FunctionCallNode* node);
    bool visitIndexCall(IndexCallNode* node);
    bool visitMemberAccess(MemberAccessNode* node);
    bool visitUnaryOp(UnaryOpNode* node);
    bool visitBinaryOp(BinaryOpNode* node);
    bool visitTernaryExpr(TernaryExprNode* node);

    bool visitExpressionStatement(ExpressionStatementNode* node);
    bool visitAssignmentStatement(AssignmentStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);
    bool visitIfStatement(IfStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitWhileStatement(WhileStatementNode* node);
    bool visitSwitchStatement(SwitchStatementNode* node);
    bool visitReturnStatement(ReturnStatementNode* node);
    bool visitPrintStatement(PrintStatementNode* node);
    bool visitParamDeclaration(ParamDeclarationNode* node);

private:
    void checkExpression(ExpressionNode*& e);

    CompilerContext& mCtx;
    bool mInMemberFunction = false;
};

} // namespace simlang
