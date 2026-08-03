#pragma once

#include <limits>

#include "util/types.h"

namespace simlang
{

inline constexpr u32 cInvalidSourceOffset = std::numeric_limits<u32>::max();
inline constexpr u32 cMaxSourceOffset = cInvalidSourceOffset - 1U;

class SourceLocation
{
public:
    constexpr SourceLocation() = default;

    explicit constexpr SourceLocation(u32 offset)
        : mOffset(offset)
    {
    }

    constexpr bool operator==(const SourceLocation& other) const { return mOffset == other.mOffset; }
    constexpr bool operator!=(const SourceLocation& other) const { return mOffset != other.mOffset; }
    constexpr bool operator<(const SourceLocation& other) const { return mOffset < other.mOffset; }
    constexpr bool operator<=(const SourceLocation& other) const { return mOffset <= other.mOffset; }
    constexpr bool operator>(const SourceLocation& other) const { return mOffset > other.mOffset; }
    constexpr bool operator>=(const SourceLocation& other) const { return mOffset >= other.mOffset; }

    constexpr u32 getSourceOffset() const { return mOffset; }

    constexpr SourceLocation getAdvancedBy(u32 offset) const
    {
        return (mOffset <= cMaxSourceOffset && offset <= cMaxSourceOffset - mOffset)
                   ? SourceLocation{mOffset + offset}
                   : SourceLocation{cInvalidSourceOffset};
    }

private:
    u32 mOffset = cInvalidSourceOffset;
};

inline constexpr SourceLocation cInvalidSourceLoc{cInvalidSourceOffset};

} // namespace simlang
