#pragma once

#include <functional>

#include "ast/astwalker.h"
#include "module/modulestage.h"

namespace simlang
{

struct CompilerContext;
struct ModuleEntry;

class ImportVisitor : public ASTWalker<ImportVisitor>
{
public:
    using ProcessorCallbackFn = std::function<bool(ModuleEntry*, ModuleStage)>;

    explicit ImportVisitor(CompilerContext& ctx, ProcessorCallbackFn processorCallback);

    bool run(ASTNode* node);

    bool visitImportDeclarationStatement(ImportDeclarationStatementNode* node);
    bool visitTranslationUnit(TranslationUnitNode* node);

private:
    CompilerContext& mCtx;

    ModuleEntry* mCurrentModule = nullptr;
    ProcessorCallbackFn mProcessorCallback = nullptr;
};

} // namespace simlang
