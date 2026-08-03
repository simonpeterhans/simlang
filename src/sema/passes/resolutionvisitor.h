#pragma once

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
    bool visitModuleAccess(ModuleAccessNode* node);

    bool visitBlockStatement(BlockStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

    bool visitParamDeclaration(ParamDeclarationNode* node);

    bool visitNamedTypeSpecifier(NamedTypeSpecifierNode* node);
    bool visitSubstitutedTypeSpecifier(SubstitutedTypeSpecifierNode* node);

    bool visitTranslationUnit(TranslationUnitNode* node);

private:
    CompilerContext& mCtx;

    Symbol* mCurrentSymbol = nullptr;
    TranslationUnitNode* mCurrentTranslationUnit = nullptr;
};

} // namespace simlang
