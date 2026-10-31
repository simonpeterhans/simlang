#pragma once

#include <vector>

#include "runtime/callinfo.h"
#include "runtime/mapaccess.h"
#include "runtime/memory/keyopstable.h"
#include "runtime/memory/typelayouttable.h"
#include "runtime/runtimedebuginfo.h"
#include "runtime/stringdata.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct ExecutableImage
{
    std::vector<u8> mBytes;

    std::vector<FunctionInfo> mFunctionInfos;
    std::vector<FunctionIdx> mInterfaceMethods;
    std::vector<InterfaceCallInfo> mInterfaceCallInfos;
    std::vector<MapAccessInfo> mMapAccessInfos;
    std::vector<SyscallEntry> mSyscallInfos;

    TypeLayoutTable mTypeLayoutTable;
    KeyOpsTable mKeyOpsTable;

    StringPool mStrings;
    std::vector<StringFormatTemplate> mStringFormats;

    std::vector<VMWord> mInitialGlobals;

    RuntimeDebugInfo mDebugInfo;

    FunctionIdx mMainIndex = cInvalidFunctionIdx;
};

} // namespace simlang
