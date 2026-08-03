#pragma once

#include <vector>

#include "runtime/runtimedebuginfo.h"
#include "runtime/vmdefines.h"

namespace simlang
{

class BytecodeChunk;
class FunctionBytecode;
class SourceRange;
struct CompilerContext;

class BytecodeEmitter
{
public:
    explicit BytecodeEmitter(CompilerContext& context);

    bool emit();

private:
    RuntimeSourceLocation getRuntimeSourceLocation(const SourceRange& range) const;
    void addRuntimeSourceMapEntry(SourceRange sourceRange, VMAddress startAddress, VMAddress endAddress);

    VMAddress getCurrentAddress() const;

    bool resolveLabels();
    bool resolveLabelsForCode(const BytecodeChunk& code, std::vector<VMAddress>& labelAddresses);
    bool computeStackDepths();
    void emitCode(const BytecodeChunk& code, const std::vector<VMAddress>& labelAddresses);
    void emitFunction(const FunctionBytecode& function, const std::vector<VMAddress>& labelAddresses);

    CompilerContext& mCtx;

    std::vector<VMAddress> mEntryLabelAddresses;
    std::vector<std::vector<VMAddress>> mFunctionLabelAddresses;

    VMAddress mBytecodeSize = 0;
};

} // namespace simlang
