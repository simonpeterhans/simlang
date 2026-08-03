#pragma once

#include <vector>

#include "ast/astwalker.h"

namespace simlang
{

struct CompilerContext;
struct ModuleEntry;
struct Symbol;

class DeclCollectorVisitor : public ASTWalker<DeclCollectorVisitor>
{
public:
    explicit DeclCollectorVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);

    bool visitBlockStatement(BlockStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

    bool visitTranslationUnit(TranslationUnitNode* node);

private:
    CompilerContext& mCtx;

    ModuleEntry* mCurrentModule = nullptr;
    Symbol* mCurrentSymbol = nullptr;
    std::vector<Symbol*>* mCurrentMemberList = nullptr;
};

} // namespace simlang
