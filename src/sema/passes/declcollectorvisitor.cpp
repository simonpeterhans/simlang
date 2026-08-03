#include "sema/passes/declcollectorvisitor.h"

#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "module/moduleentry.h"
#include "sema/scopes.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symbolregistry.h"
#include "symbol/symboltype.h"
#include "symbol/symbolutils.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/scoping.h"

namespace simlang
{

using ModuleScope = ScopedValueBinder<ModuleEntry*>;
using SymbolScope = ScopedValueBinder<Symbol*>;
using MemberListScope = ScopedValueBinder<std::vector<Symbol*>*>;

static void reportDuplicateSymbol(CompilerContext& ctx, SourceRange range, Identifier* identifier, Symbol* previous)
{
    auto diag = ctx.report<cSymbolAlreadyDefined>(range, identifier);

    SourceRange previousRange = getSymbolSourceRange(previous);
    if (previousRange.isValid())
    {
        diag.note<cPreviousDefinition>(previousRange);
    }
}

DeclCollectorVisitor::DeclCollectorVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool DeclCollectorVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool DeclCollectorVisitor::visitBlockStatement(BlockStatementNode* node)
{
    // Enter a new scope for the block.
    ScopeGuard sg{mCtx.mScopes};

    // Assign that to the block node.
    node->mScope = mCtx.mScopes.getCurrentScope();

    // Visit all the statements in the block.
    for (StatementNode* stmt : node->mStatements)
    {
        if (visit(stmt) == false)
        {
            return false;
        }
    }

    return true;
}

bool DeclCollectorVisitor::visitForStatement(ForStatementNode* node)
{
    // Enter a new scope for the for loop.
    ScopeGuard sg{mCtx.mScopes};

    // Assign that to the for node.
    node->mScope = mCtx.mScopes.getCurrentScope();

    // Visit the init statement if we have one.
    if (node->mInit != nullptr && visit(node->mInit) == false)
    {
        return false;
    }

    // Skip condition (no work to do).
    // Skip increment (no work to do).

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool DeclCollectorVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    SymbolType symbolType;
    bool isTypeMember;
    if (mCurrentSymbol == nullptr)
    {
        // If we're not inside a struct or class declaration, this is a global variable.
        isTypeMember = false;
        symbolType = SymbolType::cGlobalVariable;
    }
    else if (mCurrentSymbol->mSymbolType == SymbolType::cStruct || mCurrentSymbol->mSymbolType == SymbolType::cClass)
    {
        // Struct/class variables are instance fields.
        isTypeMember = true;
        symbolType = SymbolType::cMemberVariable;
    }
    else
    {
        // Local variables are resolved after imports.
        return true;
    }

    // Check if this already exists.
    if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(node->mIdentifier))
    {
        reportDuplicateSymbol(mCtx, node->mIdentifierRange, node->mIdentifier, previous);
        return true;
    }

    // Otherwise, we're good to go and can create a new symbol.
    Symbol* symbol = mCtx.mSymbols.createSymbol(symbolType);
    symbol->mIdentifier = node->mIdentifier;
    symbol->mDeclNode = node;
    if (isTypeMember)
    {
        // This is a member variable.
        symbol->mFlags.set(SymbolFlags::cPrivate, node->mFlags.test(cStmtIsPrivate));
    }
    else
    {
        // This is a global variable.
        symbol->mFlags.set(SymbolFlags::cExport, node->mFlags.test(cStmtIsExported));
    }

    // The type may not be known yet at this time so we can't resolve it yet.

    // Set the symbol in the node.
    node->mSymbol = symbol;

    // Add the symbol to the current scope.
    mCtx.mScopes.addSymbol(node->mSymbol);

    // Add the symbol to the current type if it's a member.
    if (isTypeMember)
    {
        mCurrentMemberList->push_back(symbol);
    }

    // The initializer will be resolved later.

    return true;
}

bool DeclCollectorVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // Functions have to either be on global level or inside a type.
    if (mCurrentSymbol != nullptr && mCurrentSymbol->mSymbolType != SymbolType::cStruct &&
        mCurrentSymbol->mSymbolType != SymbolType::cClass && mCurrentSymbol->mSymbolType != SymbolType::cInterface)
    {
        mCtx.report<cInvalidFunctionDeclarationScope>(node->mIdentifierRange, node->mIdentifier);
        return true;
    }

    if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(node->mIdentifier))
    {
        reportDuplicateSymbol(mCtx, node->mIdentifierRange, node->mIdentifier, previous);
        return true;
    }

    // Otherwise, we're good to go and can create a new symbol.
    SymbolType symbolType;
    bool isTypeMember;
    if (mCurrentSymbol == nullptr)
    {
        // If we're not inside an aggregate, this is a free function.
        symbolType = SymbolType::cFunction;
        isTypeMember = false;
    }
    else
    {
        // Type and level validity were already checked, so this is a member function.
        symbolType = SymbolType::cMemberFunction;
        isTypeMember = true;
    }

    Symbol* symbol = mCtx.mSymbols.createSymbol(symbolType);
    symbol->mIdentifier = node->mIdentifier;
    symbol->mDeclNode = node;
    if (isTypeMember)
    {
        symbol->mFlags.set(SymbolFlags::cPrivate, node->mFlags.test(cStmtIsPrivate));
        symbol->mFlags.set(SymbolFlags::cImpl, node->mFlags.test(cStmtIsInterfaceImpl));
    }
    else
    {
        symbol->mFlags.set(SymbolFlags::cExport, node->mFlags.test(cStmtIsExported));
    }

    // Set the symbol in the node.
    node->mSymbol = symbol;

    // Before we enter a new scope, add the symbol to the current type if it's a member.
    if (isTypeMember)
    {
        mCurrentMemberList->push_back(symbol);
    }

    // Add the symbol to the current scope.
    mCtx.mScopes.addSymbol(symbol);

    // We're currently in this function, so bind the symbol and create a scope.
    SymbolScope cs{mCurrentSymbol, symbol};
    ScopeGuard sg{mCtx.mScopes};

    // We need to remember the scope, so assign it to the node and symbol.
    node->mScope = mCtx.mScopes.getCurrentScope();
    node->mSymbol->mScope = node->mScope;

    // We can ignore the return type for now as it will be resolved later.
    // We also ignore the params for now for the same reason.

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool DeclCollectorVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    if (node->mDeclModule == nullptr)
    {
        node->mDeclModule = mCurrentModule;
    }

    // This is only allowed if we're not already in a type (or in anything really).
    if (mCurrentSymbol != nullptr)
    {
        mCtx.report<cInvalidTypeDeclarationScope>(node->mIdentifierRange, node->mIdentifier);
        return true;
    }

    // Check if a symbol with this name already exists in our scope.
    // We only need to do this if this is not a type instantiated from a template.
    bool isTemplateInstance = node->mFlags.test(cStmtIsTemplateInstance);
    if (isTemplateInstance == false)
    {
        if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(node->mIdentifier))
        {
            reportDuplicateSymbol(mCtx, node->mIdentifierRange, node->mIdentifier, previous);
            return true;
        }
    }

    // Otherwise, we're good to go and can create a new symbol.
    // If this node is the actual template declaration, register it as such instead of doing the usual stuff.
    if (node->isTemplate())
    {
        Symbol* symbol = mCtx.mSymbols.createSymbol(SymbolType::cTypeTemplate);
        symbol->mIdentifier = node->mIdentifier;
        symbol->mDeclNode = node;
        symbol->mScope = mCtx.mScopes.getCurrentScope();
        symbol->mFlags.set(SymbolFlags::cExport, node->mFlags.test(cStmtIsExported));

        node->mSymbol = symbol;
        node->mScope = symbol->mScope;

        mCtx.mScopes.addSymbol(node->mSymbol);

        return true;
    }

    // Create the corresponding symbol type.
    SymbolType symbolType = SymbolType::cStruct;
    if (node->isInterface())
    {
        symbolType = SymbolType::cInterface;
    }
    else if (node->isClass())
    {
        symbolType = SymbolType::cClass;
    }

    Symbol* symbol = mCtx.mSymbols.createSymbol(symbolType);
    symbol->mIdentifier = node->mIdentifier;
    symbol->mDeclNode = node;
    if (isTemplateInstance == false)
    {
        symbol->mFlags.set(SymbolFlags::cExport, node->mFlags.test(cStmtIsExported));
    }
    // We can also create the type since this is the declaring symbol.
    symbol->mType = node->isInterface() ? static_cast<Type*>(mCtx.mTypes.getOrAddInterfaceType(symbol))
                                        : static_cast<Type*>(mCtx.mTypes.getOrAddAggregateType(symbol));

    // Set the symbol in the node.
    node->mSymbol = symbol;

    // If this is a type instantiated from a template, we don't add it to the current scope.
    // For instantiated types, the lookup is done differently (to see if a new generic instantiation has to be created).
    if (isTemplateInstance == false)
    {
        mCtx.mScopes.addSymbol(node->mSymbol);
    }

    // Bind the current symbol.
    SymbolScope cs{mCurrentSymbol, symbol};

    // Create a scope.
    ScopeGuard sg{mCtx.mScopes};
    // We need to remember the scope, so assign it to the node and symbol.
    node->mScope = mCtx.mScopes.getCurrentScope();
    node->mSymbol->mScope = node->mScope;

    // Member decls will add their symbol to this list.
    std::vector<Symbol*> members;
    MemberListScope ms{mCurrentMemberList, &members};

    // Process the member declarations.
    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    // Store the stuff in the member list of the symbol.
    symbol->mMembers = makeArrayView(mCtx.mAllocator, members);

    // Find the initializer across the collected member list.
    if (node->isClass())
    {
        auto* classType = static_cast<AggregateType*>(symbol->mType);

        for (Symbol* member : symbol->mMembers)
        {
            if (member->mSymbolType != SymbolType::cMemberFunction)
            {
                continue;
            }

            auto* fun = static_cast<FunctionDeclarationStatementNode*>(member->mDeclNode);
            if (fun->mIsInitMethod)
            {
                classType->mInitMethodSymbol = member;
                break;
            }
        }
    }

    return true;
}

bool DeclCollectorVisitor::visitTranslationUnit(TranslationUnitNode* node)
{
    // Create a scope and bind it.
    // A good thing to remember is that imports bind to this scope.
    // This can quickly be confused with module scopes, to which imports do not bind.
    // (As you may have guessed, this is because module scopes don't share imports.)
    Scope* tuScope = mCtx.mScopes.createScope(mCtx.mScopes.getRootScope());
    ScopeGuard sg{mCtx.mScopes, tuScope};
    ModuleScope ms{mCurrentModule, node->mModuleEntry};
    node->mScope = tuScope;

    for (ASTNode* stmt : node->mNodes)
    {
        if (visit(stmt) == false)
        {
            return false;
        }
    }

    return true;
}

} // namespace simlang
