#pragma once

#include "ast/astwalker.h"
#include "symbol/constvalue.h"

namespace simlang
{

class ArenaAllocator;

class ConstEvalVisitor : public ASTWalker<ConstEvalVisitor>
{
public:
    explicit ConstEvalVisitor(ArenaAllocator& allocator);

    bool evaluate(ExpressionNode* node, ConstValue& outValue);

    bool visitImplicitCast(ImplicitCastNode* node);
    bool visitCast(CastNode* node);
    bool visitIdentifier(IdentifierNode* node);
    bool visitThis(ThisNode*);
    bool visitIntLiteral(IntLiteralNode* node);
    bool visitFloatLiteral(FloatLiteralNode* node);
    bool visitBoolLiteral(BoolLiteralNode* node);
    bool visitStringLiteral(StringLiteralNode* node);
    bool visitNullLiteral(NullLiteralNode*);
    bool visitFormatString(FormatStringNode* node);
    bool visitNewObject(NewObjectNode*);
    bool visitFunctionCall(FunctionCallNode* node);
    bool visitIndexCall(IndexCallNode*);
    bool visitMemberAccess(MemberAccessNode*);
    bool visitModuleAccess(ModuleAccessNode*);
    bool visitUnaryOp(UnaryOpNode* node);
    bool visitBinaryOp(BinaryOpNode* node);
    bool visitTernaryExpr(TernaryExprNode* node);

private:
    ArenaAllocator& mAllocator;
    ConstValue mValue;
};

} // namespace simlang
