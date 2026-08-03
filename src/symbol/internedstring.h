#pragma once

#include <limits>
#include <string_view>

#include "util/types.h"

namespace simlang
{

using InternedStringIdx = u32;

inline constexpr u64 cMaxInternedStringIdx = std::numeric_limits<InternedStringIdx>::max();
inline constexpr u64 cMaxInternedStringLength = std::numeric_limits<u16>::max();

struct InternedString
{
    const char* mData = nullptr;
    u16 mLength = 0;
    InternedStringIdx mIndex = 0;

    std::string_view toView() const
    {
        if (mLength == 0)
        {
            return {};
        }

        return std::string_view{mData, mLength};
    }
};

} // namespace simlang
