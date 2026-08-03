#pragma once

#include <limits>
#include <vector>

#include "runtime/op/opcode.h"
#include "util/types.h"

namespace simlang
{

class BytecodeChunk;
enum class OpCode : u8;

using BytecodeCFGBlockIndex = u32;

constexpr BytecodeCFGBlockIndex cInvalidBytecodeCFGBlockIndex = std::numeric_limits<BytecodeCFGBlockIndex>::max();

bool isCFGBlockTerminatorOp(OpCode opCode);

enum class BytecodeCFGEdgeKind : u8
{
    cFallthrough,
    cJump,
    cConditionalTaken,
    cConditionalFallthrough,
    cTestTakenKeep,
    cTestFallthroughPop
};

struct BytecodeCFGEdge
{
    BytecodeCFGBlockIndex mTarget;
    BytecodeCFGEdgeKind mKind;
};

struct BytecodeCFGBlock
{
    usize mFirstOp = 0;
    usize mEndOp = 0;
    BytecodeCFGEdge mSuccessors[2] = {};
    u8 mSuccessorCount = 0;
};

struct BytecodeCFG
{
    static BytecodeCFG fromCode(const BytecodeChunk& code);

    std::vector<BytecodeCFGBlock> mBlocks;
    std::vector<BytecodeCFGBlockIndex> mLabelToBlock;
};

} // namespace simlang
