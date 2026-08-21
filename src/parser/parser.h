#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "ast/nodes/astnode.h"
#include "parser/token.h"
#include "parser/tokentype.h"
#include "source/sourcerange.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

class Source;
class Tokenizer;
struct CallArgument;
struct ExpressionNode;
struct Identifier;
struct ImportSelectedEntry;
struct ParamNode;
struct ParserContext;
struct StatementNode;
struct TranslationUnitNode;
struct TypeSpecifierNode;
enum class AssignmentOp : u8;
enum class BinaryOp : u8;
enum class ConstructionKind : u8;
enum class UnaryOp : u8;

class Parser
{
public:
    explicit Parser(ParserContext& ctx, Tokenizer& tokenizer);

    bool parseCode(TranslationUnitNode*& out);

private:
    using PrecedenceType = u8;

    enum class Precedence : PrecedenceType
    {
        cNone,

        cAss,       // = += -= *= /= %= <<= >>= &= ^= |=
        cCond,      // ?:
        cOr,        // ||
        cAnd,       // &&
        cBitOr,     // |
        cBitXor,    // ^
        cBitAnd,    // &
        cEquality,  // == !=
        cComp,      // < <= > >=
        cShift,     // << >>
        cTerm,      // + -
        cFactor,    // * / %
        cUnaryPre,  // -, ~
        cUnaryPost, // (), [], .
        cScope,     // ::

        cPrimary,   // Stronger than everything else (e.g. for errors).

        cCount
    };

    enum class ParseLevel : u8
    {
        cTopLevel,
        cStatement,
        cMember
    };

    enum class RecoveryMode : u8
    {
        cAfterFailure,
        cMalformedStart
    };

    using PrefixParseFn = ExpressionNode* (Parser::*)();
    using InfixParseFn = ExpressionNode* (Parser::*)(ExpressionNode*);

    struct ParseRule
    {
        PrefixParseFn mPrefix;
        InfixParseFn mInfix;
        Precedence mPrecedence;
    };

    SourceRange makeRangeToPrevious(Token start) const;
    SourceRange makeRangeToPrevious(const ASTNode* start) const;
    static SourceRange makeRange(Token token, const ASTNode* node);

    const Token& getCurrentToken() const;
    TokenType getCurrentTokenType() const;
    std::string_view getCurrentTokenText() const;
    SourceRange getCurrentTokenRange() const;

    Token consume();
    bool check(TokenType t) const;
    bool tryConsume(TokenType t);
    bool expect(TokenType t, Token* out = nullptr, bool reportIfMissing = true);
    bool expectSemi();

    void recover(ParseLevel ctx, RecoveryMode mode);
    static bool isRecoveryBoundary(ParseLevel ctx, RecoveryMode mode, TokenType tt);
    static bool isTopLevelDeclarationStart(TokenType tt);
    static bool isStatementStart(TokenType tt);
    static bool isTypeMemberStart(TokenType tt);
    static bool tokenIsAnyOf(TokenType tt, std::initializer_list<TokenType> tokens);

    bool parseIntLiteral(Token token, i32& out) const;
    bool parseFloatLiteral(Token token, f32& out) const;
    bool parseStringTokenText(std::string& out);
    static UnaryOp getUnaryOp(TokenType token);
    static BinaryOp getBinaryOp(TokenType tt);

    static constexpr std::array<ParseRule, static_cast<std::size_t>(TokenType::cCount)> makeExpressionRules();
    static const ParseRule* getExpressionRule(TokenType tt);

    ExpressionNode* parsePrecedence(Precedence prec);
    ExpressionNode* parseError();
    ExpressionNode* parseCast();
    ExpressionNode* parseIdentifier();
    ExpressionNode* parseThis();
    ExpressionNode* parseInt();
    ExpressionNode* parseFloat();
    ExpressionNode* parseBool();
    ExpressionNode* parseNull();
    ExpressionNode* parseString();
    ExpressionNode* parseFormattedString();
    ExpressionNode* parseParen();
    ExpressionNode* parseMake();
    ExpressionNode* parseNew();
    bool parseArgumentList(std::vector<CallArgument>& args);
    ExpressionNode* parseClassConstruction(Token constructionToken, TypeSpecifierNode* typeSpec);
    ExpressionNode* parseObjectConstruction(Token constructionToken,
                                            TypeSpecifierNode* typeSpec,
                                            ConstructionKind constructionKind);
    ExpressionNode* parseFunctionCall(ExpressionNode* lhs);
    ExpressionNode* parseIndexCall(ExpressionNode* lhs);
    ExpressionNode* parseMemberAccess(ExpressionNode* lhs);
    ExpressionNode* parseModuleAccess(ExpressionNode* lhs);
    ExpressionNode* parseUnaryPre();
    ExpressionNode* parseConditional(ExpressionNode* lhs);
    ExpressionNode* parseBinary(ExpressionNode* lhs);

    ExpressionNode* parseExpression();

    AssignmentOp getAssignmentOp(TokenType tt) const;
    void parseExportSpecifiers(FlagSet<NodeFlagType>& flags);
    void parseMemberSpecifiers(FlagSet<NodeFlagType>& flags);

    StatementNode* parseEmptyStatement();
    StatementNode* parseBlock();
    StatementNode* parseAssignment(ExpressionNode* lhs, bool consumeSemi);
    StatementNode* parseExpressionOrAssignment(bool consumeSemi);
    StatementNode* parseVariableDeclaration(bool consumeSemi);
    StatementNode* parseIf();
    StatementNode* parseFor();
    StatementNode* parseWhile();
    StatementNode* parseSwitch();
    StatementNode* parseReturn();
    StatementNode* parseBreak();
    StatementNode* parseContinue();
    StatementNode* parsePrint();
    StatementNode* parseTypeMember();

    StatementNode* parseStatement();

    bool parseQualifiedName(std::vector<Identifier*>& out);
    bool parseTemplateParameterList(std::vector<Identifier*>& params);

    ImportSelectedEntry* parseImportSelectedEntry();
    ParamNode* parseFunctionParameter();
    StatementNode* parseFunctionDeclaration();
    StatementNode* parseInitializerDeclaration();
    StatementNode* parseTypeDeclaration();
    StatementNode* parseTemplateDeclaration();
    StatementNode* parseImportDeclaration();
    StatementNode* parseTopLevelDeclaration();

    bool parseTypeArgumentList(std::vector<TypeSpecifierNode*>& typeArgs);

    TypeSpecifierNode* parseTypeSpec();

    ParserContext& mCtx;
    Tokenizer& mTokenizer;
    Token mCurrentToken = cErrorToken;
    SourceLocation mPreviousTokenEnd = cInvalidSourceLoc;
    bool mInTypeDeclaration = false;
    bool mInClass = false;
    bool mInInterface = false;
};

} // namespace simlang
