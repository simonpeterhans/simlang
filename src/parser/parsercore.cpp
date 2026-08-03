#include <initializer_list>
#include <string_view>

#include "ast/nodes/astnode.h"
#include "diag/diagnostictype.h"
#include "parser/parser.h"
#include "parser/parsercontext.h"
#include "parser/token.h"
#include "parser/tokenizer.h"
#include "parser/tokentype.h"
#include "source/source.h"
#include "source/sourcelocation.h"
#include "source/sourcerange.h"
#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

Parser::Parser(ParserContext& ctx, Tokenizer& tokenizer)
    : mCtx(ctx)
    , mTokenizer(tokenizer)
{
}

SourceRange Parser::makeRangeToPrevious(Token start) const
{
    // The range goes from the start of the given token to the end of the previous token.
    SourceLocation end = mPreviousTokenEnd;
    if (end == cInvalidSourceLoc)
    {
        end = start.getEndLoc();
    }
    else
    {
        // If the end is before the start, that's a parser error.
        SIMLANG_ASSERT(end >= start.getStartLoc());
    }

    return SourceRange{start.getStartLoc(), end};
}

SourceRange Parser::makeRangeToPrevious(const ASTNode* start) const
{
    // The range goes from the start of the given node to the end of the previous token.
    SourceLocation startLoc = start->mSourceRange.getStartLoc();
    SourceLocation endLoc = mPreviousTokenEnd;
    if (endLoc == cInvalidSourceLoc)
    {
        endLoc = start->mSourceRange.getEndLoc();
    }
    else
    {
        // If the end is before the start, that's a parser error.
        SIMLANG_ASSERT(startLoc <= endLoc);
    }

    return SourceRange{startLoc, endLoc};
}

/* static */ SourceRange Parser::makeRange(Token token, const ASTNode* node)
{
    // The range goes from the start of the given token to the end of the given node.
    return token.makeRangeTo(node->mSourceRange);
}

const Token& Parser::getCurrentToken() const
{
    return mCurrentToken;
}

TokenType Parser::getCurrentTokenType() const
{
    return getCurrentToken().mType;
}

std::string_view Parser::getCurrentTokenText() const
{
    SourceRange range = getCurrentTokenRange();
    const Source& source = mTokenizer.getSource();
    return source.getSlice(range.getStartLoc(), range.getRangeLength());
}

SourceRange Parser::getCurrentTokenRange() const
{
    return getCurrentToken().getRange();
}

Token Parser::consume()
{
    Token current = getCurrentToken();

    // The previous end location is relevant for AST source ranges.
    mPreviousTokenEnd = current.getEndLoc();
    // The token is what we're (usually) interested in.
    mCurrentToken = mTokenizer.scanNextToken();

    return current;
}

bool Parser::check(TokenType t) const
{
    return (t == getCurrentTokenType());
}

bool Parser::tryConsume(TokenType expectedType)
{
    if (check(expectedType) == false)
    {
        return false;
    }
    consume();
    return true;
}

bool Parser::expect(TokenType t, Token* out, bool reportIfMissing)
{
    if (getCurrentTokenType() != t)
    {
        if (reportIfMissing)
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), t, getCurrentTokenText());
        }

        return false;
    }

    if (out != nullptr)
    {
        *out = consume();
    }
    else
    {
        consume();
    }

    return true;
}

bool Parser::expectSemi()
{
    if (tryConsume(TokenType::cSemiColon))
    {
        return true;
    }
    mCtx.report<cMissingSemicolon>(SourceRange::at(mPreviousTokenEnd), getCurrentTokenType());
    return false;
}

/* static */ bool Parser::tokenIsAnyOf(TokenType tt, std::initializer_list<TokenType> tokens)
{
    for (TokenType token : tokens)
    {
        if (token == tt)
        {
            return true;
        }
    }
    return false;
}

/* static */ bool Parser::isTopLevelDeclarationStart(TokenType tt)
{
    switch (tt)
    {
        case TokenType::cVar:
        case TokenType::cConst:
        case TokenType::cFun:
        case TokenType::cStruct:
        case TokenType::cClass:
        case TokenType::cInterface:
        case TokenType::cTemplate:
        case TokenType::cImport:
        case TokenType::cExport:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

/* static */ bool Parser::isStatementStart(TokenType tt)
{
    switch (tt)
    {
        // The semi is here for the empty statement (which we allow currently).
        case TokenType::cSemiColon:
        case TokenType::cLeftBrace:
        case TokenType::cReturn:
        case TokenType::cIf:
        case TokenType::cSwitch:
        case TokenType::cCase:
        case TokenType::cDefault:
        case TokenType::cFor:
        case TokenType::cWhile:
        case TokenType::cBreak:
        case TokenType::cContinue:
        case TokenType::cVar:
        case TokenType::cConst:
        case TokenType::cPrint:
        {
            return true;
        }
        default:
        {
            break;
        }
    }

    return (getExpressionRule(tt)->mPrefix != nullptr);
}

/* static */ bool Parser::isTypeMemberStart(TokenType tt)
{
    switch (tt)
    {
        case TokenType::cImpl:
        case TokenType::cPrivate:
        case TokenType::cExport:
        case TokenType::cFun:
        case TokenType::cVar:
        case TokenType::cConst:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

/* static */ bool Parser::isRecoveryBoundary(ParseLevel ctx, RecoveryMode mode, TokenType tt)
{
    if (tt == TokenType::cEOF)
    {
        return true;
    }

    // Unlike the functions above, this is more complex.
    // We want to find out if a token type is a recovery boundary for the given context and recovery mode.
    switch (ctx)
    {
        case ParseLevel::cTopLevel:
        {
            return isTopLevelDeclarationStart(tt);
        }
        case ParseLevel::cStatement:
        {
            // '}' and ';' are always recovery boundaries for statements.
            if (tt == TokenType::cRightBrace || tt == TokenType::cSemiColon)
            {
                return true;
            }

            // If we had a malformed start, try to recover on a lot of token types.
            if (mode == RecoveryMode::cMalformedStart)
            {
                return tokenIsAnyOf(tt, {TokenType::cReturn,   TokenType::cIf,        TokenType::cElse,
                                         TokenType::cSwitch,   TokenType::cCase,      TokenType::cDefault,
                                         TokenType::cFor,      TokenType::cWhile,     TokenType::cBreak,
                                         TokenType::cContinue, TokenType::cVar,       TokenType::cConst,
                                         TokenType::cPrint,    TokenType::cImport,    TokenType::cExport,
                                         TokenType::cPrivate,  TokenType::cFun,       TokenType::cStruct,
                                         TokenType::cClass,    TokenType::cInterface, TokenType::cTemplate,
                                         TokenType::cInOut,    TokenType::cAs,        TokenType::cImpl});
            }

            // If we failed in the middle of something, skip to the next statement.
            return isStatementStart(tt);
        }
        case ParseLevel::cMember:
        {
            // A new member can start after a '}', ';', or a type member start token type.
            return (tt == TokenType::cRightBrace || tt == TokenType::cSemiColon || isTypeMemberStart(tt));
        }
        default:
        {
            break;
        }
    }

    return false;
}

void Parser::recover(ParseLevel ctx, RecoveryMode mode)
{
    // If we tried to do something and the first token was unexpected, consume that.
    // Otherwise, we don't do this since the token might be useful for recovery.
    if (mode == RecoveryMode::cMalformedStart && getCurrentTokenType() != TokenType::cEOF)
    {
        consume();
    }

    // Before we try to recover, balance delimiters.
    // If we're balanced, try to see if we're at a valid recovery boundary.
    u32 parenDepth = 0;
    u32 bracketDepth = 0;
    u32 braceDepth = 0;

    while (true)
    {
        TokenType tt = getCurrentTokenType();

        if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 && isRecoveryBoundary(ctx, mode, tt))
        {
            // If we have a semi, we need to advance so we make progress.
            if (tt == TokenType::cSemiColon)
            {
                consume();
            }
            return;
        }

        switch (tt)
        {
            case TokenType::cLeftParen:
            {
                ++parenDepth;
                consume();
                break;
            }
            case TokenType::cRightParen:
            {
                if (parenDepth > 0)
                {
                    --parenDepth;
                }

                consume();
                break;
            }
            case TokenType::cLeftBracket:
            {
                ++bracketDepth;
                consume();
                break;
            }
            case TokenType::cRightBracket:
            {
                if (bracketDepth > 0)
                {
                    --bracketDepth;
                }

                consume();
                break;
            }
            case TokenType::cLeftBrace:
            {
                ++braceDepth;
                consume();
                break;
            }
            case TokenType::cRightBrace:
            {
                if (braceDepth > 0)
                {
                    --braceDepth;
                }

                consume();
                break;
            }
            default:
            {
                consume();
                break;
            }
        }
    }
}

} // namespace simlang
