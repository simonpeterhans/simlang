#pragma once

#include <string_view>

#include "ast/astwalker.h"

namespace simlang
{

struct CompilerContext;

class ConstAnalysisVisitor : public ASTWalker<ConstAnalysisVisitor>
{
public:
    explicit ConstAnalysisVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);
    void leaveNode(ASTNode* node);

    bool visitCast(CastNode* node);
    bool visitNewObject(NewObjectNode* node);
    bool visitLambda(LambdaNode* node);
    bool visitMemberAccess(MemberAccessNode* node);
    bool visitModuleAccess(ModuleAccessNode* node);
    bool visitBinaryOp(BinaryOpNode* node);
    bool visitTernaryExpr(TernaryExprNode* node);
    bool visitAssignmentStatement(AssignmentStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);
    bool visitSwitchStatement(SwitchStatementNode* node);

private:
    bool checkDivisor(ExpressionNode* expr, std::string_view op);
    bool checkShiftCount(ExpressionNode* expr, std::string_view op);

    CompilerContext& mCtx;
};

} // namespace simlang
