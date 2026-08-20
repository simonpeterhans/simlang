#include <array>
#include <vector>

#include "ast/assignmentop.h"
#include "ast/nodes/astnode.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "diag/diagnostictype.h"
#include "parser/parser.h"
#include "parser/parsercontext.h"
#include "parser/token.h"
#include "parser/tokentype.h"
#include "source/sourcelocation.h"
#include "source/sourcerange.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/flags.h"

namespace simlang
{

struct TypeSpecifierNode;

AssignmentOp Parser::getAssignmentOp(TokenType tt) const
{
    // clang-format off
    switch (tt)
    {
        case TokenType::cAss:       return AssignmentOp::cAss;
        case TokenType::cAssAdd:    return AssignmentOp::cAssAdd;
        case TokenType::cAssSub:    return AssignmentOp::cAssSub;
        case TokenType::cAssMul:    return AssignmentOp::cAssMul;
        case TokenType::cAssDiv:    return AssignmentOp::cAssDiv;
        case TokenType::cAssMod:    return AssignmentOp::cAssMod;
        case TokenType::cAssAnd:    return AssignmentOp::cAssAnd;
        case TokenType::cAssOr:     return AssignmentOp::cAssOr;
        case TokenType::cAssXor:    return AssignmentOp::cAssXor;
        case TokenType::cAssSl:     return AssignmentOp::cAssShl;
        case TokenType::cAssSr:     return AssignmentOp::cAssShr;
        default:                    break;
    }
    // clang-format on

    return AssignmentOp::cInvalid;
}

StatementNode* Parser::parseEmptyStatement()
{
    // Consume the ';'.
    Token semiToken = consume();
    return mCtx.create<EmptyStatementNode>(semiToken.getRange());
}

StatementNode* Parser::parseBlock()
{
    // { stmt0; stmt1; ... }

    // Consume the '{'.
    Token leftBraceToken = cErrorToken;
    if (expect(TokenType::cLeftBrace, &leftBraceToken, true) == false)
    {
        return nullptr;
    }

    std::vector<StatementNode*> bodyStatements;

    // Parse statements until the closing '}' or EOF.
    while (getCurrentToken().mType != TokenType::cRightBrace && getCurrentToken().mType != TokenType::cEOF)
    {
        if (StatementNode* stmt = parseStatement())
        {
            bodyStatements.push_back(stmt);
        }
    }

    // Consume the '}'.
    if (tryConsume(TokenType::cRightBrace) == false)
    {
        mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightBrace, getCurrentTokenText());
        return nullptr;
    }

    // Build the block node.
    ArrayView<StatementNode*> bodyStatementsView = makeArrayView(mCtx.mAllocator, bodyStatements);
    return mCtx.create<BlockStatementNode>(makeRangeToPrevious(leftBraceToken), bodyStatementsView);
}

StatementNode* Parser::parseAssignment(ExpressionNode* lhs, bool consumeSemi)
{
    // lhs op rhs

    // Get the assignment op for the current token type.
    AssignmentOp op = getAssignmentOp(getCurrentTokenType());
    if (op == AssignmentOp::cInvalid)
    {
        return nullptr;
    }

    // Consume the assignment op.
    consume();

    // Parse the RHS.
    ExpressionNode* rhs = parseExpression();
    if (rhs == nullptr)
    {
        return nullptr;
    }

    // Include the semicolon in the range when requested.
    SourceRange range = lhs->makeRangeTo(rhs);
    if (consumeSemi)
    {
        if (expectSemi() == false)
        {
            return nullptr;
        }

        range = makeRangeToPrevious(lhs);
    }

    return mCtx.create<AssignmentStatementNode>(range, lhs, rhs, op);
}

StatementNode* Parser::parseExpressionOrAssignment(bool consumeSemi)
{
    // Parse the expression that either forms the whole statement or the LHS of an assignment.
    ExpressionNode* expr = parseExpression();
    if (expr == nullptr)
    {
        return nullptr;
    }

    // If an assignment op follows, parse this as an assignment statement.
    TokenType tt = getCurrentTokenType();

    switch (tt)
    {
        case TokenType::cAss:
        case TokenType::cAssAdd:
        case TokenType::cAssSub:
        case TokenType::cAssMul:
        case TokenType::cAssDiv:
        case TokenType::cAssMod:
        case TokenType::cAssAnd:
        case TokenType::cAssOr:
        case TokenType::cAssXor:
        case TokenType::cAssSl:
        case TokenType::cAssSr:
        {
            return parseAssignment(expr, consumeSemi);
        }
        default:
        {
            break;
        }
    }

    // Otherwise, this is a regular expression statement.
    SourceRange range = expr->mSourceRange;
    if (consumeSemi)
    {
        // Include the semicolon in the range when requested.
        if (expectSemi() == false)
        {
            return nullptr;
        }

        range = makeRangeToPrevious(expr);
    }

    return mCtx.create<ExpressionStatementNode>(range, expr);
}

void Parser::parseExportSpecifiers(FlagSet<NodeFlagType>& flags)
{
    while (true)
    {
        switch (getCurrentTokenType())
        {
            case TokenType::cExport:
            {
                // Consume the "export".
                Token specToken = consume();

                if (flags.test(cStmtIsExported))
                {
                    // Duplicate export specifier.
                    mCtx.report<cDuplicateExportSpecifier>(specToken.getRange());
                }

                flags.set(cStmtIsExported);
                break;
            }
            default:
            {
                // If we have no more export specifiers, we're done.
                return;
            }
        }
    }
}

void Parser::parseMemberSpecifiers(FlagSet<NodeFlagType>& flags)
{
    while (true)
    {
        switch (getCurrentTokenType())
        {
            case TokenType::cPrivate:
            {
                // Consume the "private".
                Token specToken = consume();

                if (flags.test(cStmtIsPrivate))
                {
                    // Duplicate private specifier.
                    mCtx.report<cDuplicatePrivateSpecifier>(specToken.getRange());
                }

                flags.set(cStmtIsPrivate);
                break;
            }
            case TokenType::cImpl:
            {
                // Consume the "impl".
                Token specToken = consume();

                if (flags.test(cStmtIsInterfaceImpl))
                {
                    // Duplicate impl specifier.
                    mCtx.report<cDuplicateImplSpecifier>(specToken.getRange());
                }

                flags.set(cStmtIsInterfaceImpl);
                break;
            }
            default:
            {
                // If we have no more member specifiers, we're done.
                return;
            }
        }
    }
}

StatementNode* Parser::parseVariableDeclaration(bool consumeSemi)
{
    // (var | const) identifier [: type] [= expression]

    // Keep the first token for range purposes.
    Token startToken = getCurrentToken();

    Token nameToken = cErrorToken;
    TypeSpecifierNode* typeSpec = nullptr;
    ExpressionNode* init = nullptr;
    FlagSet<NodeFlagType> flags;

    auto buildNode = [&](SourceRange range)
    {
        auto* node = mCtx.create<VariableDeclarationStatementNode>(range,
                                                                   nameToken.getRange(),
                                                                   nameToken.mIdentifier,
                                                                   typeSpec,
                                                                   init);
        node->mFlags = flags;
        return node;
    };

    // Consume declaration and mutability stuff.
    if (tryConsume(TokenType::cVar))
    {
        flags.set(cStmtIsMutable);
    }
    else if (tryConsume(TokenType::cConst) == false)
    {
        flags.set(cStmtIsMutable);
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                      std::array{TokenType::cConst, TokenType::cVar},
                                      getCurrentTokenText());
        return nullptr;
    }

    // Parse the variable name.
    if (expect(TokenType::cIdentifier, &nameToken, true) == false)
    {
        return nullptr;
    }

    // Parse the type annotation if we have one.
    if (tryConsume(TokenType::cColon))
    {
        typeSpec = parseTypeSpec();
        if (typeSpec == nullptr)
        {
            return nullptr;
        }
    }

    // If we're not in a type declaration, expect "==".
    if (mInTypeDeclaration == false || check(TokenType::cAss))
    {
        // Consume the '='.
        if (tryConsume(TokenType::cAss) == false)
        {
            if (mInTypeDeclaration == false)
            {
                mCtx.report<cVariableInitializerRequired>(nameToken.getRange(), nameToken.mIdentifier);
            }
            else
            {
                mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cAss, getCurrentTokenText());
            }
            return nullptr;
        }

        // Parse the initializer expression.
        init = parseExpression();
        if (init == nullptr)
        {
            return nullptr;
        }
    }

    SourceRange range = makeRangeToPrevious(startToken);
    if (consumeSemi)
    {
        if (expectSemi() == false)
        {
            return nullptr;
        }

        range = makeRangeToPrevious(startToken);
    }

    return buildNode(range);
}

StatementNode* Parser::parseReturn()
{
    // Consume the "return".
    Token returnToken = consume();
    ExpressionNode* expr = nullptr;

    // Parse the optional return expression.
    if (check(TokenType::cSemiColon) == false)
    {
        expr = parseExpression();
        if (expr == nullptr)
        {
            return nullptr;
        }
    }

    // Consume the ';'.
    if (expectSemi() == false)
    {
        return nullptr;
    }

    return mCtx.create<ReturnStatementNode>(makeRangeToPrevious(returnToken), expr);
}

StatementNode* Parser::parseIf()
{
    // if (condition) { ... } [else if (condition) { ... }] [else { ... }]

    std::vector<IfBranchStatementNode*> branches;
    StatementNode* elseBody = nullptr;

    // Build the full if statement once all branches have been parsed.
    auto buildNode = [&]()
    {
        ArrayView<IfBranchStatementNode*> branchesView = makeArrayView(mCtx.mAllocator, branches);
        // The last node (for source range purposes) is either the last branch or the else body.
        ASTNode* endNode = (elseBody != nullptr) ? elseBody : branches.back();
        SourceRange range = branches.front()->makeRangeTo(endNode);
        return mCtx.create<IfStatementNode>(range, branchesView, elseBody);
    };

    // Parse either the initial "if" branch or an "else if" branch.
    auto parseIfBranch = [this]() -> IfBranchStatementNode*
    {
        // Consume the "if".
        Token ifToken = consume();

        // Consume the '('.
        if (expect(TokenType::cLeftParen, nullptr, true) == false)
        {
            return nullptr;
        }

        // Parse the branch condition.
        ExpressionNode* condition = parseExpression();
        if (condition == nullptr)
        {
            return nullptr;
        }

        // Consume the ')'.
        if (expect(TokenType::cRightParen, nullptr, true) == false)
        {
            return nullptr;
        }

        // Parse the branch body.
        StatementNode* body = parseBlock();
        if (body == nullptr)
        {
            return nullptr;
        }
        return mCtx.create<IfBranchStatementNode>(makeRange(ifToken, body), condition, body);
    };

    // Parse the initial "if" branch.
    IfBranchStatementNode* branch = parseIfBranch();
    if (branch == nullptr)
    {
        return nullptr;
    }
    branches.push_back(branch);

    // Parse all else ifs and the final else.
    while (tryConsume(TokenType::cElse))
    {
        if (getCurrentToken().mType == TokenType::cIf)
        {
            IfBranchStatementNode* elseIf = parseIfBranch();
            if (elseIf == nullptr)
            {
                return nullptr;
            }
            branches.push_back(elseIf);
        }
        else
        {
            elseBody = parseBlock();
            if (elseBody == nullptr)
            {
                return nullptr;
            }
            break;
        }
    }

    return buildNode();
}

StatementNode* Parser::parseWhile()
{
    // while (condition) { ... }

    // Consume the "while".
    Token whileToken = consume();

    // Consume the '('.
    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the loop condition.
    ExpressionNode* condition = parseExpression();
    if (condition == nullptr)
    {
        return nullptr;
    }

    // Consume the ')'.
    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the loop body.
    StatementNode* body = parseBlock();
    if (body == nullptr)
    {
        return nullptr;
    }
    return mCtx.create<WhileStatementNode>(makeRange(whileToken, body), condition, body);
}

StatementNode* Parser::parseFor()
{
    // for (initializer; condition; increment) { ... }

    // Consume the "for".
    Token forToken = consume();
    StatementNode* init = nullptr;
    ExpressionNode* condition = nullptr;
    StatementNode* increment = nullptr;

    // Consume the '('.
    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the initializer if we have one.
    if (getCurrentTokenType() != TokenType::cSemiColon)
    {
        if (getCurrentTokenType() == TokenType::cVar || getCurrentTokenType() == TokenType::cConst)
        {
            init = parseVariableDeclaration(false);
            if (init == nullptr)
            {
                return nullptr;
            }
        }
        else
        {
            init = parseExpressionOrAssignment(false);
            if (init == nullptr)
            {
                return nullptr;
            }
        }
    }

    // Consume the first ';'.
    if (expect(TokenType::cSemiColon, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the condition if we have one.
    if (getCurrentTokenType() != TokenType::cSemiColon)
    {
        condition = parseExpression();
        if (condition == nullptr)
        {
            return nullptr;
        }
    }

    // Consume the second ';'.
    if (expect(TokenType::cSemiColon, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the increment if we have one.
    if (getCurrentTokenType() != TokenType::cRightParen)
    {
        increment = parseExpressionOrAssignment(false);
        if (increment == nullptr)
        {
            return nullptr;
        }
    }

    // Consume the ')'.
    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the loop body.
    StatementNode* body = parseBlock();
    if (body == nullptr)
    {
        return nullptr;
    }
    return mCtx.create<ForStatementNode>(makeRange(forToken, body), init, condition, increment, body);
}

StatementNode* Parser::parseSwitch()
{
    // switch (expression) { case expression: ... default: ... }

    // Consume the "switch".
    Token switchToken = consume();

    // Consume the '('.
    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the switch expression.
    ExpressionNode* expr = parseExpression();
    if (expr == nullptr)
    {
        return nullptr;
    }

    // Consume the ')'.
    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Consume the '{'.
    if (expect(TokenType::cLeftBrace, nullptr, true) == false)
    {
        return nullptr;
    }

    std::vector<SwitchSectionStatementNode*> sections;

    // Parse all branches.
    while (getCurrentTokenType() != TokenType::cRightBrace && getCurrentTokenType() != TokenType::cEOF)
    {
        TokenType labelType = getCurrentTokenType();

        // Check if we have a case or a default label.
        if (labelType != TokenType::cCase && labelType != TokenType::cDefault)
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                          std::array{TokenType::cCase, TokenType::cDefault, TokenType::cRightBrace},
                                          getCurrentTokenText());
            recover(ParseLevel::cStatement, RecoveryMode::cMalformedStart);
            continue;
        }

        // Consume the label.
        Token labelToken = consume();
        ExpressionNode* caseExpression = nullptr;

        // If we have a label, parse the expression.
        if (labelType == TokenType::cCase)
        {
            caseExpression = parseExpression();
            if (caseExpression == nullptr)
            {
                return nullptr;
            }
        }

        // Consume the ':'.
        if (expect(TokenType::cColon, nullptr, true) == false)
        {
            return nullptr;
        }

        // Parse statements until the next label or the end of the switch.
        std::vector<StatementNode*> statements;
        while (getCurrentTokenType() != TokenType::cCase && getCurrentTokenType() != TokenType::cDefault &&
               getCurrentTokenType() != TokenType::cRightBrace && getCurrentTokenType() != TokenType::cEOF)
        {
            if (StatementNode* stmt = parseStatement())
            {
                statements.push_back(stmt);
            }
        }

        // Build a block for the section body and then the section itself.
        SourceLocation end = statements.empty() ? mPreviousTokenEnd : statements.back()->mSourceRange.getEndLoc();
        SourceRange sectionRange{labelToken.getStartLoc(), end};
        ArrayView<StatementNode*> statementsView = makeArrayView(mCtx.mAllocator, statements);
        auto* body = mCtx.create<BlockStatementNode>(sectionRange, statementsView);
        sections.push_back(mCtx.create<SwitchSectionStatementNode>(sectionRange, caseExpression, body));
    }

    // Consume the '}'.
    if (tryConsume(TokenType::cRightBrace) == false)
    {
        mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightBrace, getCurrentTokenText());
        return nullptr;
    }

    ArrayView<SwitchSectionStatementNode*> sectionsView = makeArrayView(mCtx.mAllocator, sections);
    return mCtx.create<SwitchStatementNode>(makeRangeToPrevious(switchToken), expr, sectionsView);
}

StatementNode* Parser::parseContinue()
{
    // Consume the "continue".
    Token continueToken = consume();

    // Consume the ';'.
    if (expectSemi() == false)
    {
        return nullptr;
    }

    return mCtx.create<ContinueStatementNode>(makeRangeToPrevious(continueToken));
}

StatementNode* Parser::parseBreak()
{
    // Consume the "break".
    Token breakToken = consume();

    // Consume the ';'.
    if (expectSemi() == false)
    {
        return nullptr;
    }

    return mCtx.create<BreakStatementNode>(makeRangeToPrevious(breakToken));
}

StatementNode* Parser::parsePrint()
{
    // print(expression);

    // Consume the "print".
    Token printToken = consume();
    ExpressionNode* expr = nullptr;

    // Consume the '('.
    if (tryConsume(TokenType::cLeftParen) == false)
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cLeftParen, getCurrentTokenText());
        return nullptr;
    }

    // Parse the expression to print.
    expr = parseExpression();
    if (expr == nullptr)
    {
        return nullptr;
    }

    // Consume the ')'.
    if (tryConsume(TokenType::cRightParen) == false)
    {
        // At a statement boundary, report the more specific missing-delimiter diagnostic.
        if (getCurrentTokenType() == TokenType::cSemiColon || getCurrentTokenType() == TokenType::cRightBrace ||
            getCurrentTokenType() == TokenType::cEOF)
        {
            mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightParen, getCurrentTokenType());
        }
        else
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cRightParen, getCurrentTokenText());
        }
        return nullptr;
    }

    // Consume the ';'.
    if (expectSemi() == false)
    {
        return nullptr;
    }

    return mCtx.create<PrintStatementNode>(makeRangeToPrevious(printToken), expr);
}

StatementNode* Parser::parseStatement()
{
    Token startToken = getCurrentToken();
    TokenType tt = getCurrentTokenType();
    StatementNode* result = nullptr;

    switch (tt)
    {
        case TokenType::cSemiColon:
        {
            result = parseEmptyStatement();
            break;
        }
        case TokenType::cLeftBrace:
        {
            result = parseBlock();
            break;
        }
        case TokenType::cReturn:
        {
            result = parseReturn();
            break;
        }
        case TokenType::cIf:
        {
            result = parseIf();
            break;
        }
        case TokenType::cSwitch:
        {
            result = parseSwitch();
            break;
        }
        case TokenType::cFor:
        {
            result = parseFor();
            break;
        }
        case TokenType::cWhile:
        {
            result = parseWhile();
            break;
        }
        case TokenType::cContinue:
        {
            result = parseContinue();
            break;
        }
        case TokenType::cBreak:
        {
            result = parseBreak();
            break;
        }
        case TokenType::cCase:
        {
            mCtx.report<cCaseOutsideSwitch>(startToken.getRange());
            recover(ParseLevel::cStatement, RecoveryMode::cMalformedStart);
            return nullptr;
        }
        case TokenType::cDefault:
        {
            mCtx.report<cDefaultOutsideSwitch>(startToken.getRange());
            recover(ParseLevel::cStatement, RecoveryMode::cMalformedStart);
            return nullptr;
        }
        case TokenType::cVar:
        case TokenType::cConst:
        {
            result = parseVariableDeclaration(true);
            break;
        }
        case TokenType::cPrint:
        {
            result = parsePrint();
            break;
        }
        default:
        {
            if (getExpressionRule(tt)->mPrefix != nullptr)
            {
                result = parseExpressionOrAssignment(true);
            }
            else
            {
                mCtx.report<cInvalidStatementStart>(startToken.getRange(), getCurrentTokenText());
                recover(ParseLevel::cStatement, RecoveryMode::cMalformedStart);
                return nullptr;
            }
            break;
        }
    }

    if (result == nullptr)
    {
        recover(ParseLevel::cStatement, RecoveryMode::cAfterFailure);
        return nullptr;
    }

    return result;
}

} // namespace simlang
