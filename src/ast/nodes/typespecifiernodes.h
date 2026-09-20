#pragma once

#include "ast/nodes/astnode.h"
#include "util/arrayview.h"

namespace simlang
{

struct ExpressionNode;
struct Identifier;
struct Type;

struct TypeSpecifierNode : ASTNode
{
    explicit TypeSpecifierNode(NodeType type, SourceRange range)
        : ASTNode(type, range)
    {
    }

    // The resolved type.
    Type* mType = nullptr;
};

struct NamedTypeSpecifierNode : TypeSpecifierNode
{
    explicit NamedTypeSpecifierNode(SourceRange range,
                                    ExpressionNode* nameExpression,
                                    ArrayView<TypeSpecifierNode*> typeArgs = {})
        : TypeSpecifierNode(NodeType::cNamedTypeSpecifier, range)
        , mNameExpression(nameExpression)
        , mTypeArgs(typeArgs)
    {
    }

    // This is either an IdentifierNode or a ModuleAccessNode.
    ExpressionNode* mNameExpression;
    ArrayView<TypeSpecifierNode*> mTypeArgs;
};

struct FunctionTypeParameterSpecifier
{
    SourceRange mSourceRange;
    TypeSpecifierNode* mTypeSpecifier = nullptr;
    bool mIsInOut = false;
};

struct FunctionTypeSpecifierNode : TypeSpecifierNode
{
    explicit FunctionTypeSpecifierNode(SourceRange range,
                                       ArrayView<FunctionTypeParameterSpecifier> params,
                                       TypeSpecifierNode* returnTypeSpecifier)
        : TypeSpecifierNode(NodeType::cFunctionTypeSpecifier, range)
        , mParams(params)
        , mReturnTypeSpecifier(returnTypeSpecifier)
    {
    }

    ArrayView<FunctionTypeParameterSpecifier> mParams;
    TypeSpecifierNode* mReturnTypeSpecifier;
};

struct SubstitutedTypeSpecifierNode : TypeSpecifierNode
{
    explicit SubstitutedTypeSpecifierNode(SourceRange range, Type* type)
        : TypeSpecifierNode(NodeType::cSubstitutedTypeSpecifier, range)
    {
        mType = type;
    }
};

} // namespace simlang
