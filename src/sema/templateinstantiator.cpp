#include "sema/templateinstantiator.h"

#include <string>

#include "ast/astcloner.h"
#include "ast/nodes/astnode.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/declcollectorvisitor.h"
#include "sema/scopes.h"
#include "sema/templateinstantiationcache.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typeformat.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

class ArenaAllocator;

struct TemplateSubstitution
{
    Identifier* mParam = nullptr;
    Type* mType = nullptr;
};

class TemplateASTCloner : public ASTCloner<TemplateASTCloner>
{
public:
    explicit TemplateASTCloner(ArenaAllocator& allocator, ArrayView<TemplateSubstitution> substitutions)
        : ASTCloner(allocator)
        , mSubstitutions(substitutions)
    {
    }

    TypeSpecifierNode* cloneNamedTypeSpecifier(NamedTypeSpecifierNode* node)
    {
        // For templates, we clone the AST exactly, but we have to replace any type arguments.
        // For a type to be replaceable, it must be a template parameter and have no type arguments.
        if (node->mNameExpression != nullptr && node->mNameExpression->mNodeType == NodeType::cIdentifier &&
            node->mTypeArgs.empty())
        {
            // Then, we get the identifier.
            auto* id = static_cast<IdentifierNode*>(node->mNameExpression);

            // We go through all substitutions to find a match for the identifier.
            for (const TemplateSubstitution& substitution : mSubstitutions)
            {
                if (substitution.mParam == id->mIdentifier)
                {
                    // We already know the type here, so we can simply insert a substituted type node.
                    return cloneNode<SubstitutedTypeSpecifierNode>(node, substitution.mType);
                }
            }
        }

        return ASTCloner::cloneNamedTypeSpecifier(node);
    }

private:
    ArrayView<TemplateSubstitution> mSubstitutions;
};

TemplateInstantiator::TemplateInstantiator(CompilerContext& ctx)
    : mCtx(ctx)
{
}

TypeDeclarationStatementNode* TemplateInstantiator::instantiateType(Symbol* templateSymbol,
                                                                    const std::vector<Type*>& args,
                                                                    SourceRange useRange,
                                                                    TranslationUnitNode* outputTU) const
{
    // To instantiate from a template symbol, it must be a type template with a valid declaration node.
    if (templateSymbol == nullptr || templateSymbol->mSymbolType != SymbolType::cTypeTemplate ||
        templateSymbol->mDeclNode == nullptr ||
        templateSymbol->mDeclNode->mNodeType != NodeType::cTypeDeclarationStatement)
    {
        return nullptr;
    }

    auto* pattern = static_cast<TypeDeclarationStatementNode*>(templateSymbol->mDeclNode);
    // If the number of template arguments doesn't match the number of template parameters, this is an error.
    if (pattern->mTemplateParams.size() != args.size())
    {
        mCtx.report<cWrongTemplateArgumentCount>(useRange, pattern->mTemplateParams.size(), args.size())
            .note<cTemplateDeclaredHere>(pattern->mIdentifierRange, pattern->mIdentifier);
        return nullptr;
    }

    // Lookup: If we already have an instance with the same template symbol and arguments, return it.
    if (TypeDeclarationStatementNode* existing = mCtx.mTemplateCache.find(templateSymbol, args))
    {
        return existing;
    }

    // Otherwise, we need to create a new instance.
    // For this, we first need to generate a name for the instance based on the template symbol and the arguments.
    // This will serve as the identifier for the new instance.
    std::string generatedNameText{templateSymbol->mIdentifier->mName, templateSymbol->mIdentifier->mLength};
    generatedNameText += "<";
    for (usize i = 0; i < args.size(); ++i)
    {
        if (i != 0)
        {
            generatedNameText += ", ";
        }
        generatedNameText += typeToString(args[i]);
    }
    generatedNameText += ">";

    if (generatedNameText.length() > cMaxIdentifierLen)
    {
        mCtx.report<cTemplateInstanceNameTooLong>(useRange,
                                                  generatedNameText,
                                                  generatedNameText.length(),
                                                  cMaxIdentifierLen);
        return nullptr;
    }

    Identifier* generatedName = mCtx.internIdentifier(generatedNameText, useRange);
    if (generatedName == nullptr)
    {
        return nullptr;
    }

    // Then, we create the substitutions for the template parameters.
    // This indicates which template parameters are substituted with which arguments.
    std::vector<TemplateSubstitution> substitutions;
    substitutions.reserve(args.size());
    for (usize i = 0; i < args.size(); ++i)
    {
        substitutions.push_back(TemplateSubstitution{pattern->mTemplateParams[i], args[i]});
    }

    // Now we need to clone the template pattern into a new AST (declaration) node.

    TemplateASTCloner cloner{mCtx.mAllocator, makeArrayView(mCtx.mAllocator, substitutions)};
    ASTNode* cloned = cloner.clone(pattern);
    auto* generated = static_cast<TypeDeclarationStatementNode*>(cloned);

    // We need to set the identifier and template params for the generated node.
    generated->mIdentifier = generatedName;
    generated->mTemplateParams = {};
    // This is an instance of a template, so mark it as such.
    generated->mFlags.set(cStmtIsTemplateInstance);

    mCtx.mTemplateCache.add(templateSymbol, generated, args);

    // Now we have to append it to a TU node so it can be properly processed.
    std::vector<ASTNode*> nodes;
    nodes.reserve(outputTU->mNodes.size() + 1);
    // Copy over the pointers.
    for (ASTNode* existing : outputTU->mNodes)
    {
        nodes.push_back(existing);
    }
    // Add the new type decl node.
    nodes.push_back(generated);
    // Set the new nodes array.
    outputTU->mNodes = makeArrayView(mCtx.mAllocator, nodes);

    // Now we need to collect the new declarations since we missed that step for the generated node.
    ScopeGuard scope{mCtx.mScopes, templateSymbol->mScope};
    DeclCollectorVisitor collector{mCtx};
    collector.run(generated);

    return generated;
}

} // namespace simlang
