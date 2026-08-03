#pragma once

#include "runtime/vmdefines.h"

namespace simlang
{

struct FunctionInfo
{
    VMAddress mEntryAddress = cInvalidVMAddress;
    u32 mMaxStackWords = 0;
    FrameWordCount mArgWords = 0;
    FrameWordCount mLocalWords = 0;
    ReturnWordCount mReturnWords = 0;
};

struct InterfaceCallInfo
{
    InterfaceMethodSlot mSlot = 0;
    OpWordCount mArgWords = 0;
    ReturnWordCount mReturnWords = 0;
};

} // namespace simlang
