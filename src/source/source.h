#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "source/linecolumninfo.h"
#include "source/sourcelocation.h"
#include "util/types.h"

namespace simlang
{

struct SourceLineInfo
{
    // The offset in the file where the line starts.
    u32 mStartOffset = 0;
    // The offset where visible line text ends (relevant for \r\n and the likes).
    u32 mTextEndOffset = 0;
};

class Source
{
public:
    explicit Source(std::unique_ptr<char[]>&& src,
                    u32 srcLen,
                    u32 baseOffset,
                    std::unique_ptr<char[]>&& filename,
                    u32 filenameLen);
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    Source(Source&&) noexcept = default;
    Source& operator=(Source&&) noexcept = default;

    std::string_view getSlice(SourceLocation loc, u32 len) const;
    LineColumnInfo getLineAndColumnFromOffset(u32 offset) const;

    const char* getSource() const { return mSource.get(); }
    std::string_view getFilename() const;

    u32 getSourceLength() const { return mSourceLength; }
    u32 getBaseOffset() const { return mBaseOffset; }
    u32 getEndOffset() const { return mBaseOffset + mSourceLength; }
    SourceLocation getLocation(u32 localOffset) const { return SourceLocation{mBaseOffset + localOffset}; }

private:
    void buildLineInfos() const;
    bool lineOwnsOffset(u32 lineIndex, u32 offset) const;
    u32 findLineIndexForOffset(u32 offset) const;

    // This is mutable because we're lazily building the lines.
    // That allows us to still expose the getters as const (as they should be).
    mutable std::vector<SourceLineInfo> mLines;
    std::unique_ptr<char[]> mSource;
    std::unique_ptr<char[]> mFilename;
    u32 mSourceLength = 0;
    u32 mBaseOffset = 0;
    u32 mFilenameLength = 0;
    // Cache the index of the last line that was looked up.
    mutable u32 mLastLineIndex = 0;
};

} // namespace simlang
