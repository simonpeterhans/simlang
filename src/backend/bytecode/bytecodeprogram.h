#pragma once

#include <vector>

#include "runtime/op/op.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "util/types.h"

namespace simlang
{

using BytecodeLabel = u32;

class BytecodeChunk
{
public:
    void emit(Op op, SourceRange sourceRange = cInvalidSourceRange)
    {
        mOps.push_back(op);
        mSourceRanges.push_back(sourceRange);
    }

    BytecodeLabel makeLabel() { return mLabelCount++; }
    u32 getLabelCount() const { return mLabelCount; }

    std::vector<Op>& getOps() { return mOps; }
    const std::vector<Op>& getOps() const { return mOps; }

    std::vector<SourceRange>& getSourceRanges() { return mSourceRanges; }
    const std::vector<SourceRange>& getSourceRanges() const { return mSourceRanges; }

private:
    std::vector<Op> mOps;
    std::vector<SourceRange> mSourceRanges;
    u32 mLabelCount = 0;
};

class FunctionBytecode
{
public:
    explicit FunctionBytecode(FunctionIdx functionIdx)
        : mFunctionIdx(functionIdx)
    {
    }

    FunctionIdx getFunctionIdx() const { return mFunctionIdx; }

    BytecodeChunk& getCode() { return mCode; }
    const BytecodeChunk& getCode() const { return mCode; }

private:
    BytecodeChunk mCode;
    FunctionIdx mFunctionIdx;
};

class ProgramBytecode
{
public:
    FunctionBytecode& addFunction(FunctionIdx functionIdx)
    {
        mFunctions.emplace_back(functionIdx);
        return mFunctions.back();
    }

    BytecodeChunk& getEntryCode() { return mEntry; }
    const BytecodeChunk& getEntryCode() const { return mEntry; }

    std::vector<FunctionBytecode>& getFunctions() { return mFunctions; }
    const std::vector<FunctionBytecode>& getFunctions() const { return mFunctions; }

private:
    BytecodeChunk mEntry;
    std::vector<FunctionBytecode> mFunctions;
};

} // namespace simlang
