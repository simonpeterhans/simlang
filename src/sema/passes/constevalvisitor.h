#pragma once

#include "ast/astwalker.h"
#include "symbol/constvalue.h"

namespace simlang
{

class ArenaAllocator;

enum class ConstEvalFailureKind : u8
{
    cNone,
    cDivisionByZero,
    cShiftCountOutOfRange,
    cInvalidFloatToIntCast
};

struct ConstEvalFailure
{
    ConstEvalFailureKind mKind = ConstEvalFailureKind::cNone;
    ExpressionNode* mExpression = nullptr;
    i32 mShiftCount = 0;
};

class ConstEvalVisitor : public ASTWalker<ConstEvalVisitor>
{
public:
    explicit ConstEvalVisitor(ArenaAllocator& allocator);

    // A false result without a failure means the expression cannot be evaluated at compile time.
    bool evaluate(ExpressionNode* node, ConstValue& outValue, ConstEvalFailure* failure = nullptr);

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
    bool visitNewObject(NewObjectNode* node);
    bool visitLambda(LambdaNode* node);
    bool visitFunctionCall(FunctionCallNode* node);
    bool visitIndexCall(IndexCallNode*);
    bool visitMemberAccess(MemberAccessNode*);
    bool visitModuleAccess(ModuleAccessNode*);
    bool visitUnaryOp(UnaryOpNode* node);
    bool visitBinaryOp(BinaryOpNode* node);
    bool visitTernaryExpr(TernaryExprNode* node);

private:
    bool evaluateValue(ExpressionNode* node, ConstValue& outValue);
    bool fail(ConstEvalFailureKind kind, ExpressionNode* expression, i32 shiftCount = 0);

    struct CallFrame
    {
        FunctionDeclarationStatementNode* mConstructor;
        ArrayView<const ConstValue> mArguments;
        const CallFrame* mCaller;
    };

    ArenaAllocator& mAllocator;
    const CallFrame* mFrame = nullptr;
    ConstEvalFailure* mFailure = nullptr;
    ConstValue mValue;
};

} // namespace simlang
