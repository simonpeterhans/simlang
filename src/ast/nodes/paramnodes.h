#pragma once

#include "ast/nodes/astnode.h"

namespace simlang
{

struct ExpressionNode;
struct Identifier;
struct Symbol;
struct TypeSpecifierNode;

struct ParamNode : ASTNode
{
    explicit ParamNode(NodeType type, SourceRange range)
        : ASTNode(type, range)
    {
    }
};

struct ParamDeclarationNode : ParamNode
{
    explicit ParamDeclarationNode(SourceRange range,
                                  SourceRange identifierRange,
                                  Identifier* identifier,
                                  TypeSpecifierNode* typeSpec,
                                  ExpressionNode* defaultValue,
                                  bool isInOut)
        : ParamNode(NodeType::cParamDeclaration, range)
        , mIsInOut(isInOut)
        , mIdentifierRange(identifierRange)
        , mIdentifier(identifier)
        , mTypeSpec(typeSpec)
        , mDefaultValue(defaultValue)
    {
    }

    // This is here for alignment purposes since we inherit from ParamNode/ASTNode.
    bool mIsInOut;
    SourceRange mIdentifierRange;
    Identifier* mIdentifier;
    TypeSpecifierNode* mTypeSpec;
    ExpressionNode* mDefaultValue;

    // Resolved symbol.
    Symbol* mSymbol = nullptr;
};

} // namespace simlang
