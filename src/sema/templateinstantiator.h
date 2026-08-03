#pragma once

#include <vector>

namespace simlang
{

class SourceRange;
struct CompilerContext;
struct Symbol;
struct TranslationUnitNode;
struct Type;
struct TypeDeclarationStatementNode;

class TemplateInstantiator
{
public:
    explicit TemplateInstantiator(CompilerContext& ctx);

    TypeDeclarationStatementNode* instantiateType(Symbol* templateSymbol,
                                                  const std::vector<Type*>& args,
                                                  SourceRange useRange,
                                                  TranslationUnitNode* outputTU) const;

private:
    CompilerContext& mCtx;
};

} // namespace simlang
