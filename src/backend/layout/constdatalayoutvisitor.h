#pragma once

#include "ast/astwalker.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct CompilerContext;
struct ConstValue;
struct InternedString;
struct Type;

class ConstDataLayoutVisitor : public ASTWalker<ConstDataLayoutVisitor>
{
public:
    explicit ConstDataLayoutVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);

    // Expressions.
    bool visitIdentifier(IdentifierNode* node);
    bool visitStringLiteral(StringLiteralNode* node);
    bool visitFormatString(FormatStringNode* node);

    // Statements.
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);

private:
    StringLiteralIdx addStringLiteral(const InternedString* str);
    bool writeConstGlobalInitializer(VariableDeclarationStatementNode* node);
    bool writeConstGlobalValue(Type* type, u32 globalIdx, const ConstValue& value);

    CompilerContext& mCtx;
};

} // namespace simlang
