#pragma once

#include "ast/binaryop.h"
#include "ast/nodes/astnode.h"
#include "ast/unaryop.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

struct Identifier;
struct InternedString;
struct Symbol;
struct Type;
struct TypeSpecifierNode;

enum ExprFlags : NodeFlagType
{
    cExprIsConstExpr = 1 << (cNodeFlagOffset + 0),
    cExprIsMutable = 1 << (cNodeFlagOffset + 1),
    cExprIsLValue = 1 << (cNodeFlagOffset + 2),
    cExprIsUsedAsLValue = 1 << (cNodeFlagOffset + 3),
    cExprIsInOutArgument = 1 << (cNodeFlagOffset + 4),

    cExprFlagsOffset = cNodeFlagOffset + 5
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

struct FieldInitializer
{
    explicit FieldInitializer(SourceRange range,
                              SourceRange identifierRange,
                              Identifier* identifier,
                              ExpressionNode* value)
        : mSourceRange(range)
        , mIdentifierRange(identifierRange)
        , mIdentifier(identifier)
        , mValue(value)
    {
    }

    SourceRange mSourceRange;
    SourceRange mIdentifierRange;
    Identifier* mIdentifier;
    ExpressionNode* mValue;

    // The resolved field the initializer is initializing.
    Symbol* mResolvedField = nullptr;
};

enum class ConstructionKind : u8
{
    cValue,
    cReference
};

struct NewObjectNode : ExpressionNode
{
    explicit NewObjectNode(SourceRange range,
                           TypeSpecifierNode* typeSpecifier,
                           ArrayView<FieldInitializer*> fieldInitializers,
                           ArrayView<ExpressionNode*> initializerArguments,
                           ConstructionKind constructionKind)
        : ExpressionNode(NodeType::cNewObject, range)
        , mTypeSpecifier(typeSpecifier)
        , mFieldInitializers(fieldInitializers)
        , mInitializerArguments(initializerArguments)
        , mConstructionKind(constructionKind)
    {
    }

    TypeSpecifierNode* mTypeSpecifier;
    ArrayView<FieldInitializer*> mFieldInitializers;
    ArrayView<ExpressionNode*> mInitializerArguments;
    ConstructionKind mConstructionKind = ConstructionKind::cReference;

    // The selected class init method.
    Symbol* mInitMethodSymbol = nullptr;
};

struct FunctionCallNode : ExpressionNode
{
    explicit FunctionCallNode(SourceRange range, ExpressionNode* receiver, ArrayView<ExpressionNode*> args)
        : ExpressionNode(NodeType::cFunctionCall, range)
        , mReceiver(receiver)
        , mArgs(args)
    {
    }

    ExpressionNode* mReceiver;
    ArrayView<ExpressionNode*> mArgs;
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
