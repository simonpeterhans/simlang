#include "driver/frontenddriver.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/nodes/astnode.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "driver/compilerlog.h"
#include "driver/phaselogger.h"
#include "module/moduleentry.h"
#include "module/modulemanager.h"
#include "module/modulestage.h"
#include "parser/parser.h"
#include "parser/parsercontext.h"
#include "parser/tokenizer.h"
#include "sema/passes/constfoldvisitor.h"
#include "sema/passes/declcollectorvisitor.h"
#include "sema/passes/implicitthisvisitor.h"
#include "sema/passes/importvisitor.h"
#include "sema/passes/resolutionvisitor.h"
#include "sema/passes/typecheckvisitor.h"
#include "sema/scopes.h"
#include "source/source.h"
#include "source/sourcemanager.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

FrontendDriver::FrontendDriver(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool FrontendDriver::setSource(std::filesystem::path path) const
{
    // Register the source file as a module.
    const ModuleEntry* e = mCtx.mModules.getOrRegisterModule(std::move(path), cInvalidSourceRange);
    // If something went wrong, the error was already reported.
    return (e != nullptr);
}

void FrontendDriver::setRoot(const std::filesystem::path& root) const
{
    mCtx.mModules.setRoot(root);
}

bool FrontendDriver::run()
{
    if (processAllToStage(ModuleStage::cParsed) == false)
    {
        return false;
    }

    if (processAllToStage(ModuleStage::cDeclsCollected) == false)
    {
        return false;
    }

    // Note that this discovers new modules, which will be processed up to decl collection.
    if (processAllToStage(ModuleStage::cImportsBound) == false)
    {
        return false;
    }

    if (processAllToStage(ModuleStage::cNamesResolved) == false)
    {
        return false;
    }

    if (processAllToStage(ModuleStage::cThisRewritten) == false)
    {
        return false;
    }

    if (processAllToStage(ModuleStage::cTypesChecked) == false)
    {
        return false;
    }

    if (processAllToStage(ModuleStage::cConstsFolded) == false)
    {
        return false;
    }

    emitStats();

    return true;
}

bool FrontendDriver::doParse(ModuleEntry* module)
{
    static constexpr const char* cFileLoadingPhaseName = "Loading files";
    static constexpr const char* cParsingPhaseName = "Parsing";

    bool collectStats = mCtx.mLog.isEnabled();
    std::string sourcePath = module->mPath.string();

    auto loadPhase = mCtx.mLog.getPhaseLogger(cFileLoadingPhaseName);

    // Add the source to the source manager.
    SourceLoadResult loadResult = mCtx.mSources.addSource(sourcePath);
    if (loadResult.hasError())
    {
        switch (loadResult.mError)
        {
            case SourceLoadError::cSourceTooLong:
            {
                mCtx.report<cSourceTooLong>(cInvalidSourceRange, sourcePath);
                break;
            }
            case SourceLoadError::cFailedToOpen:
            {
                mCtx.report<cFailedToOpenSource>(cInvalidSourceRange, sourcePath);
                break;
            }
            case SourceLoadError::cFailedToRead:
            {
                mCtx.report<cFailedToReadSource>(cInvalidSourceRange, sourcePath);
                break;
            }
            case SourceLoadError::cNone:
            default:
            {
                break;
            }
        }

        return loadPhase.finish(false);
    }

    loadPhase.finish(true);

    if (collectStats)
    {
        ++mStats.mFilesLoaded;
        mStats.mSourceBytes += loadResult.mSource->getSourceLength();
    }

    // Do the parsing.
    auto parsePhase = mCtx.mLog.getPhaseLogger(cParsingPhaseName);

    ParserContext parseCtx = mCtx.makeParserContext();
    Tokenizer tokenizer{parseCtx, *loadResult.mSource};
    Parser parser{parseCtx, tokenizer};

    TranslationUnitNode* ast = nullptr;
    if (parser.parseCode(ast) == false)
    {
        return parsePhase.finish(false);
    }

    if (collectStats)
    {
        mStats.mTokensScanned += tokenizer.getScannedTokenCount();
        mStats.mASTNodesCreated += parseCtx.mCreatedASTNodeCount;
    }

    ast->mModuleEntry = module;
    module->mAST = ast;
    module->mStage = ModuleStage::cParsed;

    return parsePhase.finish(true);
}

bool FrontendDriver::doDeclarations(ModuleEntry* module)
{
    static constexpr const char* cPhaseName = "Collecting declarations";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    DeclCollectorVisitor visitor{mCtx};
    if (visitor.run(module->mAST) == false)
    {
        return phase.finish(false);
    }

    // Build the export scope.
    // This includes every top-level declaration marked with "export".
    Scope* exportScope = mCtx.mScopes.createScope(nullptr);
    for (ASTNode* node : module->mAST->mNodes)
    {
        if (node == nullptr)
        {
            continue;
        }

        Symbol* symbol = nullptr;
        switch (node->mNodeType)
        {
            case NodeType::cVariableDeclarationStatement:
            {
                symbol = static_cast<VariableDeclarationStatementNode*>(node)->mSymbol;
                break;
            }
            case NodeType::cFunctionDeclarationStatement:
            {
                symbol = static_cast<FunctionDeclarationStatementNode*>(node)->mSymbol;
                break;
            }
            case NodeType::cTypeDeclarationStatement:
            {
                symbol = static_cast<TypeDeclarationStatementNode*>(node)->mSymbol;
                break;
            }
            default:
            {
                break;
            }
        }

        if (symbol != nullptr && symbol->mFlags.test(SymbolFlags::cExport))
        {
            exportScope->addSymbol(symbol);
        }
    }

    module->mExportScope = exportScope;
    module->mModuleSymbol->mScope = exportScope;
    module->mStage = ModuleStage::cDeclsCollected;

    return phase.finish(true);
}

bool FrontendDriver::doImports(ModuleEntry* module)
{
    static constexpr const char* cPhaseName = "Binding imports";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    // We want to make sure that the imports only get resolved once, so we mark the module as in progress.
    ScopedValueBinder inProgress{module->mInProgress, true};

    // The import visitor needs a callback to process newly discovered modules (since we need their declarations).
    ImportVisitor visitor(mCtx,
                          [this](ModuleEntry* m, ModuleStage s)
                          {
                              return this->processToStage(m, s);
                          });

    if (visitor.run(module->mAST) == false)
    {
        return phase.finish(false);
    }

    module->mStage = ModuleStage::cImportsBound;

    return phase.finish(true);
}

bool FrontendDriver::doResolve(ModuleEntry* module)
{
    static constexpr const char* cPhaseName = "Resolving names";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    ResolutionVisitor visitor{mCtx};
    if (visitor.run(module->mAST) == false)
    {
        return phase.finish(false);
    }

    module->mStage = ModuleStage::cNamesResolved;

    return phase.finish(true);
}

bool FrontendDriver::doThisRewrite(ModuleEntry* module)
{
    static constexpr const char* cPhaseName = "Rewriting implicit this";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    ImplicitThisVisitor visitor{mCtx};
    if (visitor.run(module->mAST) == false)
    {
        return phase.finish(false);
    }

    module->mStage = ModuleStage::cThisRewritten;

    return phase.finish(true);
}

bool FrontendDriver::doTypeCheck(ModuleEntry* module)
{
    static constexpr const char* cPhaseName = "Checking types";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    TypeCheckVisitor visitor{mCtx};
    if (visitor.run(module->mAST) == false)
    {
        return phase.finish(false);
    }

    module->mStage = ModuleStage::cTypesChecked;

    return phase.finish(true);
}

bool FrontendDriver::doConstFolding(ModuleEntry* module)
{
    static constexpr const char* cPhaseName = "Folding constants";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    ConstFoldVisitor visitor{mCtx};
    if (visitor.run(module->mAST) == false)
    {
        return phase.finish(false);
    }

    module->mStage = ModuleStage::cConstsFolded;

    return phase.finish(true);
}

void FrontendDriver::emitStats()
{
    static constexpr const char* cFileLoadingPhaseName = "Loading files";
    static constexpr const char* cParsingPhaseName = "Parsing";

    if (mCtx.mLog.isEnabled() == false)
    {
        return;
    }

    mCtx.mLog.addPhaseDetail(cFileLoadingPhaseName, "loaded " + std::to_string(mStats.mFilesLoaded) + " files");
    mCtx.mLog.addPhaseDetail(cFileLoadingPhaseName, "read " + std::to_string(mStats.mSourceBytes) + " source bytes");

    mCtx.mLog.addPhaseDetail(cParsingPhaseName, "scanned " + std::to_string(mStats.mTokensScanned) + " tokens");
    mCtx.mLog.addPhaseDetail(cParsingPhaseName, "created " + std::to_string(mStats.mASTNodesCreated) + " nodes");
}

bool FrontendDriver::processToStage(ModuleEntry* module, ModuleStage target)
{
    // Do nothing if this is already being handled (imports!).
    if (module->mInProgress)
    {
        return true;
    }

    // Otherwise, advance until done.
    while (static_cast<u8>(module->mStage) < static_cast<u8>(target))
    {
        switch (module->mStage)
        {
            case ModuleStage::cCreated:
            {
                if (doParse(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cParsed:
            {
                if (doDeclarations(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cDeclsCollected:
            {
                if (doImports(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cImportsBound:
            {
                if (doResolve(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cNamesResolved:
            {
                if (doThisRewrite(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cThisRewritten:
            {
                if (doTypeCheck(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cTypesChecked:
            {
                if (doConstFolding(module) == false)
                {
                    return false;
                }
                break;
            }
            case ModuleStage::cConstsFolded:
            {
                break;
            }
        }
    }

    return true;
}

bool FrontendDriver::processAllToStage(ModuleStage target)
{
    auto& modules = mCtx.mModules.getModules();
    ModuleStage currentStage = ModuleStage::cCreated;

    // We want everything to be processed up to the target stage.
    while (static_cast<u8>(currentStage) < static_cast<u8>(target))
    {
        auto nextStage = static_cast<ModuleStage>(static_cast<u8>(currentStage) + 1);

        // Now the tricky part here is that the module list can change during processing.
        // Thus, we check for .size() so we can detect newer modules.
        usize next = 0;
        while (next < modules.size())
        {
            if (processToStage(modules[next].get(), nextStage) == false)
            {
                return false;
            }
            ++next;
        }

        currentStage = nextStage;
    }

    return true;
}

} // namespace simlang
