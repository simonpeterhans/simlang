#pragma once

#include "ast/astwalker.h"
#include "util/types.h"

namespace simlang
{

struct CompilerContext;
struct FunctionInfo;

class SymbolLayoutVisitor : public ASTWalker<SymbolLayoutVisitor>
{
public:
    explicit SymbolLayoutVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);

    // Statements.
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

    // Params.
    bool visitParamDeclaration(ParamDeclarationNode* node);

private:
    CompilerContext& mCtx;

    FunctionInfo* mCurrentFunction = nullptr;
    u32 mNextGlobalWordIndex = 0;
    u32 mNextMemberFieldIndex = 0;
};

} // namespace simlang
