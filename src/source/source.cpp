#include "source/source.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace simlang
{

Source::Source(std::unique_ptr<char[]>&& src,
               u32 srcLen,
               u32 baseOffset,
               std::unique_ptr<char[]>&& filename,
               u32 filenameLen)
    : mSource(std::move(src))
    , mFilename(std::move(filename))
    , mSourceLength(srcLen)
    , mBaseOffset(baseOffset)
    , mFilenameLength(filenameLen)
{
}

void Source::buildLineInfos() const
{
    mLines.clear();

    // If the source is empty, this is trivial.
    if (mSourceLength == 0)
    {
        mLines.push_back(SourceLineInfo{});
        return;
    }

    const char* src = mSource.get();
    u32 lineStart = 0;

    // Otherwise, we go line by line.
    while (true)
    {
        u32 textEnd = lineStart;
        while (textEnd < mSourceLength && src[textEnd] != '\n' && src[textEnd] != '\r')
        {
            // Scan text until we reach a newline or the end of the source.
            ++textEnd;
        }

        // Add the line info.
        mLines.push_back(SourceLineInfo{lineStart, textEnd});

        // If we reached the end, we're done.
        if (textEnd >= mSourceLength)
        {
            return;
        }

        // Otherwise, we consume any potential \n or \r\n.
        u32 nextLineStart = textEnd;
        if (src[nextLineStart] == '\r')
        {
            ++nextLineStart;
            if (nextLineStart < mSourceLength && src[nextLineStart] == '\n')
            {
                ++nextLineStart;
            }
        }
        else // if (src[nextLineStart] == '\n')
        {
            ++nextLineStart;
        }

        if (nextLineStart >= mSourceLength)
        {
            // EOF belongs to the last visible line (!).
            return;
        }

        lineStart = nextLineStart;
    }
}

std::string_view Source::getSlice(SourceLocation loc, u32 len) const
{
    // If the caller wants nothing, give it nothing.
    if (len == 0)
    {
        return {};
    }

    // Compute the offset from the source loc.
    u32 offset = loc.getSourceOffset();
    if (offset < mBaseOffset)
    {
        // If it's before our start, bail.
        return {};
    }

    u32 localOffset = offset - mBaseOffset;
    if (localOffset >= mSourceLength)
    {
        // If it's after our end, also bail.
        return {};
    }

    // Clamp the desired length to our source length.
    u32 actualLen = std::min(len, mSourceLength - localOffset);

    return std::string_view{mSource.get() + localOffset, actualLen};
}

bool Source::lineOwnsOffset(u32 lineIndex, u32 offset) const
{
    if (lineIndex >= mLines.size())
    {
        return false;
    }

    // Get the target line and the next one to get a range like [line.mStartOffset, nextLine.mStartOffset).

    if (offset < mLines[lineIndex].mStartOffset)
    {
        // Before our target line, bail.
        return false;
    }

    if (lineIndex < mLines.size() - 1)
    {
        if (offset < mLines[lineIndex + 1].mStartOffset)
        {
            return true;
        }

        return false;
    }

    // If there is no next line, we can directly check against the source length.
    return offset <= mSourceLength;
}

u32 Source::findLineIndexForOffset(u32 offset) const
{
    // If we have only one line or no offset, we're done.
    if (mLines.size() <= 1 || offset == 0)
    {
        return 0;
    }

    // Check the last cached line.
    if (lineOwnsOffset(mLastLineIndex, offset))
    {
        return mLastLineIndex;
    }

    // Check the line after the cached line.
    if (mLastLineIndex < mLines.size() - 1 && lineOwnsOffset(mLastLineIndex + 1, offset))
    {
        ++mLastLineIndex;
        return mLastLineIndex;
    }

    // Check the line before the cached line.
    if (mLastLineIndex > 0 && lineOwnsOffset(mLastLineIndex - 1, offset))
    {
        --mLastLineIndex;
        return mLastLineIndex;
    }

    // Find the first like AFTER our offset (since we keep looking until value < line.mStartOffset).
    auto it = std::upper_bound(mLines.begin(),
                               mLines.end(),
                               offset,
                               [](u32 value, const SourceLineInfo& line)
                               {
                                   return value < line.mStartOffset;
                               });

    // Go to the previous line, which is the one we're interested in.
    u32 lineIndex = static_cast<u32>(std::distance(mLines.begin(), it)) - 1;
    mLastLineIndex = lineIndex;
    return lineIndex;
}

LineColumnInfo Source::getLineAndColumnFromOffset(u32 offset) const
{
    // We build the line infos lazily, so do that now if we didn't already.
    if (mLines.empty())
    {
        buildLineInfos();
    }

    u32 lineNumber = findLineIndexForOffset(offset);
    LineColumnInfo info;

    // Set the line number that we just obtained.
    info.mLineIndex = lineNumber;

    // Get the visible line string.
    const SourceLineInfo& line = mLines[lineNumber];
    info.mLine = std::string_view{mSource.get() + line.mStartOffset, line.mTextEndOffset - line.mStartOffset};

    // Also set the column number.
    // If the offset points at EOF or a newline byte, report the end of the visible line.
    // This can happen if we have e.g. a line ending like \r\n.
    u32 visibleOffset = std::min(offset, line.mTextEndOffset);
    info.mColumnIndex = visibleOffset - line.mStartOffset;

    return info;
}

std::string_view Source::getFilename() const
{
    if (mFilename == nullptr)
    {
        return {};
    }

    return std::string_view{mFilename.get(), mFilenameLength};
}

} // namespace simlang
