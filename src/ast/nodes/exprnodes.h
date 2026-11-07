#pragma once

#include "ast/binaryop.h"
#include "ast/nodes/astnode.h"
#include "ast/unaryop.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

class Scope;
struct Identifier;
struct InternedString;
struct ParamNode;
struct StatementNode;
struct Symbol;
struct Type;
struct TypeSpecifierNode;

enum ExprFlags : NodeFlagType
{
    cExprIsConstExpr = 1 << (cNodeFlagOffset + 0),
    cExprIsMutable = 1 << (cNodeFlagOffset + 1),
    cExprIsLValue = 1 << (cNodeFlagOffset + 2),
    cExprIsUsedAsLValue = 1 << (cNodeFlagOffset + 3),

    cExprFlagsOffset = cNodeFlagOffset + 4
};

struct ExpressionNode : ASTNode
{
    explicit ExpressionNode(NodeType type, SourceRange range)
        : ASTNode(type, range)
    {
    }

    Type* mResolvedType = nullptr;
};

struct ImplicitCastNode : ExpressionNode
{
    explicit ImplicitCastNode(SourceRange range, ExpressionNode* target)
        : ExpressionNode(NodeType::cImplicitCast, range)
        , mTarget(target)
    {
    }

    // No type node since the resolved type from the target node is what we're casting to.
    ExpressionNode* mTarget;
};

struct CastNode : ExpressionNode
{
    explicit CastNode(SourceRange range, TypeSpecifierNode* typeSpecifier, ExpressionNode* target)
        : ExpressionNode(NodeType::cCast, range)
        , mTypeSpecifier(typeSpecifier)
        , mTarget(target)
    {
    }

    TypeSpecifierNode* mTypeSpecifier;
    ExpressionNode* mTarget;
};

struct IdentifierNode : ExpressionNode
{
    explicit IdentifierNode(SourceRange range, Identifier* identifier)
        : ExpressionNode(NodeType::cIdentifier, range)
        , mIdentifier(identifier)
    {
    }

    Identifier* mIdentifier;

    // Resolved symbol.
    Symbol* mSymbol = nullptr;
};

struct ThisNode : ExpressionNode
{
    explicit ThisNode(SourceRange range)
        : ExpressionNode(NodeType::cThis, range)
    {
    }
};

struct IntLiteralNode : ExpressionNode
{
    explicit IntLiteralNode(SourceRange range, i32 i)
        : ExpressionNode(NodeType::cIntLiteral, range)
        , mInt(i)
    {
    }

    i32 mInt;
};

struct FloatLiteralNode : ExpressionNode
{
    explicit FloatLiteralNode(SourceRange range, f32 f)
        : ExpressionNode(NodeType::cFloatLiteral, range)
        , mFloat(f)
    {
    }

    f32 mFloat;
};

struct BoolLiteralNode : ExpressionNode
{
    explicit BoolLiteralNode(SourceRange range, bool b)
        : ExpressionNode(NodeType::cBoolLiteral, range)
        , mBool(b)
    {
    }

    bool mBool;
};

struct StringLiteralNode : ExpressionNode
{
    explicit StringLiteralNode(SourceRange range, const InternedString* str)
        : ExpressionNode(NodeType::cStringLiteral, range)
        , mString(str)
    {
    }

    const InternedString* mString;
};

struct NullLiteralNode : ExpressionNode
{
    explicit NullLiteralNode(SourceRange range)
        : ExpressionNode(NodeType::cNullLiteral, range)
    {
    }
};

struct FormatStringNode : ExpressionNode
{
    explicit FormatStringNode(SourceRange range,
                              ArrayView<const InternedString*> literals,
                              ArrayView<ExpressionNode*> args)
        : ExpressionNode(NodeType::cFormatString, range)
        , mLiterals(literals)
        , mArgs(args)
    {
    }

    ArrayView<const InternedString*> mLiterals;
    ArrayView<ExpressionNode*> mArgs;
};

struct CallArgument
{
    SourceRange mSourceRange;
    ExpressionNode* mValue = nullptr;
    bool mIsInOut = false;
};

struct NewObjectNode : ExpressionNode
{
    explicit NewObjectNode(SourceRange range, TypeSpecifierNode* typeSpecifier, ArrayView<CallArgument> args)
        : ExpressionNode(NodeType::cNewObject, range)
        , mTypeSpecifier(typeSpecifier)
        , mArgs(args)
    {
    }

    TypeSpecifierNode* mTypeSpecifier;
    ArrayView<CallArgument> mArgs;

    // The selected constructor (unused for lists/maps).
    Symbol* mConstructorSymbol = nullptr;
};

enum class LambdaCaptureKind : u8
{
    cSymbol,
    cThis
};

struct LambdaCapture
{
    // The symbol that is captured.
    Symbol* mSymbol = nullptr;
    // First use in the lambda that caused the variable to be captured.
    SourceRange mFirstUseRange;
    // Offset from the start of the closure on the heap.
    u32 mEnvironmentOffset = 0;
    // Symbol or "this".
    LambdaCaptureKind mKind = LambdaCaptureKind::cSymbol;
};

struct LambdaNode : ExpressionNode
{
    explicit LambdaNode(SourceRange range,
                        ArrayView<ParamNode*> params,
                        TypeSpecifierNode* returnTypeSpec,
                        StatementNode* body)
        : ExpressionNode(NodeType::cLambda, range)
        , mParams(params)
        , mReturnTypeSpec(returnTypeSpec)
        , mBody(body)
    {
    }

    ArrayView<ParamNode*> mParams;
    TypeSpecifierNode* mReturnTypeSpec;
    StatementNode* mBody;

    // Resolved semantic data.
    Symbol* mSymbol = nullptr;
    Scope* mScope = nullptr;
    Type* mLexicalThisType = nullptr;
    ArrayView<LambdaCapture> mCaptures;

    // Assigned during backend layout. Zero means that no environment is needed.
    u32 mEnvironmentTypeID = 0;
};

struct FunctionCallNode : ExpressionNode
{
    explicit FunctionCallNode(SourceRange range, ExpressionNode* receiver, ArrayView<CallArgument> args)
        : ExpressionNode(NodeType::cFunctionCall, range)
        , mReceiver(receiver)
        , mArgs(args)
    {
    }

    ExpressionNode* mReceiver;
    ArrayView<CallArgument> mArgs;
};

struct IndexCallNode : ExpressionNode
{
    explicit IndexCallNode(SourceRange range, ExpressionNode* receiver, ExpressionNode* index)
        : ExpressionNode(NodeType::cIndexCall, range)
        , mReceiver(receiver)
        , mIndex(index)
    {
    }

    ExpressionNode* mReceiver;
    ExpressionNode* mIndex;
};

struct MemberAccessNode : ExpressionNode
{
    explicit MemberAccessNode(SourceRange range, ExpressionNode* receiver, Identifier* member)
        : ExpressionNode(NodeType::cMemberAccess, range)
        , mReceiver(receiver)
        , mMember(member)
    {
    }

    ExpressionNode* mReceiver;
    Identifier* mMember;

    // Resolved symbol for the member.
    Symbol* mSymbol = nullptr;
};

struct ModuleAccessNode : ExpressionNode
{
    explicit ModuleAccessNode(SourceRange range, ExpressionNode* left, ExpressionNode* right)
        : ExpressionNode(NodeType::cModuleAccess, range)
        , mLeft(left)
        , mRight(right)
    {
    }

    ExpressionNode* mLeft;
    ExpressionNode* mRight;

    // Resolved symbol.
    Symbol* mSymbol = nullptr;
};

struct UnaryOpNode : ExpressionNode
{
    explicit UnaryOpNode(SourceRange range, UnaryOp op, ExpressionNode* expr)
        : ExpressionNode(NodeType::cUnaryOp, range)
        , mOp(op)
        , mExpr(expr)
    {
    }

    UnaryOp mOp;
    ExpressionNode* mExpr;
};

struct BinaryOpNode : ExpressionNode
{
    explicit BinaryOpNode(SourceRange range, BinaryOp op, ExpressionNode* left, ExpressionNode* right)
        : ExpressionNode(NodeType::cBinaryOp, range)
        , mOp(op)
        , mLeft(left)
        , mRight(right)
    {
    }

    BinaryOp mOp;
    ExpressionNode* mLeft;
    ExpressionNode* mRight;
};

struct TernaryExprNode : ExpressionNode
{
    explicit TernaryExprNode(SourceRange range,
                             ExpressionNode* condition,
                             ExpressionNode* thenExpr,
                             ExpressionNode* elseExpr)
        : ExpressionNode(NodeType::cTernaryExpr, range)
        , mCondition(condition)
        , mThenExpr(thenExpr)
        , mElseExpr(elseExpr)
    {
    }

    ExpressionNode* mCondition;
    ExpressionNode* mThenExpr;
    ExpressionNode* mElseExpr;
};

} // namespace simlang
