#pragma once

#include <string_view>

#include "util/types.h"

namespace simlang
{

struct LineColumnInfo
{
    std::string_view mLine;
    u32 mLineIndex = 0;
    u32 mColumnIndex = 0;
};

inline constexpr LineColumnInfo cInvalidLineColumnInfo{"\0", 0, 0};

} // namespace simlang
