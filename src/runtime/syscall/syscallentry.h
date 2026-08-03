#pragma once

#include "runtime/vmdefines.h"

namespace simlang
{

class VM;

struct SyscallEntry
{
    void* mFunction = nullptr;
    bool (*mCaller)(VM& vm, const SyscallEntry& entry) = nullptr;
    SyscallIdx mID = cInvalidSyscallIdx;
    OpWordCount mArgWords = 0;
    ReturnWordCount mReturnWords = 0;
};

} // namespace simlang
