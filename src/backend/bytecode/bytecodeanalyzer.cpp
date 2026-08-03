#include "backend/bytecode/bytecodeanalyzer.h"

#include <limits>
#include <vector>

#include "backend/backendstate.h"
#include "backend/bytecode/bytecodecfg.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "backend/stringdata.h"
#include "runtime/callinfo.h"
#include "runtime/op/op.h"
#include "runtime/op/opcode.h"
#include "runtime/stringdata.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/vmdefines.h"

namespace simlang
{

BytecodeAnalyzer::BytecodeAnalyzer(const BackendState& backend)
    : mBackend(backend)
{
}

static bool popStackWords(u32& height, u32 wordCount)
{
    if (height < wordCount)
    {
        return false;
    }

    height -= wordCount;
    return true;
}

static bool pushStackWords(u32& height, u32 wordCount)
{
    u64 next = static_cast<u64>(height) + wordCount;
    if (next > cMaxVMStackWords)
    {
        return false;
    }

    height = static_cast<u32>(next);
    return true;
}

static bool replaceStackWords(u32& height, u32 poppedWords, u32 pushedWords)
{
    return popStackWords(height, poppedWords) && pushStackWords(height, pushedWords);
}

static bool getSuccessorHeight(u32 height, BytecodeCFGEdgeKind kind, u32& outHeight)
{
    outHeight = height;

    switch (kind)
    {
        case BytecodeCFGEdgeKind::cFallthrough:
        case BytecodeCFGEdgeKind::cJump:
        {
            return true;
        }

        case BytecodeCFGEdgeKind::cConditionalTaken:
        case BytecodeCFGEdgeKind::cConditionalFallthrough:
        case BytecodeCFGEdgeKind::cTestFallthroughPop:
        {
            return popStackWords(outHeight, 1);
        }

        case BytecodeCFGEdgeKind::cTestTakenKeep:
        {
            return true;
        }
    }

    return false;
}

bool BytecodeAnalyzer::computeMaxStackWords(const BytecodeChunk& code,
                                            u32 initialStackWords,
                                            u32& outMaxStackWords) const
{
    static constexpr u32 cUnknownStackHeight = std::numeric_limits<u32>::max();

    // Build the CFG.
    BytecodeCFG cfg = BytecodeCFG::fromCode(code);

    u32 maxHeight = initialStackWords;

    // If we have no blocks (?), then we're already done.
    if (cfg.mBlocks.empty())
    {
        outMaxStackWords = maxHeight;
        return true;
    }

    // Now we're interested in navigating from the root block to all successors and tracking the stack height.
    std::vector<BytecodeCFGBlockIndex> blockStack;
    blockStack.reserve(cfg.mBlocks.size());

    // Record the height of every block at entry.
    std::vector<u32> blockEntryHeights(cfg.mBlocks.size(), cUnknownStackHeight);

    auto propagate = [&](BytecodeCFGBlockIndex blockIndex, u32 height) -> bool
    {
        u32& storedHeight = blockEntryHeights[blockIndex];

        if (storedHeight == cUnknownStackHeight)
        {
            storedHeight = height;
            blockStack.push_back(blockIndex);
            return true;
        }

        return storedHeight == height;
    };

    // Get our ops.
    const std::vector<Op>& ops = code.getOps();

    // Start with the first block.
    propagate(0, maxHeight);

    while (blockStack.empty() == false)
    {
        // We've reached a new block.
        BytecodeCFGBlockIndex blockIdx = blockStack.back();
        blockStack.pop_back();

        const BytecodeCFGBlock& block = cfg.mBlocks[blockIdx];
        // Get the current stack height at the start of the block to start from.
        u32 height = blockEntryHeights[blockIdx];

        // Before we get to the (final) op that gets us to the successors, do a normal linear scan.
        // Unfortunately we have to check again if the final op is a terminator, and reduce our range in that case.
        usize endIdx = block.mEndOp;
        if (isCFGBlockTerminatorOp(ops[endIdx - 1].mOpCode))
        {
            --endIdx;
        }

        for (usize i = block.mFirstOp; i < endIdx; ++i)
        {
            if (applyStackEffect(ops[i], height) == false)
            {
                return false;
            }

            maxHeight = (maxHeight < height) ? height : maxHeight;
        }

        // Deal with the successors.
        for (u8 i = 0; i < block.mSuccessorCount; ++i)
        {
            BytecodeCFGEdge edge = block.mSuccessors[i];

            u32 successorHeight = 0;
            if (getSuccessorHeight(height, edge.mKind, successorHeight) == false)
            {
                return false;
            }

            if (propagate(edge.mTarget, successorHeight) == false)
            {
                return false;
            }
        }
    }

    outMaxStackWords = maxHeight;

    return true;
}

bool BytecodeAnalyzer::applyStackEffect(const Op& op, u32& height) const
{
    // Simulates an op's effect on the stack.

    switch (op.mOpCode)
    {
        case OpCode::cLabel:
        case OpCode::cHalt:
        case OpCode::cJump:
        case OpCode::cReturn:
        case OpCode::cRefField:
        case OpCode::cRefObjField:
        case OpCode::cLoadRef:
        case OpCode::cLoadRefField:
        case OpCode::cLoadObjField:
        case OpCode::cListSize:
        case OpCode::cListIsEmpty:
        case OpCode::cMapSize:
        case OpCode::cMapIsEmpty:
        case OpCode::cIncLocal:
        case OpCode::cI2F:
        case OpCode::cF2I:
        case OpCode::cCheckCast:
        case OpCode::cINeg:
        case OpCode::cFNeg:
        case OpCode::cINot:
        {
            return true;
        }

        case OpCode::cSyscall:
        {
            const SyscallEntry& entry = mBackend.mSyscallInfos[op.as.mSyscall.mIndex];
            return replaceStackWords(height, entry.mArgWords, entry.mReturnWords);
        }
        case OpCode::cCall:
        case OpCode::cCallMethod:
        {
            FunctionIdx index = (op.mOpCode == OpCode::cCall) ? op.as.mCall.mIndex : op.as.mCallMethod.mIndex;
            const FunctionInfo& function = mBackend.mFunctionInfos[index];
            return replaceStackWords(height, function.mArgWords, function.mReturnWords);
        }
        case OpCode::cCallInterface:
        {
            const InterfaceCallInfo& info = mBackend.mInterfaceCallInfos[op.as.mCallInterface.mIndex];
            return replaceStackWords(height, static_cast<u32>(info.mArgWords) + 2U, info.mReturnWords);
        }

        case OpCode::cJumpZ:
        case OpCode::cJumpNZ:
        case OpCode::cPop:
        case OpCode::cPrint:
        {
            return popStackWords(height, 1);
        }

        case OpCode::cTestZ:
        case OpCode::cTestNZ:
        {
            // Assume we pass and leave the value on the stack.
            return true;
        }

        case OpCode::cPopN:
        {
            return popStackWords(height, op.as.mPopN.mSize);
        }

        case OpCode::cDup:
        case OpCode::cPush8:
        case OpCode::cPush8S:
        case OpCode::cPush16:
        case OpCode::cPush16S:
        case OpCode::cPush32:
        case OpCode::cPushString:
        case OpCode::cRefLocal:
        case OpCode::cRefGlobal:
        case OpCode::cLoadLocal:
        case OpCode::cLoadGlobal:
        case OpCode::cNewObject:
        case OpCode::cNewList:
        case OpCode::cNewMap:
        {
            return pushStackWords(height, 1);
        }

        case OpCode::cDupN:
        {
            return pushStackWords(height, op.as.mDupN.mSize);
        }

        case OpCode::cFormatString:
        {
            const StringFormatTemplate& tmpl = mBackend.mStringFormats.geTemplates()[op.as.mFormatString.mIndex];
            return replaceStackWords(height, static_cast<u32>(tmpl.mArgKinds.size()), 1);
        }

        case OpCode::cLoadLocalN:
        {
            return pushStackWords(height, op.as.mLoadLocalN.mSize);
        }
        case OpCode::cLoadGlobalN:
        {
            return pushStackWords(height, op.as.mLoadGlobalN.mSize);
        }
        case OpCode::cLoadRefN:
        {
            return replaceStackWords(height, 1, op.as.mLoadRefN.mSize);
        }
        case OpCode::cLoadRefFieldN:
        {
            return replaceStackWords(height, 1, op.as.mLoadRefFieldN.mSize);
        }
        case OpCode::cLoadObjFieldN:
        {
            return replaceStackWords(height, 1, op.as.mLoadObjFieldN.mSize);
        }
        case OpCode::cLoadListElement:
        {
            return replaceStackWords(height, 2, op.as.mLoadListElement.mSize);
        }
        case OpCode::cListPop:
        {
            return replaceStackWords(height, 1, op.as.mListPop.mSize);
        }
        case OpCode::cListBack:
        {
            return replaceStackWords(height, 1, op.as.mListBack.mSize);
        }
        case OpCode::cListRemoveAt:
        {
            return replaceStackWords(height, 2, op.as.mListRemoveAt.mSize);
        }
        case OpCode::cListIndexOf:
        {
            return replaceStackWords(height, static_cast<u32>(op.as.mListIndexOf.mSize) + 1U, 1);
        }
        case OpCode::cListContains:
        {
            return replaceStackWords(height, static_cast<u32>(op.as.mListContains.mSize) + 1U, 1);
        }
        case OpCode::cMapContainsKey:
        case OpCode::cMapRemove:
        {
            return replaceStackWords(height, 2, 1);
        }
        case OpCode::cMapGet:
        {
            return replaceStackWords(height, 2, op.as.mMapGet.mSize);
        }

        case OpCode::cStoreLocal:
        case OpCode::cStoreGlobal:
        {
            return popStackWords(height, 1);
        }
        case OpCode::cStoreLocalN:
        {
            return popStackWords(height, op.as.mStoreLocalN.mSize);
        }
        case OpCode::cStoreGlobalN:
        {
            return popStackWords(height, op.as.mStoreGlobalN.mSize);
        }

        case OpCode::cStoreRef:
        case OpCode::cStoreRefField:
        case OpCode::cStoreObjField:
        {
            return popStackWords(height, 2);
        }
        case OpCode::cStoreRefN:
        {
            return popStackWords(height, static_cast<u32>(op.as.mStoreRefN.mSize) + 1U);
        }
        case OpCode::cStoreRefFieldN:
        {
            return popStackWords(height, static_cast<u32>(op.as.mStoreRefFieldN.mSize) + 1U);
        }
        case OpCode::cStoreObjFieldN:
        {
            return popStackWords(height, static_cast<u32>(op.as.mStoreObjFieldN.mSize) + 1U);
        }
        case OpCode::cStoreListElement:
        {
            return popStackWords(height, static_cast<u32>(op.as.mStoreListElement.mSize) + 2U);
        }
        case OpCode::cListClear:
        case OpCode::cMapClear:
        {
            return popStackWords(height, 1);
        }
        case OpCode::cListReserve:
        case OpCode::cListAddList:
        case OpCode::cMapReserve:
        {
            return popStackWords(height, 2);
        }
        case OpCode::cListPush:
        {
            return popStackWords(height, static_cast<u32>(op.as.mListPush.mSize) + 1U);
        }
        case OpCode::cListInsert:
        {
            return popStackWords(height, static_cast<u32>(op.as.mListInsert.mSize) + 2U);
        }
        case OpCode::cMapSet:
        {
            return popStackWords(height, static_cast<u32>(op.as.mMapSet.mSize) + 2U);
        }

        case OpCode::cSEQ:
        case OpCode::cSNE:
        case OpCode::cIEQ:
        case OpCode::cINE:
        case OpCode::cILT:
        case OpCode::cILE:
        case OpCode::cIGT:
        case OpCode::cIGE:
        case OpCode::cFEQ:
        case OpCode::cFNE:
        case OpCode::cFLT:
        case OpCode::cFLE:
        case OpCode::cFGT:
        case OpCode::cFGE:
        case OpCode::cIAdd:
        case OpCode::cISub:
        case OpCode::cIMul:
        case OpCode::cIDiv:
        case OpCode::cIMod:
        case OpCode::cFAdd:
        case OpCode::cFSub:
        case OpCode::cFMul:
        case OpCode::cFDiv:
        case OpCode::cFMod:
        case OpCode::cIAnd:
        case OpCode::cIOr:
        case OpCode::cIXor:
        case OpCode::cIShL:
        case OpCode::cIShR:
        {
            return popStackWords(height, 1);
        }
    }

    return false;
}

} // namespace simlang
