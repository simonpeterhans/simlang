#pragma once

#include "ast/astwalker.h"

namespace simlang
{

struct CompilerContext;
struct ConstValue;

class ConstFoldVisitor : public ASTWalker<ConstFoldVisitor>
{
public:
    explicit ConstFoldVisitor(CompilerContext& ctx);

    bool run(ASTNode* n);

    bool visitImplicitCast(ImplicitCastNode* node);
    bool visitCast(CastNode* node);
    bool visitFormatString(FormatStringNode* node);
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
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);
    bool visitIfStatement(IfStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitWhileStatement(WhileStatementNode* node);
    bool visitReturnStatement(ReturnStatementNode* node);
    bool visitPrintStatement(PrintStatementNode* node);

private:
    void foldExpr(ExpressionNode*& e);
    ExpressionNode* makeLiteral(ExpressionNode* src, const ConstValue& cv) const;

    CompilerContext& mCtx;
};

} // namespace simlang
