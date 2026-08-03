#include "backend/backendstate.h"

#include <utility>

#include "runtime/executableimage.h"
#include "runtime/memory/typelayouttable.h"
#include "runtime/stringdata.h"

namespace simlang
{

ExecutableImage BackendState::toExecutableImage() &&
{
    ExecutableImage image;

    image.mBytes = std::move(mBytes);

    image.mFunctionInfos = std::move(mFunctionInfos);
    image.mInterfaceMethods = std::move(mInterfaceMethods);
    image.mInterfaceCallInfos = std::move(mInterfaceCallInfos);
    image.mSyscallInfos = std::move(mSyscallInfos);

    image.mTypeLayoutTable = std::move(mTypeLayoutTable).build();

    image.mStrings = std::move(mStrings).build();
    image.mStringFormats = std::move(mStringFormats).build();

    image.mInitialGlobals = std::move(mInitialGlobals);

    image.mDebugInfo = std::move(mDebugInfo);

    image.mMainIndex = mMainIndex;

    return image;
}

} // namespace simlang
