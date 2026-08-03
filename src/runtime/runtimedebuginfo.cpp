#include "runtime/runtimedebuginfo.h"

#include <algorithm>

namespace simlang
{

void RuntimeDebugInfo::clear()
{
    mFiles.clear();
    mSourceMap.clear();
}

void RuntimeDebugInfo::setSourceFile(RuntimeSourceID sourceID, std::string_view filename)
{
    if (sourceID == cInvalidRuntimeSourceID)
    {
        return;
    }

    usize index = sourceID;
    if (index >= mFiles.size())
    {
        mFiles.resize(index + 1);
    }

    RuntimeSourceFile& file = mFiles[index];
    file.mFilename = std::string{filename};
}

void RuntimeDebugInfo::addSourceMapEntry(VMAddress startAddress, VMAddress endAddress, RuntimeSourceLocation location)
{
    if (startAddress == cInvalidVMAddress || endAddress == cInvalidVMAddress || endAddress <= startAddress ||
        location.isValid() == false)
    {
        return;
    }

    if (mSourceMap.empty() == false && mSourceMap.back().mEndAddress == startAddress &&
        mSourceMap.back().mLocation == location)
    {
        mSourceMap.back().mEndAddress = endAddress;
        return;
    }

    mSourceMap.push_back(RuntimeSourceMapEntry{startAddress, endAddress, location});
}

const RuntimeSourceLocation* RuntimeDebugInfo::findSourceLocation(VMAddress address) const
{
    if (address == cInvalidVMAddress || mSourceMap.empty())
    {
        return nullptr;
    }

    auto it = std::upper_bound(mSourceMap.begin(),
                               mSourceMap.end(),
                               address,
                               [](VMAddress value, const RuntimeSourceMapEntry& entry)
                               {
                                   return value < entry.mStartAddress;
                               });

    if (it == mSourceMap.begin())
    {
        return nullptr;
    }

    --it;
    if (address >= it->mEndAddress)
    {
        return nullptr;
    }

    const RuntimeSourceLocation& location = it->mLocation;
    if (location.isValid() == false || static_cast<usize>(location.mSourceID) >= mFiles.size())
    {
        return nullptr;
    }

    return &location;
}

std::string_view RuntimeDebugInfo::getSourceFilename(RuntimeSourceID sourceID) const
{
    if (static_cast<usize>(sourceID) >= mFiles.size())
    {
        return {};
    }

    return mFiles[static_cast<usize>(sourceID)].mFilename;
}

} // namespace simlang
