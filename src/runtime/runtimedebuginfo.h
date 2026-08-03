#pragma once

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

using RuntimeSourceID = u32;

inline constexpr RuntimeSourceID cInvalidRuntimeSourceID = std::numeric_limits<RuntimeSourceID>::max();

struct RuntimeSourceFile
{
    std::string mFilename;
};

struct RuntimeSourceLocation
{
    bool operator==(const RuntimeSourceLocation& other) const
    {
        return mSourceID == other.mSourceID && mLine == other.mLine && mColumn == other.mColumn;
    }

    bool isValid() const { return mSourceID != cInvalidRuntimeSourceID; }

    RuntimeSourceID mSourceID = cInvalidRuntimeSourceID;
    u32 mLine = 0;
    u32 mColumn = 0;
};

struct RuntimeSourceMapEntry
{
    VMAddress mStartAddress = cInvalidVMAddress;
    VMAddress mEndAddress = cInvalidVMAddress;
    RuntimeSourceLocation mLocation;
};

class RuntimeDebugInfo
{
public:
    void clear();

    void setSourceFile(RuntimeSourceID sourceID, std::string_view filename);
    void addSourceMapEntry(VMAddress startAddress, VMAddress endAddress, RuntimeSourceLocation location);

    const RuntimeSourceLocation* findSourceLocation(VMAddress address) const;
    std::string_view getSourceFilename(RuntimeSourceID sourceID) const;

private:
    std::vector<RuntimeSourceFile> mFiles;
    std::vector<RuntimeSourceMapEntry> mSourceMap;
};

} // namespace simlang
