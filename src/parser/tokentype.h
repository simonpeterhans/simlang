#pragma once

#include "util/types.h"

namespace simlang
{

enum class TokenType : u8
{
#define X(name, str) c##name,

#include "parser/tokentypes.def"

#undef X
    cCount
};

constexpr const char* tokenTypeToString(TokenType tt)
{
    switch (tt)
    {
#define X(name, str) \
    case TokenType::c##name: return str;

#include "parser/tokentypes.def"

#undef X
        default: return "???";
    }
}

} // namespace simlang
