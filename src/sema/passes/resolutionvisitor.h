#pragma once

#include <vector>

#include "ast/astwalker.h"

namespace simlang
{

struct CompilerContext;
struct Symbol;

class ResolutionVisitor : public ASTWalker<ResolutionVisitor>
{
public:
    explicit ResolutionVisitor(CompilerContext& ctx);

    bool run(TranslationUnitNode* node);

    bool visitIdentifier(IdentifierNode* node);
    bool visitThis(ThisNode*);
    bool visitLambda(LambdaNode* node);
    bool visitModuleAccess(ModuleAccessNode* node);

    bool visitBlockStatement(BlockStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

    bool visitParamDeclaration(ParamDeclarationNode* node);

    bool visitNamedTypeSpecifier(NamedTypeSpecifierNode* node);
    bool visitFunctionTypeSpecifier(FunctionTypeSpecifierNode* node);
    bool visitSubstitutedTypeSpecifier(SubstitutedTypeSpecifierNode* node);

    bool visitTranslationUnit(TranslationUnitNode* node);

private:
    bool isInitializing(Symbol* symbol) const;

    CompilerContext& mCtx;

    Symbol* mCurrentSymbol = nullptr;
    Symbol* mCurrentThisOwner = nullptr;
    Symbol* mCurrentFieldDefault = nullptr;
    TranslationUnitNode* mCurrentTranslationUnit = nullptr;
    std::vector<Symbol*> mInitializingSymbols;
};

} // namespace simlang
