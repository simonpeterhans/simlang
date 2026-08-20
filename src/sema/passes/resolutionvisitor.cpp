#include "sema/passes/resolutionvisitor.h"

#include <string_view>
#include <vector>

#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/scopes.h"
#include "sema/templateinstantiator.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symbolregistry.h"
#include "symbol/symboltype.h"
#include "symbol/symbolutils.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

using SymbolScope = ScopedValueBinder<Symbol*>;

static constexpr std::string_view cListTypeName = "list";
static constexpr std::string_view cMapTypeName = "map";

static void reportDuplicateSymbol(CompilerContext& ctx, SourceRange range, Identifier* identifier, Symbol* previous)
{
    auto diag = ctx.report<cSymbolAlreadyDefined>(range, identifier);

    SourceRange previousRange = getSymbolSourceRange(previous);
    if (previousRange.isValid())
    {
        diag.note<cPreviousDefinition>(previousRange);
    }
}

ResolutionVisitor::ResolutionVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool ResolutionVisitor::run(TranslationUnitNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    mCurrentTranslationUnit = node;
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool ResolutionVisitor::visitIdentifier(IdentifierNode* node)
{
    // Look it up in the current scopes.
    if (Symbol* symbol = mCtx.mScopes.getSymbolRecursive(node->mIdentifier))
    {
        node->mSymbol = symbol;

        if (mCurrentFieldDefault != nullptr)
        {
            // If we're currently inside of a field default expression, restrict access to other fields (or methods).
            if (symbol->mSymbolType == SymbolType::cMemberVariable ||
                symbol->mSymbolType == SymbolType::cMemberFunction)
            {
                mCtx.report<cInvalidFieldDefaultReference>(node->mSourceRange,
                                                           mCurrentFieldDefault->mIdentifier,
                                                           symbol->mIdentifier);
            }
        }

        return true;
    }

    // Otherwise this is an undefined identifier.
    mCtx.report<cUndefinedIdentifier>(node->mSourceRange, node->mIdentifier);
    return true;
}

bool ResolutionVisitor::visitThis(ThisNode* node)
{
    // If we're not inside a method, this is an error.
    if (mCurrentSymbol == nullptr || mCurrentSymbol->mSymbolType != SymbolType::cMemberFunction)
    {
        mCtx.report<cThisOutsideMethod>(node->mSourceRange);
    }

    return true;
}

bool ResolutionVisitor::visitModuleAccess(ModuleAccessNode* node)
{
    // For module access, the RHS is resolved against the module export scope.
    if (visit(node->mLeft) == false)
    {
        return false;
    }

    Symbol* lhsSymbol = nullptr;
    switch (node->mLeft->mNodeType)
    {
        case NodeType::cIdentifier:
        {
            lhsSymbol = static_cast<IdentifierNode*>(node->mLeft)->mSymbol;
            break;
        }
        case NodeType::cModuleAccess:
        {
            lhsSymbol = static_cast<ModuleAccessNode*>(node->mLeft)->mSymbol;
            break;
        }
        default:
        {
            mCtx.report<cInvalidModuleAccess>(node->mLeft->mSourceRange);
            return true;
        }
    }

    if (lhsSymbol == nullptr)
    {
        // If something went wrong with the LHS symbol resolution, bail.
        return true;
    }

    if (node->mRight->mNodeType != NodeType::cIdentifier)
    {
        mCtx.report<cInvalidModuleAccess>(node->mRight->mSourceRange);
        return true;
    }

    auto* rhs = static_cast<IdentifierNode*>(node->mRight);

    switch (lhsSymbol->mSymbolType)
    {
        case SymbolType::cModule:
        {
            if (lhsSymbol->mScope == nullptr)
            {
                mCtx.report<cInvalidModuleAccess>(node->mLeft->mSourceRange);
                return true;
            }

            Symbol* rhsSymbol = lhsSymbol->mScope->getSymbol(rhs->mIdentifier);
            if (rhsSymbol == nullptr)
            {
                mCtx.report<cUndefinedIdentifier>(rhs->mSourceRange, rhs->mIdentifier);
                return true;
            }

            rhs->mSymbol = rhsSymbol;
            node->mSymbol = rhs->mSymbol;
            return true;
        }

        default:
        {
            mCtx.report<cNotAModule>(node->mLeft->mSourceRange, lhsSymbol->mIdentifier);
            return true;
        }
    }
}

bool ResolutionVisitor::visitBlockStatement(BlockStatementNode* node)
{
    // Apply the scope for this block.
    ScopeGuard sg{mCtx.mScopes, node->mScope};

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

bool ResolutionVisitor::visitForStatement(ForStatementNode* node)
{
    // Apply the scope for this for loop.
    ScopeGuard sg{mCtx.mScopes, node->mScope};

    // Visit the init statement.
    if (node->mInit != nullptr && visit(node->mInit) == false)
    {
        return false;
    }

    // Visit the condition.
    if (node->mCondition != nullptr && visit(node->mCondition) == false)
    {
        return false;
    }

    // Visit the increment.
    if (node->mIncrement != nullptr && visit(node->mIncrement) == false)
    {
        return false;
    }

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool ResolutionVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    // If we have no symbol yet, this must be a local variable.
    if (node->mSymbol == nullptr)
    {
        // Create that here.
        if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(node->mIdentifier))
        {
            reportDuplicateSymbol(mCtx, node->mIdentifierRange, node->mIdentifier, previous);
            return true;
        }

        Symbol* s = mCtx.mSymbols.createSymbol(SymbolType::cStackVariable);
        s->mIdentifier = node->mIdentifier;
        s->mDeclNode = node;

        // Set the variable in the symbol.
        node->mSymbol = s;

        // Add the symbol to the current scope.
        mCtx.mScopes.addSymbol(s);
    }

    // Mark the symbol as mutable if it was declared as such.
    if (node->mFlags.test(cStmtIsMutable))
    {
        node->mSymbol->mFlags.set(SymbolFlags::cMutable, true);
    }

    // Visit the type specifier if we have one.
    if (node->mTypeSpec != nullptr)
    {
        if (visit(node->mTypeSpec) == false)
        {
            return false;
        }

        // Resolve the type right away.
        node->mSymbol->mType = node->mTypeSpec->mType;
    }

    // Finally, visit the init expression if we have one.
    if (node->mInit != nullptr)
    {
        // If we are declaring a member variable, we need to indicate that.
        // The subsequent expression of the initializer cannot refer to other fields (or methods).
        Symbol* fieldDefault = (node->mSymbol->mSymbolType == SymbolType::cMemberVariable) ? node->mSymbol : nullptr;
        ScopedValueBinder fieldDefaultScope{mCurrentFieldDefault, fieldDefault};

        if (visit(node->mInit) == false)
        {
            return false;
        }
    }

    return true;
}

bool ResolutionVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // We're currently in this function, so bind the symbol and scope.
    // The actual function type is only created during type checking.
    ScopeGuard sg{mCtx.mScopes, node->mScope};
    SymbolScope cs{mCurrentSymbol, node->mSymbol};

    // For non-initializers, resolve the return type.
    if (node->mIsInitMethod == false)
    {
        if (visit(node->mReturnTypeSpec) == false)
        {
            return false;
        }
    }

    // Process the params.
    for (ParamNode* param : node->mParams)
    {
        if (visit(param) == false)
        {
            return false;
        }
    }

    // Also process the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    return true;
}

bool ResolutionVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If the type declaration is a template, do nothing.
    // Derived types will be processed separately (flagged cStmtIsTemplateInstance).
    if (node->isTemplate())
    {
        return true;
    }

    // We're currently in this type, so bind the symbol and scope.
    ScopeGuard sg{mCtx.mScopes, node->mScope};
    SymbolScope cs{mCurrentSymbol, node->mSymbol};

    // Process the interfaces that this type is implementing.
    for (TypeSpecifierNode* iface : node->mImplementedInterfaces)
    {
        if (visit(iface) == false)
        {
            return false;
        }
    }

    // Process the members.
    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    return true;
}

bool ResolutionVisitor::visitParamDeclaration(ParamDeclarationNode* node)
{
    // Check if this already exists.
    if (Symbol* previous = mCtx.mScopes.getSymbolRecursive(node->mIdentifier))
    {
        reportDuplicateSymbol(mCtx, node->mIdentifierRange, node->mIdentifier, previous);
        return true;
    }

    // Otherwise, we're good to go and can create a new symbol.
    Symbol* symbol = mCtx.mSymbols.createSymbol(SymbolType::cParameter);
    symbol->mIdentifier = node->mIdentifier;
    // For now, params are mutable.
    symbol->mFlags.set(SymbolFlags::cMutable, true);
    symbol->mFlags.set(SymbolFlags::cInOut, node->mIsInOut);

    // Add the symbol to the current scope.
    mCtx.mScopes.addSymbol(symbol);

    // Set the symbol in the node.
    node->mSymbol = symbol;

    // Visit the type node to figure out what we have there.
    if (node->mTypeSpec != nullptr && visit(node->mTypeSpec) == false)
    {
        return false;
    }

    // Set the type based on what we got.
    Type* type = (node->mTypeSpec == nullptr) ? nullptr : node->mTypeSpec->mType;
    node->mSymbol->mType = type;

    if (node->mDefaultValue != nullptr && visit(node->mDefaultValue) == false)
    {
        return false;
    }

    return true;
}

bool ResolutionVisitor::visitNamedTypeSpecifier(NamedTypeSpecifierNode* node)
{
    // If this type was already resolved earlier, don't do that again.
    if (node->mType != nullptr)
    {
        return true;
    }

    if (isIdentifierExpressionNamed(node->mNameExpression, cListTypeName))
    {
        if (node->mTypeArgs.size() != 1)
        {
            mCtx.report<cWrongTemplateArgumentCount>(node->mSourceRange, 1, node->mTypeArgs.size());
            node->mType = mCtx.mTypes.getErrorType();
            return true;
        }

        TypeSpecifierNode* elementTypeSpec = node->mTypeArgs[0];
        if (visit(elementTypeSpec) == false)
        {
            return false;
        }

        Type* elementType = elementTypeSpec->mType;
        if (elementType == nullptr)
        {
            return true;
        }

        node->mType = mCtx.mTypes.getOrAddList(elementType);
        return true;
    }

    if (isIdentifierExpressionNamed(node->mNameExpression, cMapTypeName))
    {
        if (node->mTypeArgs.size() != 2)
        {
            mCtx.report<cWrongTemplateArgumentCount>(node->mSourceRange, 2, node->mTypeArgs.size());
            node->mType = mCtx.mTypes.getErrorType();
            return true;
        }

        TypeSpecifierNode* keyTypeSpec = node->mTypeArgs[0];
        TypeSpecifierNode* valueTypeSpec = node->mTypeArgs[1];
        if (visit(keyTypeSpec) == false || visit(valueTypeSpec) == false)
        {
            return false;
        }

        Type* keyType = keyTypeSpec->mType;
        Type* valueType = valueTypeSpec->mType;
        if (keyType == nullptr || valueType == nullptr)
        {
            return true;
        }

        node->mType = mCtx.mTypes.getOrAddMap(keyType, valueType);
        return true;
    }

    // Resolve the name expression.
    if (visit(node->mNameExpression) == false)
    {
        return false;
    }

    // Get the symbol from the node.
    // The name expression must either be an identifier or a module access.
    Symbol* typeSymbol = nullptr;
    switch (node->mNameExpression->mNodeType)
    {
        case NodeType::cIdentifier:
        {
            auto* id = static_cast<IdentifierNode*>(node->mNameExpression);
            typeSymbol = id->mSymbol;
            break;
        }
        case NodeType::cModuleAccess:
        {
            auto* ma = static_cast<ModuleAccessNode*>(node->mNameExpression);
            typeSymbol = ma->mSymbol;
            break;
        }
        default:
        {
            return false;
        }
    }

    // Check if we found the symbol for the type.
    if (typeSymbol == nullptr)
    {
        return true;
    }

    switch (typeSymbol->mSymbolType)
    {
        case SymbolType::cPrimitive:
        case SymbolType::cStruct:
        case SymbolType::cClass:
        case SymbolType::cInterface:
        case SymbolType::cTypeTemplate:
        {
            break;
        }
        default:
        {
            mCtx.report<cSymbolIsNotType>(node->mNameExpression->mSourceRange, typeSymbol->mIdentifier);
            return true;
        }
    }

    if (typeSymbol->mSymbolType != SymbolType::cTypeTemplate)
    {
        // If this is NOT a template and we have template params, this is an error.
        if (node->mTypeArgs.empty() == false)
        {
            mCtx.report<cTemplateArgumentsOnNonTemplateType>(node->mSourceRange, typeSymbol->mIdentifier);
            return true;
        }

        // Otherwise, we're good to go and can set the resolved type.
        node->mType = typeSymbol->mType;

        return true;
    }

    // If we have any template arguments (type specifiers) for this type, we need to resolve them.
    // This means we can get here recursively.
    std::vector<Type*> templateArgs;
    templateArgs.reserve(node->mTypeArgs.size());

    for (TypeSpecifierNode* arg : node->mTypeArgs)
    {
        // Resolve the specifier.
        if (visit(arg) == false)
        {
            return false;
        }

        // Find out if we were successful.
        Type* argType = arg ? arg->mType : nullptr;
        if (argType == nullptr)
        {
            // If we failed previously, we don't do that.
            return true;
        }

        templateArgs.push_back(argType);
    }

    // Instantiate the type if needed and fetch the decl node.
    TemplateInstantiator instantiator{mCtx};
    TypeDeclarationStatementNode* generated =
        instantiator.instantiateType(typeSymbol, templateArgs, node->mSourceRange, mCurrentTranslationUnit);
    if (generated != nullptr && generated->mSymbol != nullptr)
    {
        // Get the symbol and type from the instanced template and use that.
        node->mType = generated->mSymbol->mType;
    }

    return true;
}

bool ResolutionVisitor::visitSubstitutedTypeSpecifier(SubstitutedTypeSpecifierNode*)
{
    // We don't need to do anything here since the type is already resolved.
    return true;
}

bool ResolutionVisitor::visitTranslationUnit(TranslationUnitNode* node)
{
    // We want to use the TU scope (which now should have all imports), so bind that.
    // Note that the scopes should be linked properly, the scope guard just sets the current scope.
    ScopeGuard sg{mCtx.mScopes, node->mScope};

    // This HAS to be an index-based loop -- instantiated templates are appended to the current TU node.
    for (usize i = 0; i < node->mNodes.size(); ++i) // NOLINT(*-loop-convert)
    {
        if (visit(node->mNodes[i]) == false)
        {
            return false;
        }
    }

    return true;
}

} // namespace simlang
