#include "backend/bytecode/optimizer.h"

#include <algorithm>
#include <utility>

#include "backend/backendstate.h"
#include "backend/bytecode/bytecodecfg.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "runtime/op/op.h"
#include "runtime/op/opcode.h"
#include "runtime/op/oputils.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"

namespace simlang
{

static Op makeDupOp(OpWordCount wordCount)
{
    if (wordCount == 1)
    {
        return makeOp<OpCode::cDup>();
    }

    return makeOp<OpCode::cDupN>(wordCount);
}

static bool makeKeepStoredValueReplacement(Op storeOp, Op loadOp, Op& dupOp)
{
    switch (storeOp.mOpCode)
    {
        case OpCode::cStoreLocal:
        {
            if (loadOp.mOpCode != OpCode::cLoadLocal ||
                loadOp.as.mLoadLocal.mLocalIdx != storeOp.as.mStoreLocal.mLocalIdx)
            {
                return false;
            }

            dupOp = makeOp<OpCode::cDup>();

            return true;
        }
        case OpCode::cStoreLocalN:
        {
            if (loadOp.mOpCode != OpCode::cLoadLocalN ||
                loadOp.as.mLoadLocalN.mLocalIdx != storeOp.as.mStoreLocalN.mLocalIdx ||
                loadOp.as.mLoadLocalN.mSize != storeOp.as.mStoreLocalN.mSize)
            {
                return false;
            }

            dupOp = makeDupOp(storeOp.as.mStoreLocalN.mSize);

            return true;
        }
        case OpCode::cStoreGlobal:
        {
            if (loadOp.mOpCode != OpCode::cLoadGlobal ||
                loadOp.as.mLoadGlobal.mGlobalIdx != storeOp.as.mStoreGlobal.mGlobalIdx)
            {
                return false;
            }

            dupOp = makeOp<OpCode::cDup>();

            return true;
        }
        case OpCode::cStoreGlobalN:
        {
            if (loadOp.mOpCode != OpCode::cLoadGlobalN ||
                loadOp.as.mLoadGlobalN.mGlobalIdx != storeOp.as.mStoreGlobalN.mGlobalIdx ||
                loadOp.as.mLoadGlobalN.mSize != storeOp.as.mStoreGlobalN.mSize)
            {
                return false;
            }

            dupOp = makeDupOp(storeOp.as.mStoreGlobalN.mSize);

            return true;
        }
        default:
        {
            return false;
        }
    }
}

Optimizer::Optimizer(BackendState& ctx)
    : mCtx(ctx)
{
}

isize Optimizer::previousLive(usize idx) const
{
    if (idx == 0)
    {
        return -1;
    }

    // The previous "live" op is one that has mKill[i] set to 0.
    isize i = static_cast<isize>(idx - 1);
    while (i >= 0 && mOpMarked[static_cast<usize>(i)])
    {
        --i;
    }

    return i;
}

usize Optimizer::nextLive(usize idx) const
{
    const std::vector<Op>& ops = *mOps;

    // The next "live" op is one that has mKill[i] set to 0.
    usize i = idx + 1;
    while (i < ops.size() && mOpMarked[i])
    {
        ++i;
    }
    return i;
}

bool Optimizer::nextLiveOp(usize from, usize& outIndex, Op& outOp) const
{
    const std::vector<Op>& ops = *mOps;

    outIndex = nextLive(from);
    if (outIndex >= ops.size())
    {
        return false;
    }

    outOp = ops[outIndex];

    return true;
}

void Optimizer::recordRemovedOp(Op op)
{
    u32 bytes = getOpCodeSize(op.mOpCode);
    ++mStats.mRemovedOps;
    mStats.mBytesSaved += static_cast<i32>(bytes);
}

void Optimizer::recordRewrittenOp(Op before, Op after)
{
    i32 byteDelta = static_cast<i32>(getOpCodeSize(before.mOpCode)) - static_cast<i32>(getOpCodeSize(after.mOpCode));
    ++mStats.mRewrittenOps;
    mStats.mBytesSaved += byteDelta;
}

void Optimizer::prepare()
{
    const std::vector<Op>& ops = *mOps;

    // Assume nothing gets deleted.
    mOpMarked.assign(ops.size(), 0);

    // Prepare the vectors.
    usize numLabels = mLabelCount;

    mLabelToOpIndex.resize(numLabels + ops.size());
    std::fill(mLabelToOpIndex.begin(), mLabelToOpIndex.end(), -1);

    mLabelUsed.resize(numLabels + ops.size());
    std::fill(mLabelUsed.begin(), mLabelUsed.end(), static_cast<u8>(0));

    // Rebuild the map from label ID to op index.
    for (usize i = 0; i < ops.size(); ++i)
    {
        // If we're destroying the op, ignore it.
        if (mOpMarked.empty() == false && mOpMarked[i])
        {
            continue;
        }

        // Get the op if it's a label.
        Op op = ops[i];
        if (op.mOpCode != OpCode::cLabel)
        {
            continue;
        }

        // Get the label's index to jump to and register it for the label.
        VMAddress labelIdx = op.as.mLabel.mIndex;
        if (labelIdx < mLabelToOpIndex.size())
        {
            mLabelToOpIndex[labelIdx] = static_cast<isize>(i);
        }
    }
}

void Optimizer::compact()
{
    std::vector<Op>& ops = *mOps;

    usize write = 0;
    for (usize read = 0; read < ops.size(); ++read)
    {
        // If we're removing this op, we skip it.
        if (mOpMarked[read])
        {
            recordRemovedOp(ops[read]);
            continue;
        }

        // As soon as we removed (skipped) at least one op, we're reading ahead.
        // In that case, we (over)write whatever we read into the next non-removed slot.
        if (write != read)
        {
            ops[write] = ops[read];
            (*mSourceRanges)[write] = (*mSourceRanges)[read];
        }
        ++write;
    }

    // Finally, shrink the op array to the new size.
    if (write != ops.size())
    {
        ops.resize(write);
        mSourceRanges->resize(write);
    }
}

bool Optimizer::removeUnreachableBlocks()
{
    // Use the bytecode CFG to determine which blocks in our function are reachable.
    BytecodeCFG cfg = BytecodeCFG::fromCode(*mChunk);
    if (cfg.mBlocks.empty())
    {
        return false;
    }

    // Set them all as unreachable.
    std::vector<u8> reachable(cfg.mBlocks.size(), 0);
    std::vector<BytecodeCFGBlockIndex> blockStack;
    blockStack.reserve(cfg.mBlocks.size());

    // The first block is always reachable.
    reachable[0] = 1;
    // Start with that.
    blockStack.push_back(0);

    while (blockStack.empty() == false)
    {
        BytecodeCFGBlockIndex blockIndex = blockStack.back();
        blockStack.pop_back();

        // Go over all the block's successors.
        const BytecodeCFGBlock& block = cfg.mBlocks[blockIndex];
        for (u8 i = 0; i < block.mSuccessorCount; ++i)
        {
            const BytecodeCFGEdge& edge = block.mSuccessors[i];
            // If this successor is already reachable, we don't need to process it again.
            if (reachable[edge.mTarget])
            {
                continue;
            }

            // Otherwise, set as reachable and add to the stack.
            reachable[edge.mTarget] = 1;
            blockStack.push_back(edge.mTarget);
        }
    }

    // Now check which of them ended up reachable.
    bool anyChanges = false;
    for (usize i = 0; i < cfg.mBlocks.size(); ++i)
    {
        if (reachable[i])
        {
            continue;
        }

        // If not reachable, mark all ops as unreachable.
        const BytecodeCFGBlock& block = cfg.mBlocks[i];
        for (usize j = block.mFirstOp; j < block.mEndOp; ++j)
        {
            mOpMarked[j] = 1;
            anyChanges = true;
        }
    }

    return anyChanges;
}

bool Optimizer::mergeConsecutiveLabels()
{
    // Label L1
    // Label L2
    // -> redirect all jumps from L2 to L1.
    // (This will make L2 unused and thus removed in the later pass.)
    std::vector<Op>& ops = *mOps;

    bool anyChanges = false;
    for (usize i = 0; i < ops.size(); ++i)
    {
        if (mOpMarked[i])
        {
            continue;
        }

        // We take each jump and check whether it jumps to a label immediately preceded by a label.
        // If so, we redirect the jump to the first of those labels.
        Op& op = ops[i];
        Op oldOp = op;
        if (isJumpOp(op.mOpCode) == false)
        {
            continue;
        }

        // Get the label this jump targets.
        VMAddress labelIdx = 0;
        if (getJumpTarget(op, labelIdx) == false || labelIdx >= mLabelToOpIndex.size())
        {
            continue;
        }

        // Get the op index of the label.
        isize labelOpIdx = mLabelToOpIndex[labelIdx];
        if (labelOpIdx < 0)
        {
            continue;
        }

        // Get the index of the op before the one we're jumping to (which is a label).
        VMAddress earliestLabel = labelIdx;
        isize previous = previousLive(static_cast<usize>(labelOpIdx));

        // While we have any previous live ops, check if they are labels.
        while (previous >= 0)
        {
            // If it's not a label, bail.
            Op previousOp = ops[static_cast<usize>(previous)];
            if (previousOp.mOpCode != OpCode::cLabel)
            {
                break;
            }

            // Otherwise, update the earliest label and continue.
            earliestLabel = previousOp.as.mLabel.mIndex;
            previous = previousLive(static_cast<usize>(previous));
        }

        // If we only had that one label, we don't need to change anything.
        if (earliestLabel == labelIdx)
        {
            continue;
        }

        // Otherwise, update the jump target and indicate the change.
        setJumpTarget(op, earliestLabel);

        recordRewrittenOp(oldOp, op);
        anyChanges = true;
    }

    return anyChanges;
}

bool Optimizer::invertConditionalSkipToNext()
{
    std::vector<Op>& ops = *mOps;

    // JumpZ  Lx
    // Jump   Ly
    // Label  Lx
    // ->
    // JumpNZ Ly

    // OR:

    // JumpNZ Lx
    // Jump   Ly
    // Label  Lx
    // ->
    // JumpZ Ly

    bool anyChanges = false;

    for (usize i = 0; i < ops.size(); ++i)
    {
        if (mOpMarked[i])
        {
            continue;
        }

        Op op1 = ops[i];
        if (op1.mOpCode != OpCode::cJumpZ && op1.mOpCode != OpCode::cJumpNZ)
        {
            continue;
        }

        // Find the second op, it must be alive.
        usize j;
        Op op2;
        if (nextLiveOp(i, j, op2) == false)
        {
            // If the index is oob, we skip.
            continue;
        }

        // Same for the third one.
        usize k;
        Op op3;
        if (nextLiveOp(j, k, op3) == false)
        {
            continue;
        }

        // The second one must be a jump.
        if (op2.mOpCode != OpCode::cJump)
        {
            continue;
        }

        // The third one must be a label.
        if (op3.mOpCode != OpCode::cLabel)
        {
            continue;
        }

        VMAddress targetLabel1 = 0;
        VMAddress targetLabel2 = 0;
        if (getJumpTarget(op1, targetLabel1) == false || getJumpTarget(op2, targetLabel2) == false)
        {
            continue;
        }
        VMAddress labelIdx3 = op3.as.mLabel.mIndex;

        if (targetLabel1 != labelIdx3)
        {
            continue;
        }

        // Invert JumpZ/NZ.
        if (op1.mOpCode == OpCode::cJumpZ)
        {
            ops[i] = makeOp<OpCode::cJumpNZ>(targetLabel2);
        }
        else
        {
            ops[i] = makeOp<OpCode::cJumpZ>(targetLabel2);
        }

        recordRewrittenOp(op1, ops[i]);

        // We erase the unconditional jump.
        mOpMarked[j] = 1;

        // The label remains untouched, but we had changes.
        anyChanges = true;
    }

    return anyChanges;
}

bool Optimizer::removeJumpToNext()
{
    // Jump L
    // Label L
    // ->
    // Label L

    std::vector<Op>& ops = *mOps;

    bool anyChanges = false;

    for (usize i = 0; i < ops.size(); ++i)
    {
        if (mOpMarked[i])
        {
            continue;
        }

        // We want a Jump.
        Op op = ops[i];
        if (op.mOpCode != OpCode::cJump)
        {
            continue;
        }

        // Get the label we're jumping to.
        VMAddress targetLabel = op.as.mJump.mAddress;
        if (targetLabel >= mLabelToOpIndex.size())
        {
            continue;
        }

        // If the next live op is that label, we can remove the jump.
        // First, get the index of the op of the target label.
        isize targetOpIdx = mLabelToOpIndex[targetLabel];
        if (targetOpIdx < 0)
        {
            continue;
        }

        // Then, find the next alive op and see if it's the label we expect.
        usize next = nextLive(i);

        if (next < ops.size() && static_cast<usize>(targetOpIdx) == next)
        {
            mOpMarked[i] = 1;
            anyChanges = true;
        }
    }

    return anyChanges;
}

bool Optimizer::dropUnusedLabels()
{
    std::vector<Op>& ops = *mOps;

    // First, compute all labels that are used by any jump op.
    for (usize i = 0; i < ops.size(); ++i)
    {
        // If we're destroying the op, ignore it.
        if (mOpMarked.empty() == false && mOpMarked[i])
        {
            continue;
        }

        // Get the op if it's a jump.
        Op op = ops[i];
        if (isJumpOp(op.mOpCode) == false)
        {
            continue;
        }

        // Set the target address (label) as in use.
        VMAddress target = 0;
        if (getJumpTarget(op, target) && target < mLabelUsed.size())
        {
            mLabelUsed[target] = 1;
        }
    }

    // Then, go over all the labels and delete the ones that are not used.
    bool anyChanges = false;

    for (usize i = 0; i < ops.size(); ++i)
    {
        // No redundancy.
        if (mOpMarked[i])
        {
            continue;
        }

        // We want a label.
        Op op = ops[i];
        if (op.mOpCode != OpCode::cLabel)
        {
            continue;
        }

        // Get the index of the label.
        VMAddress labelIdx = op.as.mLabel.mIndex;

        // Find out if any jump is targeting this label by looking it up.
        bool used = (labelIdx < mLabelUsed.size()) && (mLabelUsed[labelIdx] != 0);

        if (used == false)
        {
            mOpMarked[i] = 1;
            anyChanges = true;
        }
    }

    return anyChanges;
}

bool Optimizer::compactLocalInc()
{
    std::vector<Op>& ops = *mOps;

    // LoadLocal N
    // Push8 (mInt8 == 1)
    // IAdd
    // StoreLocal N
    // ->
    // IncLocal

    bool anyChanges = false;

    for (usize i = 0; i < ops.size(); ++i)
    {
        if (mOpMarked[i])
        {
            continue;
        }

        Op op1 = ops[i];
        if (op1.mOpCode != OpCode::cLoadLocal)
        {
            continue;
        }

        LocalIdx localIdx = op1.as.mLoadLocal.mLocalIdx;

        // Find the second op, it must be alive.
        usize j;
        Op op2;
        if (nextLiveOp(i, j, op2) == false)
        {
            // If the index is oob, we skip.
            continue;
        }

        // Same for the third one.
        usize k;
        Op op3;
        if (nextLiveOp(j, k, op3) == false)
        {
            continue;
        }

        // And the fourth one.
        usize l;
        Op op4;
        if (nextLiveOp(k, l, op4) == false)
        {
            continue;
        }

        // The second one must be a Push8 with an inc of 1.
        if (op2.mOpCode != OpCode::cPush8 || op2.as.mPush8.mInt8 != 1)
        {
            continue;
        }

        // The third one must be an add.
        if (op3.mOpCode != OpCode::cIAdd)
        {
            continue;
        }

        // The fourth one must be the store back.
        if (op4.mOpCode != OpCode::cStoreLocal || op4.as.mStoreLocal.mLocalIdx != localIdx)
        {
            continue;
        }

        // Add a single op and replace the rest.
        ops[i] = makeOp<OpCode::cIncLocal>(localIdx);

        recordRewrittenOp(op1, ops[i]);

        // We erase the rest.
        mOpMarked[j] = 1;
        mOpMarked[k] = 1;
        mOpMarked[l] = 1;

        // The label remains untouched, but we had changes.
        anyChanges = true;
    }

    return anyChanges;
}

bool Optimizer::keepStoredValue()
{
    std::vector<Op>& ops = *mOps;

    // Store X
    // Load X
    // ->
    // Dup/DupN
    // Store X

    bool anyChanges = false;

    for (usize i = 0; i < ops.size(); ++i)
    {
        // If the op is already dead, skip.
        if (mOpMarked[i])
        {
            continue;
        }

        Op op1 = ops[i];
        if (op1.mOpCode != OpCode::cStoreLocal && op1.mOpCode != OpCode::cStoreGlobal &&
            op1.mOpCode != OpCode::cStoreLocalN && op1.mOpCode != OpCode::cStoreGlobalN)
        {
            continue;
        }

        // If we have a store, we want to check if it is followed by a load that targets the same.
        usize j;
        Op op2;
        if (nextLiveOp(i, j, op2) == false)
        {
            continue;
        }

        Op storeOp = op1;
        Op loadOp = op2;
        Op dupOp;
        if (makeKeepStoredValueReplacement(storeOp, loadOp, dupOp) == false)
        {
            continue;
        }

        // If we're here, we have a store followed by a load with identical params.
        // Replace the store with a dup and use the store in place of the load (which is no longer needed).
        ops[i] = dupOp;
        ops[j] = storeOp;

        std::swap((*mSourceRanges)[i], (*mSourceRanges)[j]);

        recordRewrittenOp(storeOp, dupOp);
        recordRewrittenOp(loadOp, storeOp);

        anyChanges = true;
    }

    return anyChanges;
}

bool Optimizer::iterate()
{
    if (mOps->empty())
    {
        return false;
    }

    prepare();

    bool changed = false;

    changed |= mergeConsecutiveLabels();
    changed |= removeUnreachableBlocks();
    changed |= removeJumpToNext();
    changed |= invertConditionalSkipToNext();
    changed |= dropUnusedLabels();
    changed |= compactLocalInc();
    changed |= keepStoredValue();

    if (changed)
    {
        compact();
    }

    return changed;
}

void Optimizer::optimizeCode(BytecodeChunk& code)
{
    mChunk = &code;
    mOps = &code.getOps();
    mSourceRanges = &code.getSourceRanges();
    mLabelCount = code.getLabelCount();

    bool anyChanges = false;

    do
    {
        anyChanges = iterate();
    } while (anyChanges);
}

OptimizerStats Optimizer::optimize()
{
    mStats = {};

    optimizeCode(mCtx.mProgramBytecode.getEntryCode());

    for (FunctionBytecode& function : mCtx.mProgramBytecode.getFunctions())
    {
        optimizeCode(function.getCode());
    }

    return mStats;
}

} // namespace simlang
