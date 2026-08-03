#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/unaryop.h"
#include "diag/diagnostictype.h"
#include "parser/parser.h"
#include "parser/parsercontext.h"
#include "parser/token.h"
#include "parser/tokenizer.h"
#include "parser/tokentype.h"
#include "source/source.h"
#include "source/sourcelocation.h"
#include "source/sourcerange.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/asserts.h"
#include "util/escaping.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

struct InternedString;
struct TypeSpecifierNode;

/* static */ constexpr std::array<Parser::ParseRule, static_cast<std::size_t>(TokenType::cCount)> Parser::
    makeExpressionRules()
{
    constexpr usize N = static_cast<usize>(TokenType::cCount);

    // Our parse rule table has an entry for every token type.
    std::array<ParseRule, N> a{};

    // Fill all the entries with defaults.
    for (usize i = 0; i < N; ++i)
    {
        a[i] = ParseRule{nullptr, nullptr, Precedence::cNone};
    }

    auto set = [&](TokenType t, PrefixParseFn pre, InfixParseFn inf, Precedence p)
    {
        a[static_cast<usize>(t)] = ParseRule{pre, inf, p};
    };

    // Now add the relevant entries.

    // clang-format off
    set(TokenType::cLeftParen,   &Parser::parseParen,                 &Parser::parseFunctionCall, Precedence::cUnaryPost);
    set(TokenType::cLeftBracket, nullptr,                              &Parser::parseIndexCall,    Precedence::cUnaryPost);
    set(TokenType::cPeriod,      nullptr,                              &Parser::parseMemberAccess, Precedence::cUnaryPost);
    set(TokenType::cDoubleColon, nullptr,                              &Parser::parseModuleAccess, Precedence::cScope);

    set(TokenType::cAdd,         nullptr,                              &Parser::parseBinary,       Precedence::cTerm);
    set(TokenType::cSub,         &Parser::parseUnaryPre,               &Parser::parseBinary,       Precedence::cTerm);
    set(TokenType::cMul,         nullptr,                              &Parser::parseBinary,       Precedence::cFactor);
    set(TokenType::cDiv,         nullptr,                              &Parser::parseBinary,       Precedence::cFactor);
    set(TokenType::cMod,         nullptr,                              &Parser::parseBinary,       Precedence::cFactor);
    set(TokenType::cShiftL,      nullptr,                              &Parser::parseBinary,       Precedence::cShift);
    set(TokenType::cShiftR,      nullptr,                              &Parser::parseBinary,       Precedence::cShift);

    set(TokenType::cCond,        nullptr,                              &Parser::parseConditional,  Precedence::cCond);

    set(TokenType::cNE,          nullptr,                              &Parser::parseBinary,       Precedence::cEquality);
    set(TokenType::cEQ,          nullptr,                              &Parser::parseBinary,       Precedence::cEquality);
    set(TokenType::cLT,          nullptr,                              &Parser::parseBinary,       Precedence::cComp);
    set(TokenType::cLE,          nullptr,                              &Parser::parseBinary,       Precedence::cComp);
    set(TokenType::cGT,          nullptr,                              &Parser::parseBinary,       Precedence::cComp);
    set(TokenType::cGE,          nullptr,                              &Parser::parseBinary,       Precedence::cComp);

    set(TokenType::cOr,          nullptr,                              &Parser::parseBinary,       Precedence::cOr);
    set(TokenType::cAnd,         nullptr,                              &Parser::parseBinary,       Precedence::cAnd);
    set(TokenType::cBitNot,      &Parser::parseUnaryPre,               nullptr,                    Precedence::cNone);
    set(TokenType::cBitOr,       nullptr,                              &Parser::parseBinary,       Precedence::cBitOr);
    set(TokenType::cBitAnd,      nullptr,                              &Parser::parseBinary,       Precedence::cBitAnd);
    set(TokenType::cBitXor,      nullptr,                              &Parser::parseBinary,       Precedence::cBitXor);

    set(TokenType::cIdentifier,  &Parser::parseIdentifier,             nullptr,                    Precedence::cNone);
    set(TokenType::cCast,        &Parser::parseCast,                   nullptr,                    Precedence::cNone);
    set(TokenType::cThis,        &Parser::parseThis,                   nullptr,                    Precedence::cNone);
    set(TokenType::cMake,        &Parser::parseMake,                   nullptr,                    Precedence::cUnaryPost);
    set(TokenType::cNew,         &Parser::parseNew,                    nullptr,                    Precedence::cUnaryPost);

    set(TokenType::cInt,         &Parser::parseInt,                    nullptr,                    Precedence::cNone);
    set(TokenType::cFloat,       &Parser::parseFloat,                  nullptr,                    Precedence::cNone);
    set(TokenType::cBoolTrue,    &Parser::parseBool,                   nullptr,                    Precedence::cNone);
    set(TokenType::cBoolFalse,   &Parser::parseBool,                   nullptr,                    Precedence::cNone);
    set(TokenType::cString,      &Parser::parseString,                 nullptr,                    Precedence::cNone);
    set(TokenType::cNull,        &Parser::parseNull,                   nullptr,                    Precedence::cNone);
    set(TokenType::cFormatStart, &Parser::parseFormattedString,        nullptr,                    Precedence::cNone);
    set(TokenType::cFormatEnd,   &Parser::parseFormattedString,        nullptr,                    Precedence::cNone);

    set(TokenType::cError,       &Parser::parseError,                  nullptr,                    Precedence::cPrimary);
    // clang-format on

    return a;
}

/* static */ const Parser::ParseRule* Parser::getExpressionRule(TokenType tt)
{
    static constexpr auto sRules = makeExpressionRules();
    return &sRules[static_cast<usize>(tt)];
}

ExpressionNode* Parser::parsePrecedence(Precedence prec)
{
    // We generally use getCurrentTokenType() here since the calls modify the token stream.
    ExpressionNode* lhs = nullptr;

    // Parse the prefix rule based on the token we're currently at.
    if (PrefixParseFn prefixRule = getExpressionRule(getCurrentTokenType())->mPrefix)
    {
        // Execute the rule.
        lhs = (this->*prefixRule)();
        // If we get null, then this was an error (already reported) and we're done.
        if (lhs == nullptr)
        {
            return nullptr;
        }
    }
    else
    {
        // Unknown operand token.
        // This cannot be cError because we have a prefix rule for it.
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), "operand", getCurrentTokenText());
        return nullptr;
    }

    // Now parse the infix rules as long as the current token has a >= precedence.
    while (getExpressionRule(getCurrentTokenType())->mPrecedence >= prec)
    {
        if (InfixParseFn infixRule = getExpressionRule(getCurrentTokenType())->mInfix)
        {
            lhs = (this->*infixRule)(lhs);
            if (lhs == nullptr)
            {
                return nullptr;
            }
        }
        else
        {
            // cError has no infix/postfix rule, so don't report that (again).
            if (getCurrentTokenType() != TokenType::cError)
            {
                mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), "operator", getCurrentTokenText());
            }
            return nullptr;
        }
    }

    return lhs;
}

ExpressionNode* Parser::parseError()
{
    // Consume the error token to avoid stalling.
    consume();
    return nullptr;
}

ExpressionNode* Parser::parseCast()
{
    // cast<T>(E)

    // Consume the "cast".
    Token castToken = consume();

    // We need to tell the tokenizer that we only want to split >> for the type.
    // For the subsequent expression, we need to accept >> again, hence the scoping.
    TypeSpecifierNode* typeSpec = nullptr;
    {
        auto splitRightAngles = mTokenizer.scopedSplitRightAngles(true);
        // Consume the "<".
        if (expect(TokenType::cLT, nullptr, true) == false)
        {
            return nullptr;
        }

        // Parse the type.
        typeSpec = parseTypeSpec();
        if (typeSpec == nullptr)
        {
            return nullptr;
        }

        // Consume the ">".
        if (expect(TokenType::cGT, nullptr, true) == false)
        {
            return nullptr;
        }
    }

    // Consume the "(".
    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the expression.
    ExpressionNode* child = parseExpression();
    if (child == nullptr)
    {
        return nullptr;
    }

    // Consume the ")".
    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    return mCtx.create<CastNode>(makeRangeToPrevious(castToken), typeSpec, child);
}

bool Parser::parseIntLiteral(Token token, i32& out) const
{
    const Source& source = mTokenizer.getSource();
    SourceRange literalRange = token.getRange();
    std::string_view tokenText = source.getSlice(token.mLocation, token.mLength);
    const char* startPtr = tokenText.data();

    u8 base;
    u8 offset;
    switch (token.mNumberType)
    {
        case TokenNumberType::cIntDecimal:
        {
            // Decimal (duh).
            base = 10;
            offset = 0;
            break;
        }
        case TokenNumberType::cIntHex:
        {
            // Hex, base 16, skip "0x".
            base = 16;
            offset = 2;
            break;
        }
        case TokenNumberType::cIntBinary:
        {
            // Binary, base 2, skip "0b".
            base = 2;
            offset = 2;
            break;
        }
        default:
        {
            return false;
        }
    }

    char* endPtr = nullptr;
    errno = 0;
    u64 value = std::strtoull(startPtr + offset, &endPtr, base);

    // The length of the token we parsed and what strtoull did has to match.
    // We ignore any offset because the startPtr remains unchanged as well.
    bool valid = ((endPtr - startPtr) == static_cast<isize>(tokenText.length()));
    if (valid == false)
    {
        mCtx.report<cInvalidIntLiteral>(literalRange, tokenText);
        return false;
    }

    // Check for u64 overflow.
    if (errno == ERANGE)
    {
        mCtx.report<cIntLiteralOutOfRange>(literalRange, tokenText);
        return false;
    }

    // Check for u32 overflow.
    if (token.mNumberType == TokenNumberType::cIntDecimal)
    {
        // Note that the +- discrepancy for 2147483648 is already handled by the unary operator.
        // Not the cleanest solution, but I couldn't think of anything better.
        constexpr u64 maxI32 = std::numeric_limits<i32>::max();
        if (value > maxI32)
        {
            mCtx.report<cIntLiteralOutOfRange>(literalRange, tokenText);
            return false;
        }

        out = static_cast<i32>(value);

        return true;
    }

    constexpr u64 maxI32 = std::numeric_limits<i32>::max();
    constexpr u64 maxU32 = std::numeric_limits<u32>::max();
    if (value > maxU32)
    {
        mCtx.report<cIntLiteralOutOfRange>(literalRange, tokenText);
        return false;
    }

    i64 signedValue =
        (value <= maxI32) ? static_cast<i64>(value) : static_cast<i64>(value) - (static_cast<i64>(maxU32) + 1);
    out = static_cast<i32>(signedValue);

    return true;
}

ExpressionNode* Parser::parseInt()
{
    Token token = getCurrentToken();

    i32 i = 0;
    bool ok = parseIntLiteral(token, i);

    // Consume the int.
    consume();

    if (ok == false)
    {
        return nullptr;
    }

    return mCtx.create<IntLiteralNode>(token.getRange(), i);
}

bool Parser::parseFloatLiteral(Token token, f32& out) const
{
    const Source& source = mTokenizer.getSource();
    std::string_view tokenText = source.getSlice(token.mLocation, token.mLength);

    // Convert the string to a float.
    char* endPtr;
    errno = 0;
    double d = std::strtof(tokenText.data(), &endPtr);

    // The length of the token we parsed and what strtof did has to match.
    bool valid = (endPtr - tokenText.data()) == static_cast<isize>(tokenText.length());
    if (valid == false)
    {
        mCtx.report<cInvalidFloatLiteral>(token.getRange(), tokenText);
        return false;
    }

    bool inRange =
        (std::isfinite(d) && (d <= std::numeric_limits<f32>::max()) && (d >= std::numeric_limits<f32>::lowest()));
    bool overflow = (errno == ERANGE);

    // See if we're in bounds.
    if (inRange == false || overflow)
    {
        mCtx.report<cFloatLiteralOutOfRange>(token.getRange(), tokenText);
        return false;
    }

    // Cast it down since we know it will fit into a float.
    out = static_cast<f32>(d);

    return true;
}

ExpressionNode* Parser::parseFloat()
{
    Token token = getCurrentToken();

    f32 f = 0.0f;
    bool ok = parseFloatLiteral(token, f);

    // Consume the float.
    consume();

    if (ok == false)
    {
        return nullptr;
    }

    return mCtx.create<FloatLiteralNode>(token.getRange(), f);
}

ExpressionNode* Parser::parseBool()
{
    Token token = getCurrentToken();

    bool value = (token.mType == TokenType::cBoolTrue) ? true : false;

    // Consume the bool.
    consume();

    return mCtx.create<BoolLiteralNode>(token.getRange(), value);
}

ExpressionNode* Parser::parseNull()
{
    Token token = getCurrentToken();

    // Consume the "null".
    consume();

    return mCtx.create<NullLiteralNode>(token.getRange());
}

bool Parser::parseStringTokenText(std::string& out)
{
    u8 prefixCount = 0;
    u8 suffixCount = 0;
    std::string_view tokenText = getCurrentTokenText();

    switch (getCurrentTokenType())
    {
        case TokenType::cFormatStart:
        {
            // $"abc {: 2 prefix, 1 suffix.
            prefixCount = 2;
            suffixCount = 1;
            break;
        }
        case TokenType::cFormatMid:
        {
            // } def {: 1 prefix, 1 suffix.
            prefixCount = 1;
            suffixCount = 1;
            break;
        }
        case TokenType::cFormatEnd:
        {
            // This can be either:
            // $"abc" (formatted string without any formatting)
            // } ghi" (end of a formatted string)
            if (tokenText[0] == '$')
            {
                prefixCount = 2;
            }
            else
            {
                prefixCount = 1;
            }
            suffixCount = 1;
            break;
        }
        case TokenType::cString:
        {
            // "abc": 1 prefix, 1 suffix.
            prefixCount = 1;
            suffixCount = 1;
            break;
        }
        default:
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), "string literal", getCurrentTokenText());
            return false;
        }
    }

    if (tokenText.length() < static_cast<usize>(prefixCount + suffixCount))
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), "valid string literal", getCurrentTokenText());
        return false;
    }

    out.clear();
    out.reserve(tokenText.length() - prefixCount - suffixCount);

    // Build the string from the token text without the prefix/suffix characters.
    usize end = tokenText.length() - suffixCount;
    for (usize i = prefixCount; i < end; ++i)
    {
        // Handle escapes.
        if (tokenText[i] == '\\')
        {
            // Get the rest of the token.
            std::string_view text = tokenText.substr(i + 1, end - i - 1);
            char escapedChar = '\0';
            usize charsConsumed = 0;

            // Try to decode it.
            bool decoded = escape::decodeEscapeSequence(text, charsConsumed, escapedChar);

            if (decoded == false)
            {
                SIMLANG_BREAK("Tokenizer accepted invalid string escape.");
                mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), "valid string escape", getCurrentTokenText());
                return false;
            }

            i += charsConsumed;
            out += escapedChar;

            continue;
        }

        out += tokenText[i];
    }

    // Actually consume the token.
    consume();

    return true;
}

ExpressionNode* Parser::parseString()
{
    Token token = getCurrentToken();

    // Parse the token text.
    std::string s;
    if (parseStringTokenText(s) == false)
    {
        return nullptr;
    }

    // Directly intern the string if it isn't already.
    const InternedString* internedStr = mCtx.internString(s, token.getRange());
    if (internedStr == nullptr)
    {
        return nullptr;
    }

    return mCtx.create<StringLiteralNode>(token.getRange(), internedStr);
}

ExpressionNode* Parser::parseFormattedString()
{
    Token startToken = getCurrentToken();

    // Example formatted string: $"abc {x} def {y} ghi"
    // The format start token would be something like $"abc {.
    // The format middle token would be something like } def {.
    // The format end token would be something like } ghi".
    // A $"plain" string without interpolation is tokenized as a format end.

    // The first one is always a literal.
    std::string literal;
    if (parseStringTokenText(literal) == false)
    {
        return nullptr;
    }

    // Intern that immediately.
    const InternedString* internedStr = mCtx.internString(literal, startToken.getRange());
    if (internedStr == nullptr)
    {
        return nullptr;
    }

    // If that was the last one, we only have a string literal and are done.
    if (startToken.mType == TokenType::cFormatEnd)
    {
        return mCtx.create<StringLiteralNode>(startToken.getRange(), internedStr);
    }

    // Otherwise, we expect an expression next and continue until we reach an end format string.
    std::vector<ExpressionNode*> args;
    std::vector<const InternedString*> literals;
    literals.push_back(internedStr);

    while (true)
    {
        // Parse the expression and push the result.
        ExpressionNode* expr = parseExpression();
        if (expr == nullptr)
        {
            return nullptr;
        }
        args.push_back(expr);

        // After the expression, we expect a format token.
        if (getCurrentTokenType() != TokenType::cFormatMid && getCurrentTokenType() != TokenType::cFormatEnd)
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), "format string segment", getCurrentTokenText());
            return nullptr;
        }

        bool isEnd = (getCurrentTokenType() == TokenType::cFormatEnd);

        // Parse the next literal and intern it.
        SourceRange literalRange = getCurrentTokenRange();
        if (parseStringTokenText(literal) == false)
        {
            return nullptr;
        }

        internedStr = mCtx.internString(literal, literalRange);
        if (internedStr == nullptr)
        {
            return nullptr;
        }

        literals.push_back(internedStr);

        // Check whether that was an ending segment.
        if (isEnd)
        {
            break;
        }
    }

    // Build the format node.
    SourceRange range = makeRangeToPrevious(startToken);
    ArrayView<const InternedString*> literalsView = makeArrayView(mCtx.mAllocator, literals);
    ArrayView<ExpressionNode*> argsView = makeArrayView(mCtx.mAllocator, args);

    return mCtx.create<FormatStringNode>(range, literalsView, argsView);
}

ExpressionNode* Parser::parseThis()
{
    Token token = getCurrentToken();

    // Consume the "this".
    consume();

    return mCtx.create<ThisNode>(token.getRange());
}

ExpressionNode* Parser::parseIdentifier()
{
    Token token = getCurrentToken();

    // Consume the identifier.
    consume();

    return mCtx.create<IdentifierNode>(token.getRange(), token.mIdentifier);
}

/* static */ UnaryOp Parser::getUnaryOp(TokenType tt)
{
    // clang-format off
    switch (tt)
    {
        case TokenType::cSub:       return UnaryOp::cNeg;
        case TokenType::cBitNot:    return UnaryOp::cBitNot;
        default:                    break;
    }
    // clang-format on

    return UnaryOp::cInvalid;
}

ExpressionNode* Parser::parseUnaryPre()
{
    // Consume the unary op.
    Token operatorToken = consume();

    UnaryOp opType = getUnaryOp(operatorToken.mType);
    if (opType == UnaryOp::cInvalid)
    {
        return nullptr;
    }

    // The one annoying special case for -2147483648.
    // Since int parsing doesn't know about negation, the maximum number we can parse is 2147483647.
    if (opType == UnaryOp::cNeg && check(TokenType::cInt) &&
        getCurrentToken().mNumberType == TokenNumberType::cIntDecimal)
    {
        static constexpr std::string_view cIntText = "2147483648";

        // Get the int token.
        Token intToken = getCurrentToken();

        // Get the source text of the token.
        const Source& source = mTokenizer.getSource();
        std::string_view tokenText = source.getSlice(intToken.mLocation, intToken.mLength);

        // Compare the text directly.
        if (tokenText == cIntText)
        {
            // Consume the int.
            consume();

            // Directly create the int literal without the unary op.
            return mCtx.create<IntLiteralNode>(operatorToken.makeRangeTo(intToken), std::numeric_limits<i32>::min());
        }
    }

    // Parse the expression.
    ExpressionNode* expr = parsePrecedence(Precedence::cUnaryPre);
    if (expr == nullptr)
    {
        return nullptr;
    }

    return mCtx.create<UnaryOpNode>(makeRange(operatorToken, expr), opType, expr);
}

/* static */ BinaryOp Parser::getBinaryOp(TokenType tt)
{
    // clang-format off
    switch (tt)
    {
        case TokenType::cAdd:    return BinaryOp::cAdd;
        case TokenType::cSub:    return BinaryOp::cSub;
        case TokenType::cMul:    return BinaryOp::cMul;
        case TokenType::cDiv:    return BinaryOp::cDiv;
        case TokenType::cMod:    return BinaryOp::cMod;
        case TokenType::cShiftL: return BinaryOp::cShiftL;
        case TokenType::cShiftR: return BinaryOp::cShiftR;

        case TokenType::cNE:     return BinaryOp::cNE;
        case TokenType::cEQ:     return BinaryOp::cEQ;
        case TokenType::cLT:     return BinaryOp::cLT;
        case TokenType::cLE:     return BinaryOp::cLE;
        case TokenType::cGT:     return BinaryOp::cGT;
        case TokenType::cGE:     return BinaryOp::cGE;

        case TokenType::cOr:     return BinaryOp::cOr;
        case TokenType::cAnd:    return BinaryOp::cAnd;
        case TokenType::cBitOr:  return BinaryOp::cBitOr;
        case TokenType::cBitAnd: return BinaryOp::cBitAnd;
        case TokenType::cBitXor: return BinaryOp::cBitXor;

        default:                 break;
    }
    // clang-format on

    return BinaryOp::cInvalid;
}

ExpressionNode* Parser::parseBinary(ExpressionNode* lhs)
{
    // Get the parse rule for the current token type.
    TokenType tokenType = getCurrentTokenType();
    const ParseRule* rule = getExpressionRule(tokenType);

    // Get the binary op.
    BinaryOp op = getBinaryOp(tokenType);
    if (op == BinaryOp::cInvalid)
    {
        return nullptr;
    }

    // Consume the op.
    consume();

    // Handle left-associativity: a - b - c -> (a - b) - c.
    // For this, we don't want same-precedence operators to bind inside the RHS.
    // Therefore, we increment the precedence by 1 so parsePrecedence only parses stuff of higher precedence.
    auto nextPrecedenceValue = static_cast<PrecedenceType>(static_cast<PrecedenceType>(rule->mPrecedence) + 1);
    if (nextPrecedenceValue >= static_cast<PrecedenceType>(Precedence::cCount))
    {
        return nullptr;
    }
    Precedence nextPrecedence = static_cast<Precedence>(nextPrecedenceValue);

    // Parse the RHS.
    ExpressionNode* rhs = parsePrecedence(nextPrecedence);
    if (rhs == nullptr)
    {
        return nullptr;
    }

    return mCtx.create<BinaryOpNode>(lhs->makeRangeTo(rhs), op, lhs, rhs);
}

ExpressionNode* Parser::parseConditional(ExpressionNode* lhs)
{
    // condExpr ? thenExpr : elseExpr

    // Consume the '?'.
    consume();

    // Parse the "then" expression.
    // Handle right-associativity: a ? b : c ? d : e -> a ? b : (c ? d : e).
    // For that, we continue with the same precedence.
    ExpressionNode* thenExpr = parsePrecedence(Precedence::cCond);
    if (thenExpr == nullptr)
    {
        return nullptr;
    }

    // Parse the ':'.
    if (expect(TokenType::cColon, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the "else" expression.
    // Same as for the "then" expression.
    ExpressionNode* elseExpr = parsePrecedence(Precedence::cCond);
    if (elseExpr == nullptr)
    {
        return nullptr;
    }

    return mCtx.create<TernaryExprNode>(lhs->makeRangeTo(elseExpr), lhs, thenExpr, elseExpr);
}

ExpressionNode* Parser::parseModuleAccess(ExpressionNode* lhs)
{
    // ::identifier

    // If the LHS isn't an identifier or module access, this is an error.
    if (lhs->mNodeType != NodeType::cIdentifier && lhs->mNodeType != NodeType::cModuleAccess)
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cIdentifier, getCurrentTokenText());
    }

    // Consume the "::".
    consume();

    // If the next token isn't an identifier, this is an error.
    Token identifierToken = cErrorToken;
    if (expect(TokenType::cIdentifier, &identifierToken, true) == false)
    {
        return nullptr;
    }
    auto* rhs = mCtx.create<IdentifierNode>(identifierToken.getRange(), identifierToken.mIdentifier);

    return mCtx.create<ModuleAccessNode>(lhs->makeRangeTo(rhs), lhs, rhs);
}

ExpressionNode* Parser::parseParen()
{
    // Consume the '('.
    consume();

    // Parse the expression.
    ExpressionNode* expr = parseExpression();
    if (expr == nullptr)
    {
        return nullptr;
    }

    // Consume the ')'.
    if (tryConsume(TokenType::cRightParen) == false)
    {
        mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightParen, getCurrentTokenText());
        return nullptr;
    }

    return expr;
}

ExpressionNode* Parser::parseMake()
{
    // make ValueType { field0: value0, field1: value1 }

    // Consume the "make".
    Token makeToken = consume();

    // Parse the type specifier.
    TypeSpecifierNode* typeSpec = parseTypeSpec();
    if (typeSpec == nullptr)
    {
        return nullptr;
    }

    return parseObjectConstruction(makeToken, typeSpec, ConstructionKind::cValue);
}

ExpressionNode* Parser::parseNew()
{
    // new ReferenceType(arg0, arg1)

    // Consume the "new".
    Token newToken = consume();

    // Parse the type specifier.
    TypeSpecifierNode* typeSpec = parseTypeSpec();
    if (typeSpec == nullptr)
    {
        return nullptr;
    }

    return parseClassConstruction(newToken, typeSpec);
}

bool Parser::parseArgumentList(std::vector<ExpressionNode*>& args)
{
    // e0, e1, ...)

    // We came here from a '(', if we have a ')' we're already done.
    if (check(TokenType::cRightParen))
    {
        consume();
        return true;
    }

    while (true)
    {
        // Consume the "inout" if we have one.
        bool isInOutArgument = tryConsume(TokenType::cInOut);

        // Parse the expression.
        ExpressionNode* arg = parseExpression();
        if (arg == nullptr)
        {
            return false;
        }
        arg->mFlags.set(cExprIsInOutArgument, isInOutArgument);
        args.push_back(arg);

        // Check whether to expect more args.
        if (check(TokenType::cComma))
        {
            // Consume the ','.
            consume();
            continue;
        }

        // Check if we're done.
        if (check(TokenType::cRightParen))
        {
            // Consume the ')'.
            consume();
            return true;
        }

        TokenType currentType = getCurrentTokenType();
        if (currentType == TokenType::cSemiColon || currentType == TokenType::cRightBrace ||
            currentType == TokenType::cEOF)
        {
            mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightParen, currentType);
        }
        else
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                          std::array{TokenType::cComma, TokenType::cRightParen},
                                          getCurrentTokenText());
        }

        return false;
    }
}

ExpressionNode* Parser::parseClassConstruction(Token constructionToken, TypeSpecifierNode* typeSpec)
{
    // (e0, e1, ...) (called from new ...)

    // We open with a '(' or this is an error.
    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the argument list.
    std::vector<ExpressionNode*> args;
    if (parseArgumentList(args) == false)
    {
        return nullptr;
    }

    ArrayView<FieldInitializer*> fieldsView;
    ArrayView<ExpressionNode*> argsView = makeArrayView(mCtx.mAllocator, args);

    return mCtx.create<NewObjectNode>(makeRangeToPrevious(constructionToken),
                                      typeSpec,
                                      fieldsView,
                                      argsView,
                                      ConstructionKind::cReference);
}

ExpressionNode* Parser::parseObjectConstruction(Token constructionToken,
                                                TypeSpecifierNode* typeSpec,
                                                ConstructionKind constructionKind)
{
    // {}
    // { i0: expr, i1: expr, ... }
    // { i0, i1, ... }

    std::vector<FieldInitializer*> fields;

    auto buildNode = [&]()
    {
        ArrayView<FieldInitializer*> fieldsView = makeArrayView(mCtx.mAllocator, fields);
        ArrayView<ExpressionNode*> argsView;

        return mCtx.create<NewObjectNode>(makeRangeToPrevious(constructionToken),
                                          typeSpec,
                                          fieldsView,
                                          argsView,
                                          constructionKind);
    };

    // We open with a '{' or this is an error.
    if (expect(TokenType::cLeftBrace, nullptr, true) == false)
    {
        return nullptr;
    }

    // Check if we're already done.
    if (check(TokenType::cRightBrace))
    {
        // Consume the '}'.
        consume();
        return buildNode();
    }

    while (true)
    {
        // Parse the identifier.
        Token identifierToken = cErrorToken;
        if (expect(TokenType::cIdentifier, &identifierToken, true) == false)
        {
            return nullptr;
        }

        // Parse the expression.
        ExpressionNode* value = nullptr;
        if (tryConsume(TokenType::cColon))
        {
            value = parseExpression();
        }
        else
        {
            // Allow something like make Point { x, y } if the identifiers have the same name as the members.
            value = mCtx.create<IdentifierNode>(identifierToken.getRange(), identifierToken.mIdentifier);
        }

        if (value == nullptr)
        {
            return nullptr;
        }

        SourceRange fieldRange = identifierToken.makeRangeTo(value->mSourceRange);
        fields.push_back(
            mCtx.create<FieldInitializer>(fieldRange, identifierToken.getRange(), identifierToken.mIdentifier, value));

        // Find out if we have more.
        if (check(TokenType::cComma))
        {
            // Consume the ','.
            consume();
            continue;
        }

        // Find out if we're done.
        if (check(TokenType::cRightBrace))
        {
            // Consume the '}'.
            consume();
            return buildNode();
        }

        TokenType currentType = getCurrentTokenType();
        if (currentType == TokenType::cSemiColon || currentType == TokenType::cRightParen ||
            currentType == TokenType::cEOF)
        {
            mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightBrace, currentType);
        }
        else
        {
            mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                          std::array{TokenType::cComma, TokenType::cRightBrace},
                                          getCurrentTokenText());
        }

        return nullptr;
    }
}

ExpressionNode* Parser::parseFunctionCall(ExpressionNode* lhs)
{
    // (e0, e1, ...)

    // Consume the '('.
    consume();

    std::vector<ExpressionNode*> args;

    // Parse the argument list (which will consume the ')').
    if (parseArgumentList(args) == false)
    {
        return nullptr;
    }

    ArrayView<ExpressionNode*> argsView = makeArrayView(mCtx.mAllocator, args);

    return mCtx.create<FunctionCallNode>(makeRangeToPrevious(lhs), lhs, argsView);
}

ExpressionNode* Parser::parseIndexCall(ExpressionNode* lhs)
{
    // [expr]

    // Consume the '['.
    consume();

    // Parse the expression.
    ExpressionNode* index = parseExpression();
    if (index == nullptr)
    {
        return nullptr;
    }

    // Consume the ']'.
    if (tryConsume(TokenType::cRightBracket) == false)
    {
        mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cRightBracket, getCurrentTokenText());
        return nullptr;
    }

    return mCtx.create<IndexCallNode>(makeRangeToPrevious(lhs), lhs, index);
}

ExpressionNode* Parser::parseMemberAccess(ExpressionNode* lhs)
{
    // .identifier

    // Consume the '.'.
    consume();

    // Parse the identifier.
    Token identifierToken = cErrorToken;
    if (expect(TokenType::cIdentifier, &identifierToken, true) == false)
    {
        return nullptr;
    }

    return mCtx.create<MemberAccessNode>(makeRangeToPrevious(lhs), lhs, identifierToken.mIdentifier);
}

ExpressionNode* Parser::parseExpression()
{
    return parsePrecedence(Precedence::cAss);
}

} // namespace simlang
