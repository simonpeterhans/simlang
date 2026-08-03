#include "backend/bytecode/bytecodecfg.h"

#include "backend/bytecode/bytecodeprogram.h"
#include "runtime/op/op.h"
#include "runtime/op/opcode.h"

namespace simlang
{

bool isCFGBlockTerminatorOp(OpCode opCode)
{
    switch (opCode)
    {
        case OpCode::cJump:
        case OpCode::cJumpZ:
        case OpCode::cJumpNZ:
        case OpCode::cTestZ:
        case OpCode::cTestNZ:
        case OpCode::cReturn:
        case OpCode::cHalt:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

BytecodeCFG BytecodeCFG::fromCode(const BytecodeChunk& code)
{
    const std::vector<Op>& ops = code.getOps();

    BytecodeCFG cfg;
    cfg.mLabelToBlock.resize(code.getLabelCount(), cInvalidBytecodeCFGBlockIndex);

    std::vector<usize> blockStarts;

    auto addBlockStart = [&](usize idx)
    {
        // Some checks.
        // If this is the last op, don't start a new block.
        if (idx >= ops.size())
        {
            return;
        }

        // If we already added this block start before, skip.
        // This can happen if we have multiple consecutive block terminators/labels.
        if (blockStarts.empty() || blockStarts.back() != idx)
        {
            blockStarts.push_back(idx);
        }
    };

    // Seed with the first block (duh).
    addBlockStart(0);

    for (usize i = 0; i < ops.size(); ++i)
    {
        const Op& op = ops[i];

        if (op.mOpCode == OpCode::cLabel)
        {
            // If we have a label that is not preceded by another label, this starts a new block.
            if (i == 0 || ops[i - 1].mOpCode != OpCode::cLabel)
            {
                addBlockStart(i);
            }
        }
        else if (isCFGBlockTerminatorOp(op.mOpCode))
        {
            // If we have a block terminator and are not at the end, the next block is a new one.
            addBlockStart(i + 1);
        }
    }

    // After this, we know where all of our blocks start, so build them now.
    cfg.mBlocks.reserve(blockStarts.size());

    for (usize i = 0; i < blockStarts.size(); ++i)
    {
        // The first op is where the block starts.
        usize firstOp = blockStarts[i];
        // The last op (exclusive) is where the next block starts, or the end of the ops if this is the last block.
        usize endOp = (i + 1 < blockStarts.size()) ? blockStarts[i + 1] : ops.size();

        cfg.mBlocks.push_back(BytecodeCFGBlock{firstOp, endOp});

        // Now go over the labels in this block (there might be more than 1 at the start) and set the block index.
        // This is required because later on we want to know to what block we jump when building the successors.
        BytecodeCFGBlockIndex blockIndex = static_cast<BytecodeCFGBlockIndex>(i);
        for (usize opIndex = firstOp; opIndex < endOp; ++opIndex)
        {
            const Op& op = ops[opIndex];
            if (op.mOpCode != OpCode::cLabel)
            {
                break;
            }

            cfg.mLabelToBlock[op.as.mLabel.mIndex] = blockIndex;
        }
    }

    // Now we go over all blocks and build edges/successors.
    auto addSuccessor = [&](BytecodeCFGBlock& block, BytecodeCFGBlockIndex target, BytecodeCFGEdgeKind kind)
    {
        if (target == cInvalidBytecodeCFGBlockIndex)
        {
            return;
        }

        block.mSuccessors[block.mSuccessorCount] = BytecodeCFGEdge{target, kind};
        ++block.mSuccessorCount;
    };

    for (BytecodeCFGBlockIndex i = 0; i < cfg.mBlocks.size(); ++i)
    {
        BytecodeCFGBlock& block = cfg.mBlocks[i];

        // We're interested in the last op of every block, since that can be a terminator.
        // (It doesn't have to be one since it might simply be a fallthrough if the next block starts with a label.)
        const Op& lastOp = ops[block.mEndOp - 1];

        // Get the first op of the next block as potential fallthrough.
        BytecodeCFGBlockIndex fallthrough = (i + 1 < cfg.mBlocks.size()) ? i + 1 : cInvalidBytecodeCFGBlockIndex;

        // Now see what kind of successor we have.
        switch (lastOp.mOpCode)
        {
            case OpCode::cJump:
            {
                // If we have an unconditional jump, we know what block to jump to.
                addSuccessor(block, cfg.mLabelToBlock[lastOp.as.mJump.mAddress], BytecodeCFGEdgeKind::cJump);
                break;
            }
            case OpCode::cJumpZ:
            {
                // If we have a conditional jump, we need to consider both taken and fallthrough paths.
                addSuccessor(block,
                             cfg.mLabelToBlock[lastOp.as.mJumpZ.mAddress],
                             BytecodeCFGEdgeKind::cConditionalTaken);
                addSuccessor(block, fallthrough, BytecodeCFGEdgeKind::cConditionalFallthrough);
                break;
            }
            case OpCode::cJumpNZ:
            {
                addSuccessor(block,
                             cfg.mLabelToBlock[lastOp.as.mJumpNZ.mAddress],
                             BytecodeCFGEdgeKind::cConditionalTaken);
                addSuccessor(block, fallthrough, BytecodeCFGEdgeKind::cConditionalFallthrough);
                break;
            }
            case OpCode::cTestZ:
            {
                addSuccessor(block, cfg.mLabelToBlock[lastOp.as.mTestZ.mAddress], BytecodeCFGEdgeKind::cTestTakenKeep);
                addSuccessor(block, fallthrough, BytecodeCFGEdgeKind::cTestFallthroughPop);
                break;
            }
            case OpCode::cTestNZ:
            {
                addSuccessor(block, cfg.mLabelToBlock[lastOp.as.mTestNZ.mAddress], BytecodeCFGEdgeKind::cTestTakenKeep);
                addSuccessor(block, fallthrough, BytecodeCFGEdgeKind::cTestFallthroughPop);
                break;
            }
            case OpCode::cReturn:
            case OpCode::cHalt:
            {
                // If we have a return or halt, we don't have any successors.
                break;
            }
            default:
            {
                // Otherwise, this is a normal fallthrough.
                addSuccessor(block, fallthrough, BytecodeCFGEdgeKind::cFallthrough);
                break;
            }
        }
    }

    return cfg;
}

} // namespace simlang
