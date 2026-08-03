#pragma once

#include "util/types.h"

namespace simlang
{

class BytecodeChunk;
struct BackendState;
struct Op;

class BytecodeAnalyzer
{
public:
    explicit BytecodeAnalyzer(const BackendState& backend);

    bool computeMaxStackWords(const BytecodeChunk& code, u32 initialStackWords, u32& outMaxStackWords) const;

private:
    bool applyStackEffect(const Op& op, u32& height) const;

    const BackendState& mBackend;
};

} // namespace simlang
