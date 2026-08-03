#pragma once

#include <vector>

#include "parser/token.h"
#include "parser/tokentype.h"
#include "source/sourcelocation.h"
#include "source/sourcerange.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

class Source;
struct ParserContext;

class Tokenizer
{
public:
    explicit Tokenizer(ParserContext& ctx, const Source& source);

    Token scanNextToken();
    ScopedValueBinder<bool> scopedSplitRightAngles(bool splitRightAngles);

    usize getScannedTokenCount() const { return mScannedTokenCount; }
    const Source& getSource() const { return mSource; }

private:
    struct FormatFrame
    {
        u32 mDepth = 0;
        bool mInExpression = false;
        bool mNextSegmentIsStart = true;
    };

    bool isAtEndChar() const;
    char peekChar() const;
    char peekNextChar() const;
    char advanceChar();
    bool advanceCharIfMatch(char expected);

    SourceLocation getCurrentStartLoc() const;
    SourceLocation getCurrentLoc() const;
    SourceRange getCurrentRange() const;

    void skipSingleLineComment();
    bool skipBlockComment();
    bool skipWhitespace();

    Token makeToken(TokenType type, TokenNumberType numberType = TokenNumberType::cNone);

    Token makeStringToken(bool isFormatString);

    TokenNumberType getTokenNumberType(char c);
    bool consumeTrailingNumberChars();
    Token makeNumberToken(char c);

    TokenType checkKeyword(u8 offset, const char* cmp, u8 cmpLen, TokenType tt) const;
    TokenType getIdentifierType() const;
    Token makeIdentifierToken();

    ParserContext& mCtx;
    const Source& mSource;
    std::vector<FormatFrame> mFormatStack;
    const char* mSourceStart = nullptr;
    const char* mSourceEnd = nullptr;
    const char* mTokenStart = nullptr;
    const char* mTokenCurrent = nullptr;
    usize mScannedTokenCount = 0;
    bool mSplitRightAngles = false;
};

} // namespace simlang
