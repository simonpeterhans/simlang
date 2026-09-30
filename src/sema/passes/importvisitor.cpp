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

    if (module == mCurrentModule)
    {
        mCtx.report<cModuleImportsItself>(node->mSourceRange);
        return true;
    }

    // Make sure the decls are collected since we might need them later here.
    bool callbackResult = (mProcessorCallback)(module, ModuleStage::cDeclsCollected);
    if (callbackResult == false)
    {
        return false;
    }

    auto bindSymbol = [&](Symbol* symbol, Identifier* name)
    {
        if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(name))
        {
            // This is allowed if we import the same thing more than once (for now?).
            if (previous != symbol)
            {
                reportDuplicateSymbol(mCtx, node->mSourceRange, name, previous);
            }

            return;
        }

        // Add it to the scope.
        mCtx.mScopes.addSymbol(symbol, name);
    };

    // If we import the entire thing as an alias, add the module symbol to the scope.
    if (node->mAlias != nullptr)
    {
        bindSymbol(module->mModuleSymbol, node->mAlias);
        return true;
    }

    // If we have no alias, import the entire thing directly into the scope.
    // Module symbols have all their top level declarations as members.
    for (Symbol* symbol : module->mModuleSymbol->mMembers)
    {
        bindSymbol(symbol, symbol->mIdentifier);
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
