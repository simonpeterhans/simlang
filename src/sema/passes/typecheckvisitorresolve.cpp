#include <unordered_set>
#include <vector>

#include "ast/nodes/astnode.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/typecheckvisitor.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "type/typekind.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arrayview.h"
#include "util/scoping.h"

namespace simlang
{

bool TypeCheckVisitor::resolveType(Symbol* symbol)
{
    if (symbol->mType != nullptr)
    {
        return true;
    }

    // Check whether we're already resolving this symbol to avoid infinite recursion.
    if (mResolvingSymbols.find(symbol) != mResolvingSymbols.end())
    {
        // Pass the identifier directly; Diagnostic::toParam handles null by using "<unknown identifier>".
        mCtx.report<cCircularTypeDependency>(symbol->mDeclNode->mSourceRange, symbol->mIdentifier);
        symbol->mType = getErrorType();
        return true;
    }

    // Mark as resolving.
    mResolvingSymbols.insert(symbol);
    OnScopeEnd resolvingScope(
        [&]
        {
            mResolvingSymbols.erase(symbol);
        });

    if (symbol->mDeclNode->mNodeType == NodeType::cFunctionDeclarationStatement)
    {
        auto* decl = static_cast<FunctionDeclarationStatementNode*>(symbol->mDeclNode);
        if (resolveFunctionSignature(decl) == false)
        {
            return false;
        }
    }
    else if (symbol->mDeclNode->mNodeType == NodeType::cVariableDeclarationStatement)
    {
        auto* decl = static_cast<VariableDeclarationStatementNode*>(symbol->mDeclNode);

        if (decl->mTypeSpec != nullptr)
        {
            // Resolve the type specifier of the declaration if there is one so we can use it.
            if (visit(decl->mTypeSpec) == false)
            {
                return false;
            }

            decl->mTypeSpec->mType =
                requireValueType(decl->mTypeSpec->mType, decl->mTypeSpec->mSourceRange, "a variable type");
            symbol->mType = decl->mTypeSpec->mType;
        }
        else if (decl->mInit != nullptr)
        {
            // If there is none, try to infer it from the init expression.
            // Since the init expression may contain unresolved identifiers itself, this can recurse.
            if (visit(decl->mInit) == false)
            {
                return false;
            }

            symbol->mType = requireValueType(decl->mInit->mResolvedType, decl->mInit->mSourceRange, "a variable type");
        }
    }

    if (symbol->mType == nullptr)
    {
        symbol->mType = getErrorType();
    }

    return true;
}

bool TypeCheckVisitor::resolveFunctionSignature(FunctionDeclarationStatementNode* node)
{
    // If this type was already resolved, bail.
    if (node->mSymbol->mType != nullptr)
    {
        return true;
    }

    // Return type.
    // Initializers always return void.
    Type* functionReturnType = mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cVoid);
    // For non-initializers, resolve and use the return type spec.
    if (node->mIsInitMethod == false)
    {
        if (visit(node->mReturnTypeSpec) == false)
        {
            return false;
        }

        functionReturnType =
            requireNonNullType(node->mReturnTypeSpec->mType, node->mReturnTypeSpec->mSourceRange, "a return type");
        node->mReturnTypeSpec->mType = functionReturnType;
    }

    // Parameters.
    std::vector<FunctionParam> paramTypes;
    paramTypes.reserve(node->mParams.size());

    for (ParamNode* param : node->mParams)
    {
        if (visit(param) == false)
        {
            return false;
        }

        auto* paramDecl = static_cast<ParamDeclarationNode*>(param);
        Type* paramType = paramDecl->mSymbol->mType;
        if (isErrorType(paramType))
        {
            paramType = getErrorType();
        }

        paramTypes.push_back(FunctionParam{paramType, paramDecl->mIsInOut});
    }

    node->mSymbol->mType = mCtx.mTypes.getOrAddFunction(functionReturnType, paramTypes);

    return true;
}

} // namespace simlang
