#pragma once

#include <limits>

#include "parser/tokentype.h"
#include "source/sourcerange.h"
#include "util/types.h"

namespace simlang
{

struct Identifier;

inline constexpr u16 cMaxTokenLen = std::numeric_limits<u16>::max();

enum class TokenNumberType : u8
{
    cInvalid,
    cNone,
    cFloat,
    cIntDecimal,
    cIntHex,
    cIntBinary
};

struct Token
{
    constexpr Token(SourceLocation loc, u16 len, TokenType type, TokenNumberType numberType, Identifier* identifier)
        : mIdentifier(identifier)
        , mLocation(loc)
        , mLength(len)
        , mType(type)
        , mNumberType(numberType)
    {
    }

    SourceRange makeRangeTo(Token other) const { return SourceRange{mLocation, other.getEndLoc()}; }
    SourceRange makeRangeTo(SourceRange other) const { return SourceRange{mLocation, other.getEndLoc()}; }

    // Note that ranges are always [start, end).
    SourceLocation getStartLoc() const { return mLocation; }
    SourceLocation getEndLoc() const { return mLocation.getAdvancedBy(mLength); }
    SourceRange getRange() const { return SourceRange{mLocation, getEndLoc()}; }

    Identifier* mIdentifier = nullptr;
    SourceLocation mLocation = cInvalidSourceLoc;
    u16 mLength = 0;
    TokenType mType = TokenType::cError;
    TokenNumberType mNumberType = TokenNumberType::cNone;
};

static_assert(sizeof(Token) == 16, "Token size exceeds 16 bytes (do you really want this?)!");

inline constexpr Token cErrorToken{cInvalidSourceLoc, 0, TokenType::cError, TokenNumberType::cNone, nullptr};

} // namespace simlang
