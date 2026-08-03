#pragma once

#include <limits>
#include <string>
#include <vector>

#include "source/source.h"
#include "util/types.h"

namespace simlang
{

class SourceLocation;

using SourceID = u32;

inline constexpr SourceID cInvalidSourceID = std::numeric_limits<SourceID>::max();

enum class SourceLoadError : u8
{
    cNone,
    cSourceTooLong,
    cFailedToOpen,
    cFailedToRead,
};

struct SourceLoadResult
{
    bool hasError() const { return mError != SourceLoadError::cNone; }

    const Source* mSource = nullptr;
    SourceLoadError mError = SourceLoadError::cNone;
};

struct ResolvedSourceLocation
{
    bool isValid() const { return mSource != nullptr; }

    const Source* mSource = nullptr;
    SourceID mSourceID = cInvalidSourceID;
    u32 mLocalOffset = 0;
};

class SourceManager
{
public:
    SourceLoadResult addSource(const std::string& sourcePath);
    ResolvedSourceLocation resolveLocation(SourceLocation loc) const;

    const Source* getSource(SourceID sourceID) const;
    usize getSourceCount() const;

private:
    std::vector<Source> mSources;
    u32 mNextBaseOffset = 0;
    // Small cache for the last resolved source.
    // This is because we commonly resolve locations from the same source after each other.
    // Mutable since the getter and setter methods are const, but the cache needs to be updated.
    mutable SourceID mLastResolvedSourceID = cInvalidSourceID;
};

} // namespace simlang
