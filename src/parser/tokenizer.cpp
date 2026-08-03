#include "parser/tokenizer.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "diag/diagnostictype.h"
#include "parser/parsercontext.h"
#include "source/source.h"
#include "source/sourcerange.h"
#include "util/escaping.h"

namespace simlang
{

struct Identifier;

static bool isSpace(char c)
{
    return (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f');
}

static bool isAlphaOrUnderscore(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}

static bool isDigit(char c)
{
    return (c >= '0' && c <= '9');
}

static bool isHex(char c)
{
    return isDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static bool isBinary(char c)
{
    return (c == '0' || c == '1');
}

static bool isGenericNumberChar(char c)
{
    return isDigit(c) || isAlphaOrUnderscore(c) || c == '.' || static_cast<unsigned char>(c) >= 128;
}

Tokenizer::Tokenizer(ParserContext& ctx, const Source& source)
    : mCtx(ctx)
    , mSource(source)
{
    mSourceStart = source.getSource();
    mSourceEnd = mSourceStart + source.getSourceLength();
    mTokenStart = mSourceStart;
    mTokenCurrent = mSourceStart;
}

ScopedValueBinder<bool> Tokenizer::scopedSplitRightAngles(bool splitRightAngles)
{
    return ScopedValueBinder{mSplitRightAngles, splitRightAngles};
}

bool Tokenizer::isAtEndChar() const
{
    return mTokenCurrent >= mSourceEnd;
}

char Tokenizer::peekChar() const
{
    return *mTokenCurrent;
}

char Tokenizer::peekNextChar() const
{
    if (isAtEndChar() || mTokenCurrent + 1 >= mSourceEnd)
    {
        return '\0';
    }

    return mTokenCurrent[1];
}

char Tokenizer::advanceChar()
{
    if (isAtEndChar())
    {
        return '\0';
    }

    // Return current and advance.
    char c = *mTokenCurrent++;

    return c;
}

bool Tokenizer::advanceCharIfMatch(char expected)
{
    if (isAtEndChar())
    {
        return (expected == '\0');
    }

    if (*mTokenCurrent != expected)
    {
        return false;
    }

    advanceChar();

    return true;
}

SourceLocation Tokenizer::getCurrentStartLoc() const
{
    return mSource.getLocation(static_cast<u32>(mTokenStart - mSourceStart));
}

SourceLocation Tokenizer::getCurrentLoc() const
{
    return mSource.getLocation(static_cast<u32>(mTokenCurrent - mSourceStart));
}

SourceRange Tokenizer::getCurrentRange() const
{
    return SourceRange{getCurrentStartLoc(), getCurrentLoc()};
}

void Tokenizer::skipSingleLineComment()
{
    // Single line comment, skip over that.
    advanceChar(); // '/'
    advanceChar(); // '/'

    while (isAtEndChar() == false && peekChar() != '\n')
    {
        advanceChar();
    }
    // Newline to be processed in next iteration.
}

bool Tokenizer::skipBlockComment()
{
    // We entered a block comment, skip over that.
    advanceChar(); // '/'
    advanceChar(); // '*'

    // Keep going until we reached the end.
    while (isAtEndChar() == false)
    {
        // Or if we found a "*/".
        if (peekChar() == '*' && peekNextChar() == '/')
        {
            advanceChar(); // '*'
            advanceChar(); // '/'
            return true;
        }

        // Consume the comment char.
        advanceChar();
    }

    // EOF, but no "*/".
    mCtx.report<cUnterminatedBlockComment>(getCurrentRange());
    return false;
}

bool Tokenizer::skipWhitespace()
{
    while (isAtEndChar() == false)
    {
        char c = peekChar();

        // Handle space.
        if (isSpace(c))
        {
            advanceChar(); // ' '
            continue;
        }

        // Handle comments.
        if (c == '/')
        {
            char cNext = peekNextChar();
            if (cNext == '/')
            {
                // Keep the token range anchored to the comment for diagnostics.
                mTokenStart = mTokenCurrent;
                skipSingleLineComment(); // "//"
                continue;
            }

            if (cNext == '*')
            {
                // Keep the token range anchored to the comment for diagnostics.
                mTokenStart = mTokenCurrent;
                if (skipBlockComment() == false) // "/* */"
                {
                    // Unterminated block comment.
                    return false;
                }

                continue;
            }
        }

        // No whitespace or '/', we're done.
        break;
    }

    return true;
}

Token Tokenizer::makeToken(TokenType type, TokenNumberType numberType)
{
    ++mScannedTokenCount;

    SourceLocation startLoc = getCurrentStartLoc();

    // Get the token length, make sure it doesn't exceed the maximum token length.
    usize rawLength = static_cast<usize>(mTokenCurrent - mTokenStart);
    u16 tokenLength = static_cast<u16>(std::min(rawLength, static_cast<usize>(cMaxTokenLen)));

    // If we exceeded the maximum token length, this is an error.
    if (rawLength > cMaxTokenLen)
    {
        mCtx.report<cTokenTooLong>(SourceRange::at(startLoc, tokenLength), rawLength, cMaxTokenLen);
        return Token{startLoc, tokenLength, TokenType::cError, TokenNumberType::cNone, nullptr};
    }

    // If this is an identifier, intern it.
    Identifier* identifier = nullptr;

    if (type == TokenType::cIdentifier)
    {
        identifier =
            mCtx.internIdentifier(std::string_view{mTokenStart, tokenLength}, SourceRange::at(startLoc, tokenLength));
        if (identifier == nullptr)
        {
            return Token{startLoc, tokenLength, TokenType::cError, TokenNumberType::cNone, nullptr};
        }
    }

    return Token{startLoc, tokenLength, type, numberType, identifier};
}

Token Tokenizer::makeStringToken(bool isFormatString)
{
    // There are three ways to get here:
    // We're at the start of a normal string (").
    // If we're the start of a format string ($").
    // If we just finished an expression of a format string (}).
    // mNextSegmentIsStart of the current frame tells us whether it's case 2 or 3.

    bool hadError = false;

    while (isAtEndChar() == false)
    {
        char c = peekChar();

        // Closing '"'.
        if (c == '"')
        {
            // Special case formatting string that is actually a normal string.
            TokenType tt = isFormatString ? TokenType::cFormatEnd : TokenType::cString;

            advanceChar(); // '"'

            if (isFormatString)
            {
                // Pop the current format frame since it's ending.
                mFormatStack.pop_back();
            }

            if (hadError)
            {
                return makeToken(TokenType::cError);
            }

            return makeToken(tt);
        }

        // This is not allowed.
        if (c == '\n')
        {
            // Linebreak in string literal.
            mCtx.report<cNewlineInStringLiteral>(getCurrentRange());
            hadError = true;
        }

        // Check escape.
        if (c == '\\')
        {
            // Get the bytes starting from the character after the '\'.
            std::string_view escapeText{mTokenCurrent + 1, static_cast<usize>(mSourceEnd - (mTokenCurrent + 1))};
            usize charsConsumed = 0;

            if (escape::validateEscapeSequence(escapeText, charsConsumed))
            {
                // Skip the '\'.
                advanceChar();
                // Consume all the characters part of the escape.
                for (usize i = 0; i < charsConsumed; ++i)
                {
                    advanceChar();
                }
                // We're done with the escape, continue with regular string parsing.
                continue;
            }

            // EOF and newline error reporting are handled separately.
            char next = peekNextChar();
            if (next != '\0' && next != '\n')
            {
                SourceLocation loc = getCurrentLoc().getAdvancedBy(1);
                mCtx.report<cInvalidEscapeCharacter>(SourceRange::at(loc), next);
                hadError = true;

                // Consume the '\'.
                advanceChar();
                // Consume the invalid escape char.
                advanceChar();

                // We're done with the escape, continue with regular string parsing.
                continue;
            }
        }

        // Additional checks for formatting strings in case we encounter an expr.
        if (isFormatString)
        {
            // We're ending part of the string and start an expression.
            if (c == '{')
            {
                // Consume the '{'.
                advanceChar();

                // Get the current stack frame.
                FormatFrame& ff = mFormatStack.back();

                // Find out what we want to return.
                TokenType tt = ff.mNextSegmentIsStart ? TokenType::cFormatStart : TokenType::cFormatMid;

                // Update the current stack frame.
                // We're entering an expression in the current format string.
                ff.mInExpression = true;
                // The next segment cannot be another start segment.
                ff.mNextSegmentIsStart = false;

                if (hadError)
                {
                    return makeToken(TokenType::cError);
                }

                return makeToken(tt);
            }

            if (c == '}')
            {
                // An unescaped '}' inside format text is invalid.
                SourceLocation loc = getCurrentLoc();
                mCtx.report<cIllegalCharacter>(SourceRange::at(loc), c);
                hadError = true;
            }
        }

        // Consume the character as part of the string.
        advanceChar();
    }

    mCtx.report<cUnterminatedStringLiteral>(getCurrentRange());
    return makeToken(TokenType::cError);
}

TokenNumberType Tokenizer::getTokenNumberType(char c)
{
    // We came here with c being a digit.
    // Integer: 0x..., 0b..., 1234567890 (but not octal, i.e., 0123456789).
    // Float: 1.2 (but not 1. or .2).

    // Special case for starting with 0.
    if (c == '0')
    {
        char next = peekChar();
        switch (next)
        {
            case '.':
            {
                // Valid float, parse as usual.
                break;
            }
            case 'x':
            case 'X':
            {
                // Hex.
                advanceChar(); // 'x' or 'X'

                // We want at least one hex char or this is an error.
                if (isHex(peekChar()) == false)
                {
                    SourceLocation loc = getCurrentLoc();
                    mCtx.report<cInvalidHexCharacter>(SourceRange::at(loc), peekChar());
                    return TokenNumberType::cInvalid;
                }

                // "0x" has been consumed and we have at least one hex char.
                do
                {
                    advanceChar();
                } while (isHex(peekChar()));

                return TokenNumberType::cIntHex;
            }
            case 'b':
            case 'B':
            {
                // Binary.
                advanceChar(); // 'b' or 'B'

                // We want at least one bin char or this is an error.
                if (isBinary(peekChar()) == false)
                {
                    SourceLocation loc = getCurrentLoc();
                    mCtx.report<cInvalidBinaryCharacter>(SourceRange::at(loc), peekChar());
                    return TokenNumberType::cInvalid;
                }

                // "0b" has been consumed and we have at least one bin char.
                do

                {
                    advanceChar();
                } while (isBinary(peekChar()));

                return TokenNumberType::cIntBinary;
            }
            default:
            {
                // We don't allow octal, so this has to be 0.
                if (isGenericNumberChar(peekChar()))
                {
                    mCtx.report<cLeadingZeroInteger>(getCurrentRange());
                    return TokenNumberType::cInvalid;
                }

                return TokenNumberType::cIntDecimal;
            }
        }
    }
    else
    {
        // Otherwise, just consume all the digits.
        while (isDigit(peekChar()))
        {
            advanceChar();
        }
    }

    // If we have no '.', then this has to be an int.
    if (advanceCharIfMatch('.') == false)
    {
        // Trailing characters are to be handled by the caller.
        return TokenNumberType::cIntDecimal;
    }

    // Float: Require at least one digit after the '.' or this is an error.
    if (isDigit(peekChar()) == false)
    {
        SourceLocation loc = getCurrentLoc();
        mCtx.report<cFloatLiteralMissingFraction>(SourceRange::at(loc));
        return TokenNumberType::cInvalid;
    }

    // Consume the rest of the digits.
    // Trailing zeroes are okay.
    while (isDigit(peekChar()))
    {
        advanceChar();
    }

    // Trailing characters are to be handled by the caller.
    return TokenNumberType::cFloat;
}

bool Tokenizer::consumeTrailingNumberChars()
{
    bool consumedAny = false;

    while (isGenericNumberChar(peekChar()))
    {
        consumedAny = true;
        advanceChar();
    }

    return consumedAny;
}

Token Tokenizer::makeNumberToken(char c)
{
    // Find out what type of number we have.
    TokenNumberType type = getTokenNumberType(c);

    // Consume the rest of the number characters.
    // If we consumed any, that is an error.
    if (consumeTrailingNumberChars())
    {
        Token t = makeToken(TokenType::cError);
        // Only report if we haven't already reported an error.
        if (type != TokenNumberType::cInvalid)
        {
            SourceRange range = t.getRange();
            std::string_view text = mSource.getSlice(range.getStartLoc(), range.getRangeLength());
            if (type == TokenNumberType::cFloat)
            {
                mCtx.report<cMalformedFloatLiteral>(range, text);
            }
            else
            {
                mCtx.report<cMalformedIntLiteral>(range, text);
            }
        }
        return t;
    }

    switch (type)
    {
        case TokenNumberType::cIntDecimal:
        case TokenNumberType::cIntHex:
        case TokenNumberType::cIntBinary:
        {
            return makeToken(TokenType::cInt, type);
        }
        case TokenNumberType::cFloat:
        {
            return makeToken(TokenType::cFloat, type);
        }
        default:
        {
            return makeToken(TokenType::cError);
        }
    }
}

TokenType Tokenizer::checkKeyword(u8 offset, const char* cmp, u8 cmpLen, TokenType tt) const
{
    // If we have the same length, this can be a match, otherwise it's not a keyword (and thus an identifier).
    if (mTokenCurrent - mTokenStart == cmpLen)
    {
        if (offset >= cmpLen)
        {
            // If we start comparing beyond the end, we consider it as a match.
            return tt;
        }

        // Otherwise, compare from the offset.
        if (std::memcmp(mTokenStart + offset, cmp + offset, cmpLen - offset) == 0)
        {
            return tt;
        }
    }

    return TokenType::cIdentifier;
}

TokenType Tokenizer::getIdentifierType() const
{
    // Trie-like keyword checking, also described in:
    // https://craftinginterpreters.com/scanning-on-demand.html#identifiers-and-keywords

    // Macro hell #1 begins.
    // clang-format off
#define CHECK(offset, cmp, tt) checkKeyword(offset, cmp, sizeof(cmp) - 1, tt)
#define INNER_SWITCH(level) \
    if (mTokenCurrent - mTokenStart > level) \
    { \
        switch (mTokenStart[level]) \
        {
#define INNER_SWITCH_END \
        } \
    } \
    break;

    switch (mTokenStart[0])
    {
        case 'a':
            INNER_SWITCH(1)
            case 's': return CHECK(2, "as", TokenType::cAs);
            INNER_SWITCH_END
        case 'b': return CHECK(1, "break", TokenType::cBreak);
        case 'c':
            INNER_SWITCH(1)
            case 'a':
                INNER_SWITCH(2)
                case 's':
                    INNER_SWITCH(3)
                    case 'e': return CHECK(4, "case", TokenType::cCase);
                    case 't': return CHECK(4, "cast", TokenType::cCast);
                    INNER_SWITCH_END
                INNER_SWITCH_END
            case 'l': return CHECK(2, "class", TokenType::cClass);
            case 'o':
                INNER_SWITCH(2)
                case 'n':
                    INNER_SWITCH(3)
                    case 's': return CHECK(4, "const", TokenType::cConst);
                    case 't': return CHECK(4, "continue", TokenType::cContinue);
                    INNER_SWITCH_END
                INNER_SWITCH_END
            INNER_SWITCH_END
        case 'd': return CHECK(1, "default", TokenType::cDefault);
        case 'e':
            INNER_SWITCH(1)
            case 'l': return CHECK(2, "else", TokenType::cElse);
            case 'x': return CHECK(2, "export", TokenType::cExport);
            INNER_SWITCH_END
        case 'f':
            INNER_SWITCH(1)
            case 'a': return CHECK(2, "false", TokenType::cBoolFalse);
            case 'o': return CHECK(2, "for", TokenType::cFor);
            case 'u': return CHECK(2, "fun", TokenType::cFun);
            INNER_SWITCH_END
        case 'i':
            INNER_SWITCH(1)
            case 'f': return CHECK(2, "if", TokenType::cIf);
            case 'm':
                INNER_SWITCH(2)
                case 'p':
                    INNER_SWITCH(3)
                    case 'l': return CHECK(4, "impl", TokenType::cImpl);
                    case 'o': return CHECK(4, "import", TokenType::cImport);
                    INNER_SWITCH_END
                INNER_SWITCH_END
            case 'n':
                INNER_SWITCH(2)
                case 'o': return CHECK(3, "inout", TokenType::cInOut);
                case 't': return CHECK(3, "interface", TokenType::cInterface);
                INNER_SWITCH_END
            INNER_SWITCH_END
        case 'm': return CHECK(1, "make", TokenType::cMake);
        case 'n':
            INNER_SWITCH(1)
            case 'e': return CHECK(2, "new", TokenType::cNew);
            case 'u': return CHECK(2, "null", TokenType::cNull);
            INNER_SWITCH_END
        case 'p':
            INNER_SWITCH(1)
            case 'r':
                INNER_SWITCH(2)
                case 'i':
                    INNER_SWITCH(3)
                    case 'n': return CHECK(4, "print", TokenType::cPrint);
                    case 'v': return CHECK(4, "private", TokenType::cPrivate);
                    INNER_SWITCH_END
                INNER_SWITCH_END
            INNER_SWITCH_END
        case 'r':
            INNER_SWITCH(1)
            case 'e': return CHECK(2, "return", TokenType::cReturn);
            INNER_SWITCH_END
        case 's':
            INNER_SWITCH(1)
            case 't': return CHECK(2, "struct", TokenType::cStruct);
            case 'w': return CHECK(2, "switch", TokenType::cSwitch);
            INNER_SWITCH_END
        case 't':
            INNER_SWITCH(1)
            case 'e': return CHECK(2, "template", TokenType::cTemplate);
            case 'h': return CHECK(2, "this", TokenType::cThis);
            case 'r': return CHECK(2, "true", TokenType::cBoolTrue);
            INNER_SWITCH_END
        case 'v': return CHECK(1, "var", TokenType::cVar);
        case 'w': return CHECK(1, "while", TokenType::cWhile);
        default: break;
    }

    return TokenType::cIdentifier;

#undef INNER_SWITCH_END
#undef INNER_SWITCH
#undef CHECK
    // clang-format on
    // Macro hell #1 ends.
}

Token Tokenizer::makeIdentifierToken()
{
    // Keep going with the current identifier until we have no more _, alphas, or digits.
    while (isAlphaOrUnderscore(peekChar()) || isDigit(peekChar()))
    {
        advanceChar();
    }

    return makeToken(getIdentifierType());
}

Token Tokenizer::scanNextToken()
{
    // Macro hell #2 begins.
    // clang-format off
#define advIfMatch(ch) advanceCharIfMatch(ch)
#define matchOnceOrTwice(ch, once, twice) \
    do \
    { \
        if(advIfMatch(ch)) return makeToken(twice); \
        return makeToken(once); \
    } while (false)
#define matchTwiceOrAss(ch, once, twice, ass) \
    do \
    { \
        if (advIfMatch(ch)) return makeToken(twice); \
        if (advIfMatch('=')) return makeToken(ass); \
        return makeToken(once); \
    } while (false)
#define matchTwiceAndAss(ch, once, twice, onceAss, twiceAss) \
    do \
    { \
        if (advIfMatch(ch)) return advIfMatch('=') ? makeToken(twiceAss) : makeToken(twice); \
        if (advIfMatch('=')) return makeToken(onceAss); \
        return makeToken(once); \
    } while (false)
    // clang-format on

    if (skipWhitespace() == false)
    {
        // Unterminated block comment already diagnosed.
        mTokenStart = mTokenCurrent;
        return makeToken(TokenType::cEOF);
    }

    // Set the start of the next token.
    // From here on, we only scan "valid" characters (whitespace and comments skipped).
    mTokenStart = mTokenCurrent;

    // If we're at the end, we're done.
    if (isAtEndChar())
    {
        return makeToken(TokenType::cEOF);
    }

    // Get the current char and advance.
    char c = advanceChar();

    if (isAlphaOrUnderscore(c))
    {
        return makeIdentifierToken();
    }

    // Int and float.
    if (isDigit(c))
    {
        return makeNumberToken(c);
    }

    // Check whether we entered a {} in a formatted string.
    bool inFormatExpression = (mFormatStack.empty() == false && mFormatStack.back().mInExpression);

    switch (c)
    {
        case '(': return makeToken(TokenType::cLeftParen);
        case ')': return makeToken(TokenType::cRightParen);
        case '{':
        {
            if (inFormatExpression)
            {
                // If we're already inside an expression and encounter a '{', inc the depth.
                ++mFormatStack.back().mDepth;
            }

            return makeToken(TokenType::cLeftBrace);
        }
        case '}':
        {
            if (inFormatExpression == false)
            {
                return makeToken(TokenType::cRightBrace);
            }

            // If we're inside an expression and encounter a '}', handle that.
            FormatFrame& frame = mFormatStack.back();

            if (frame.mDepth > 0)
            {
                // If we have a depth > 0 this means we're not yet done with the expression.
                --frame.mDepth;
                return makeToken(TokenType::cRightBrace);
            }

            // Otherwise, we're done with the expression for the current stack frame.
            frame.mInExpression = false;

            // Resume with the string section since we're done with the expression.
            return makeStringToken(true);
        }
        case '[': return makeToken(TokenType::cLeftBracket);
        case ']': return makeToken(TokenType::cRightBracket);
        case ',': return makeToken(TokenType::cComma);
        case '.': return makeToken(TokenType::cPeriod);
        case ';': return makeToken(TokenType::cSemiColon);
        case ':': matchOnceOrTwice(':', TokenType::cColon, TokenType::cDoubleColon);

        case '?': return makeToken(TokenType::cCond);

        case '!':
        {
            if (advIfMatch('='))
            {
                return makeToken(TokenType::cNE);
            }
            break;
        }
        case '=': return makeToken(advIfMatch('=') ? TokenType::cEQ : TokenType::cAss);
        case '+': return makeToken(advIfMatch('=') ? TokenType::cAssAdd : TokenType::cAdd);
        case '-':
        {
            if (advIfMatch('>'))
            {
                return makeToken(TokenType::cArrow);
            }

            return makeToken(advIfMatch('=') ? TokenType::cAssSub : TokenType::cSub);
        }
        case '*': return makeToken(advIfMatch('=') ? TokenType::cAssMul : TokenType::cMul);
        case '/': return makeToken(advIfMatch('=') ? TokenType::cAssDiv : TokenType::cDiv);
        case '%': return makeToken(advIfMatch('=') ? TokenType::cAssMod : TokenType::cMod);
        case '^': return makeToken(advIfMatch('=') ? TokenType::cAssXor : TokenType::cBitXor);

        // Characters that can be followed by the same character or an '=' for assignment.
        case '~': return makeToken(TokenType::cBitNot);
        case '|': matchTwiceOrAss('|', TokenType::cBitOr, TokenType::cOr, TokenType::cAssOr);
        case '&': matchTwiceOrAss('&', TokenType::cBitAnd, TokenType::cAnd, TokenType::cAssAnd);
        case '<': matchTwiceAndAss('<', TokenType::cLT, TokenType::cShiftL, TokenType::cLE, TokenType::cAssSl);
        case '>':
        {
            if (mSplitRightAngles)
            {
                return makeToken(TokenType::cGT);
            }

            matchTwiceAndAss('>', TokenType::cGT, TokenType::cShiftR, TokenType::cGE, TokenType::cAssSr);
        }

        case '"': return makeStringToken(false);
        case '$':
        {
            // If we have $", this is the start of a formatted string.
            if (advanceCharIfMatch('"'))
            {
                // Push a new stack frame.
                mFormatStack.push_back(FormatFrame{});
                return makeStringToken(true);
            }
            break;
        }

        default:
        {
            break;
        }
    }

    // If we get here, that's an error.
    Token t = makeToken(TokenType::cError);
    mCtx.report<cIllegalCharacter>(t.getRange(), c);
    return t;

#undef matchOnceOrTwice
#undef matchTwiceAndAss
#undef matchTwiceOrAss
#undef advIfMatch
    // Macro hell #2 ends.
}

} // namespace simlang
