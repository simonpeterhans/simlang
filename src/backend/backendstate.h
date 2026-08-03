#pragma once

#include <vector>

#include "backend/bytecode/bytecodeprogram.h"
#include "backend/layout/typelayouttablebuilder.h"
#include "backend/stringdata.h"
#include "runtime/callinfo.h"
#include "runtime/runtimedebuginfo.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/typeids.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct ExecutableImage;

struct BackendState
{
    ExecutableImage toExecutableImage() &&;

    bool hasValidMain() const { return mMainIndex != cInvalidFunctionIdx; }

    ProgramBytecode mProgramBytecode;

    std::vector<u8> mBytes;

    std::vector<FunctionInfo> mFunctionInfos;
    std::vector<FunctionIdx> mInterfaceMethods;
    std::vector<InterfaceCallInfo> mInterfaceCallInfos;
    std::vector<SyscallEntry> mSyscallInfos;

    TypeLayoutTableBuilder mTypeLayoutTable;

    StringPoolBuilder mStrings;
    StringFormatTableBuilder mStringFormats;

    std::vector<VMWord> mInitialGlobals;

    RuntimeDebugInfo mDebugInfo;

    FunctionIdx mMainIndex = cInvalidFunctionIdx;

    u32 mNextTypeID = cFirstUserTypeID;
};

} // namespace simlang
