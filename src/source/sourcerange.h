#pragma once

#include "source/sourcelocation.h"
#include "util/types.h"

namespace simlang
{

class SourceRange
{
public:
    constexpr SourceRange() = default;

    constexpr SourceRange(SourceLocation start, SourceLocation end)
        : mStart(start)
        , mEnd(end)
    {
    }

    static constexpr SourceRange at(SourceLocation start, u32 length)
    {
        return SourceRange{start, start.getAdvancedBy(length)};
    }
    static constexpr SourceRange at(SourceLocation loc) { return SourceRange{loc, loc}; }

    constexpr bool operator==(const SourceRange& other) const
    {
        return (mStart == other.mStart) && (mEnd == other.mEnd);
    }
    constexpr bool operator!=(const SourceRange& other) const { return (*this == other) == false; }

    constexpr bool isValid() const
    {
        return (mStart == cInvalidSourceLoc) == false && (mEnd == cInvalidSourceLoc == false) && (mStart <= mEnd);
    }

    constexpr SourceLocation getStartLoc() const { return mStart; }
    constexpr SourceLocation getEndLoc() const { return mEnd; }

    bool isEmpty() const { return (getRangeLength() == 0); }
    u32 getRangeLength() const
    {
        if (isValid() == false)
        {
            return 0;
        }

        // start <= end is already checked in isValid().
        u32 startOffset = mStart.getSourceOffset();
        u32 endOffset = mEnd.getSourceOffset();

        return endOffset - startOffset;
    }

private:
    SourceLocation mStart = cInvalidSourceLoc;
    SourceLocation mEnd = cInvalidSourceLoc;
};

inline constexpr SourceRange cInvalidSourceRange{cInvalidSourceLoc, cInvalidSourceLoc};

} // namespace simlang
