#pragma once

#include <vector>

#include "ast/astwalker.h"
#include "util/types.h"

namespace simlang
{

struct AggregateType;
struct CompilerContext;
struct Type;

class TypeLayoutTableVisitor : public ASTWalker<TypeLayoutTableVisitor>
{
public:
    explicit TypeLayoutTableVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);

    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

private:
    bool collectRefsForAggregate(AggregateType* agg, u32 baseWords, std::vector<u32>& fieldOffsets);
    bool collectRefsForField(Type* type, u32 baseOffsetWords, std::vector<u32>& fieldOffsets);

    CompilerContext& mCtx;
};

} // namespace simlang
