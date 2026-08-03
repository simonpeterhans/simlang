#pragma once

#include <limits>
#include <string_view>

#include "util/types.h"

namespace simlang
{

inline constexpr u8 cMaxIdentifierLen = std::numeric_limits<u8>::max();

struct Identifier
{
    char* mName = nullptr;
    u32 mID = 0;
    u8 mLength = 0;
};

inline bool identifierEquals(Identifier* identifier, std::string_view name)
{
    return identifier != nullptr && std::string_view{identifier->mName, identifier->mLength} == name;
}

} // namespace simlang
