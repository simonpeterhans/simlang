#include "sema/passes/importvisitor.h"

#include <utility>

#include "ast/nodes/astnode.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "module/moduleentry.h"
#include "module/modulemanager.h"
#include "sema/scopes.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symbolutils.h"
#include "util/arrayview.h"
#include "util/scoping.h"

namespace simlang
{

using ModuleScope = ScopedValueBinder<ModuleEntry*>;

static void reportDuplicateSymbol(CompilerContext& ctx, SourceRange range, Identifier* identifier, Symbol* previous)
{
    auto diag = ctx.report<cSymbolAlreadyDefined>(range, identifier);

    SourceRange previousRange = getSymbolSourceRange(previous);
    if (previousRange.isValid())
    {
        diag.note<cPreviousDefinition>(previousRange);
    }
}

ImportVisitor::ImportVisitor(CompilerContext& ctx, ProcessorCallbackFn callback)
    : mCtx(ctx)
    , mProcessorCallback(std::move(callback))
{
}

bool ImportVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool ImportVisitor::visitImportDeclarationStatement(ImportDeclarationStatementNode* node)
{
    // If we have an import, we look up stuff and add it to this TUs scope.
    // Make sure the module exists, is registered, and processed up to declaration collection.
    ModuleEntry* module;
    if (node->mIsRelative)
    {
        module = mCtx.mModules.getOrRegisterModuleRelative(node->mPath, mCurrentModule, node->mSourceRange);
    }
    else
    {
        module = mCtx.mModules.getOrRegisterModule(node->mPath, node->mSourceRange);
    }

    if (module == nullptr)
    {
        // Diag was already done.
        return true;
    }

    node->mResolvedModule = module;

    // Make sure the decls are collected since we might need them later here.
    bool callbackResult = (mProcessorCallback)(module, ModuleStage::cDeclsCollected);
    if (callbackResult == false)
    {
        return false;
    }

    // Bind the export scope to the module symbol so qualified access uses only exported names.
    module->mModuleSymbol->mScope = module->mExportScope;

    // If we have an "as" name, use that. Otherwise, use the default local name.
    // The module has to exist at this point and so mPath.back() should always be valid.
    Identifier* asName = (node->mAlias != nullptr) ? node->mAlias : node->mPath.back();

    // Always bind the module symbol so qualified access via the alias works.
    // This means that if we do "import a as b", exported names from a are accessible via b::....
    // When looking up scoped stuff like b::D, we get the module symbol (b in this example).
    // Then, its export scope is used for the member lookup.
    if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(asName))
    {
        reportDuplicateSymbol(mCtx, node->mSourceRange, asName, previous);
        return true;
    }

    mCtx.mScopes.addSymbol(module->mModuleSymbol, asName);

    // If we import specific things, do that.
    for (ImportSelectedEntry* ise : node->mSelected)
    {
        // If we have that, use that, otherwise use the normal name.
        Identifier* importedName = (ise->mAlias != nullptr) ? ise->mAlias : ise->mName;

        Symbol* s = module->mExportScope->getSymbol(ise->mName);
        if (s == nullptr)
        {
            // If we didn't find it in the export scope, do some additional diag.
            // Perhaps it was private, so look that up.
            Symbol* privateSymbol = nullptr;
            if (module->mAST != nullptr && module->mAST->mScope != nullptr)
            {
                privateSymbol = module->mAST->mScope->getSymbol(ise->mName);
            }

            if (privateSymbol != nullptr)
            {
                // If it wasn't exported, add the diag and a note.
                auto diag = mCtx.report<cImportedSymbolNotExported>(node->mSourceRange, ise->mName);
                SourceRange symbolRange = getSymbolSourceRange(privateSymbol);
                if (symbolRange.isValid())
                {
                    diag.note<cSymbolDeclaredHere>(symbolRange, ise->mName).hint<cAddExportSpecifierHint>(symbolRange);
                }
            }
            else
            {
                // If it simply doesn't exist, complain.
                mCtx.report<cImportedSymbolNotFound>(node->mSourceRange, ise->mName);
            }
            continue;
        }

        // If it already exists in our scope, complain as well.
        if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(importedName))
        {
            reportDuplicateSymbol(mCtx, node->mSourceRange, importedName, previous);
            continue;
        }

        // Finally add it to our scope.
        mCtx.mScopes.addSymbol(s, importedName);
    }

    return true;
}

bool ImportVisitor::visitTranslationUnit(TranslationUnitNode* node)
{
    // Register the current module so relative imports can use that.
    ModuleScope ms{mCurrentModule, node->mModuleEntry};

    // We want to use the TU scope for the imports, so bind that.
    ScopeGuard sg{mCtx.mScopes, node->mScope};

    for (ASTNode* n : node->mNodes)
    {
        if (n->mNodeType == NodeType::cImportDeclarationStatement)
        {
            if (visit(n) == false)
            {
                return false;
            }
        }
    }

    return true;
}

} // namespace simlang
