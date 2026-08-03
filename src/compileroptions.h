#pragma once

#include <filesystem>

#include "backend/bytecode/bytecodedumpoptions.h"

namespace simlang
{

class TextSink;

struct CompilerOptions
{
    std::filesystem::path mSourcePath;
    std::filesystem::path mRootPath;
    BytecodeDumpOptions mBytecodeDump;
    TextSink* mLogSink = nullptr;
    bool mOptimize = true;
    bool mPrintSummary = true;
};

} // namespace simlang
