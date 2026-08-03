#include "source/sourcemanager.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>

#include "source/sourcelocation.h"

namespace simlang
{

SourceLoadResult SourceManager::addSource(const std::string& sourcePath)
{
    // Open a stream and go to the end.
    std::ifstream in{sourcePath, std::ios::binary | std::ios::ate};
    if (in.fail())
    {
        return SourceLoadResult{nullptr, SourceLoadError::cFailedToOpen};
    }

    // Figure out the size by checking out the end.
    std::streamsize fileSize = in.tellg();
    if (fileSize < 0)
    {
        return SourceLoadResult{nullptr, SourceLoadError::cFailedToRead};
    }

    // Don't allow sources that would make the length of all sources exceed the max offset.
    // mNextBaseOffset can be cInvalidSourceOffset if the last source reached the exact max offset.
    if (mNextBaseOffset == cInvalidSourceOffset ||
        static_cast<u64>(fileSize) > static_cast<u64>(cMaxSourceOffset - mNextBaseOffset))
    {
        return SourceLoadResult{nullptr, SourceLoadError::cSourceTooLong};
    }

    // Convert to u32.
    u32 srcSize = static_cast<u32>(fileSize);
    u32 baseOffset = mNextBaseOffset;

    // Go back.
    in.seekg(0, std::ios::beg);

    // Allocate memory.
    std::unique_ptr<char[]> srcBuf{new char[srcSize + 1]};

    // Read.
    in.read(srcBuf.get(), srcSize);
    if (in.fail())
    {
        return SourceLoadResult{nullptr, SourceLoadError::cFailedToRead};
    }

    // Don't forget to null-terminate.
    srcBuf[srcSize] = '\0';

    // Also put the file name into a unique ptr because why not.
    usize pathLen = sourcePath.size();
    if (pathLen >= std::numeric_limits<u32>::max())
    {
        return SourceLoadResult{nullptr, SourceLoadError::cSourceTooLong};
    }

    std::unique_ptr<char[]> pathBuf{new char[pathLen + 1]};

    // Copy it in and null-terminate.
    std::memcpy(pathBuf.get(), sourcePath.data(), pathLen);
    pathBuf[pathLen] = '\0';

    // Make the source.
    mSources.emplace_back(std::move(srcBuf), srcSize, baseOffset, std::move(pathBuf), static_cast<u32>(pathLen));

    // Update the offset by adding the size of the source.
    u32 endOffset = baseOffset + srcSize;
    // The next one starts after the end of this source, thus add 1.
    mNextBaseOffset = (endOffset < cMaxSourceOffset) ? (endOffset + 1U) : cInvalidSourceOffset;

    return SourceLoadResult{&mSources.back(), SourceLoadError::cNone};
}

ResolvedSourceLocation SourceManager::resolveLocation(SourceLocation loc) const
{
    if (loc == cInvalidSourceLoc)
    {
        return {};
    }

    u32 offset = loc.getSourceOffset();

    // If we have a cache, check that first.
    if (mLastResolvedSourceID != cInvalidSourceID)
    {
        const Source& cachedSource = mSources[mLastResolvedSourceID];
        // The location has to be within the cached source's range.
        if (offset >= cachedSource.getBaseOffset() && offset <= cachedSource.getEndOffset())
        {
            // Then we can take the shortcut.
            return ResolvedSourceLocation{&cachedSource, mLastResolvedSourceID, offset - cachedSource.getBaseOffset()};
        }
    }

    // Binary search for the source containing the given offset.
    // This returns the first source that starts after the given offset.
    auto it = std::upper_bound(mSources.begin(),
                               mSources.end(),
                               offset,
                               [](u32 value, const Source& source)
                               {
                                   return value < source.getBaseOffset();
                               });

    // If we found none, we're done.
    if (it == mSources.begin())
    {
        return {};
    }

    // Otherwise, step back to the source before the one that was found, which is the one we're looking for.
    --it;
    if (offset > it->getEndOffset())
    {
        return {};
    }

    // Resolve the location and update the cache.
    SourceID sourceID = static_cast<SourceID>(it - mSources.begin());
    mLastResolvedSourceID = sourceID;
    u32 localOffset = offset - it->getBaseOffset();
    return ResolvedSourceLocation{&*it, sourceID, localOffset};
}

const Source* SourceManager::getSource(SourceID sourceID) const
{
    if (sourceID == cInvalidSourceID || static_cast<usize>(sourceID) >= mSources.size())
    {
        return nullptr;
    }

    return &mSources[static_cast<usize>(sourceID)];
}

usize SourceManager::getSourceCount() const
{
    return mSources.size();
}

} // namespace simlang
