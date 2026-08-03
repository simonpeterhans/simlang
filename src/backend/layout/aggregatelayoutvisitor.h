#pragma once

#include <unordered_set>

#include "ast/astwalker.h"

namespace simlang
{

struct AggregateLayout;
struct AggregateType;
struct CompilerContext;
struct Type;

class AggregateLayoutVisitor : public ASTWalker<AggregateLayoutVisitor>
{
public:
    explicit AggregateLayoutVisitor(CompilerContext& ctx);

    bool run(ASTNode* root);

    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

private:
    bool buildValueTypeLayout(Type* type);
    bool isPrimitiveType(Type* type);

    const AggregateLayout* buildLayout(AggregateType* type);

    CompilerContext& mCtx;
    std::unordered_set<AggregateType*> mInProgress;
};

} // namespace simlang
