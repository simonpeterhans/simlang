#include "backend/bytecode/bytecodeemitter.h"

#include "backend/backendstate.h"
#include "backend/bytecode/bytecodeanalyzer.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/op/op.h"
#include "runtime/op/opcode.h"
#include "runtime/op/opmacros.h"
#include "runtime/op/oputils.h"
#include "source/linecolumninfo.h"
#include "source/source.h"
#include "source/sourcemanager.h"
#include "source/sourcerange.h"
#include "util/asserts.h"
#include "util/bitutils.h"
#include "util/types.h"

namespace simlang
{

BytecodeEmitter::BytecodeEmitter(CompilerContext& context)
    : mCtx(context)
{
}

template <typename T>
static void emitBytes(std::vector<u8>& bytes, const T& value)
{
    // Find out where our bytes start.
    usize offset = bytes.size();

    // Make space for the new bytes we're going to write.
    bytes.resize(offset + sizeof(T));

    // Do the write.
    bits::writeUnaligned(bytes.data() + offset, value);
}

static bool canOpReportRuntimeError(OpCode opCode)
{
    // Used to identify which ops need source ranges in the runtime source map.
    switch (opCode)
    {
        case OpCode::cSyscall:
        case OpCode::cCall:
        case OpCode::cCallMethod:
        case OpCode::cCallInterface:
        case OpCode::cFormatString:
        case OpCode::cRefObjField:
        case OpCode::cLoadObjField:
        case OpCode::cLoadObjFieldN:
        case OpCode::cStoreObjField:
        case OpCode::cStoreObjFieldN:
        case OpCode::cLoadListElement:
        case OpCode::cStoreListElement:
        case OpCode::cListSize:
        case OpCode::cListIsEmpty:
        case OpCode::cListPush:
        case OpCode::cListAddList:
        case OpCode::cListPop:
        case OpCode::cListBack:
        case OpCode::cListInsert:
        case OpCode::cListRemoveAt:
        case OpCode::cListIndexOf:
        case OpCode::cListContains:
        case OpCode::cListClear:
        case OpCode::cListReserve:
        case OpCode::cMapSize:
        case OpCode::cMapIsEmpty:
        case OpCode::cMapClear:
        case OpCode::cMapContainsKey:
        case OpCode::cMapGet:
        case OpCode::cMapSet:
        case OpCode::cMapRemove:
        case OpCode::cMapReserve:
        case OpCode::cNewObject:
        case OpCode::cNewList:
        case OpCode::cNewMap:
        case OpCode::cCheckCast:
        case OpCode::cF2I:
        case OpCode::cIDiv:
        case OpCode::cIMod:
        case OpCode::cFDiv:
        case OpCode::cFMod:
        case OpCode::cIShL:
        case OpCode::cIShR:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

RuntimeSourceLocation BytecodeEmitter::getRuntimeSourceLocation(const SourceRange& range) const
{
    if (range.isValid() == false)
    {
        return {};
    }

    ResolvedSourceLocation start = mCtx.mSources.resolveLocation(range.getStartLoc());
    if (start.isValid() == false)
    {
        return {};
    }

    LineColumnInfo startInfo = start.mSource->getLineAndColumnFromOffset(start.mLocalOffset);

    // Internally we start at 0, but user-facing errors should use 1-based line and column numbers.
    RuntimeSourceLocation location;
    location.mSourceID = start.mSourceID;
    location.mLine = startInfo.mLineIndex + 1U;
    location.mColumn = startInfo.mColumnIndex + 1U;

    return location;
}

void BytecodeEmitter::addRuntimeSourceMapEntry(SourceRange sourceRange, VMAddress startAddress, VMAddress endAddress)
{
    if (sourceRange.isValid() == false)
    {
        return;
    }

    RuntimeSourceLocation location = getRuntimeSourceLocation(sourceRange);
    mCtx.mBackend.mDebugInfo.addSourceMapEntry(startAddress, endAddress, location);
}

VMAddress BytecodeEmitter::getCurrentAddress() const
{
    return static_cast<VMAddress>(mCtx.mBackend.mBytes.size());
}

bool BytecodeEmitter::resolveLabels()
{
    // Clean up stuff for good measure.
    mBytecodeSize = 0;
    mEntryLabelAddresses.clear();
    mFunctionLabelAddresses.clear();

    // Get the code.
    const ProgramBytecode& program = mCtx.mBackend.mProgramBytecode;

    // Resolve the labels for the entry.
    if (resolveLabelsForCode(program.getEntryCode(), mEntryLabelAddresses) == false)
    {
        return false;
    }

    // Get the functions, and resolve the labels for each chunk.
    const std::vector<FunctionBytecode>& functions = program.getFunctions();
    mFunctionLabelAddresses.reserve(functions.size());

    for (const FunctionBytecode& function : functions)
    {
        mFunctionLabelAddresses.emplace_back();

        if (resolveLabelsForCode(function.getCode(), mFunctionLabelAddresses.back()) == false)
        {
            return false;
        }
    }

    return true;
}

bool BytecodeEmitter::resolveLabelsForCode(const BytecodeChunk& code, std::vector<VMAddress>& labelAddresses)
{
    // Zero all of them.
    labelAddresses.assign(code.getLabelCount(), 0);

    // Labels are local for every chunk, but their resolved addresses are absolute bytecode offsets.
    for (const Op& op : code.getOps())
    {
        if (op.mOpCode == OpCode::cLabel)
        {
            // If we have a label, get the index of the label and then register the current address.
            u32 labelIdx = op.as.mLabel.mIndex;
            labelAddresses[labelIdx] = mBytecodeSize;
        }
        else
        {
            // Otherwise, get the size of the opcode and add it to the total.
            u8 opSize = getOpCodeSize(op.mOpCode);

            // Awkward bounds check.
            u64 requiredEnd = static_cast<u64>(mBytecodeSize) + opSize;
            if (requiredEnd > cMaxValidVMAddress)
            {
                mCtx.report<cBytecodeTooLarge>(cInvalidSourceRange, requiredEnd, cMaxValidVMAddress);
                return false;
            }

            mBytecodeSize = static_cast<VMAddress>(requiredEnd);
        }
    }

    return true;
}

bool BytecodeEmitter::computeStackDepths()
{
    // Create a new analyzer.
    BytecodeAnalyzer analyzer{mCtx.mBackend};

    // Iterate over all functions to compute their stack depth.
    const std::vector<FunctionBytecode>& functions = mCtx.mBackend.mProgramBytecode.getFunctions();
    for (const FunctionBytecode& functionBytecode : functions)
    {
        FunctionInfo& function = mCtx.mBackend.mFunctionInfos[functionBytecode.getFunctionIdx()];

        // The initial stack size is the args and locals.
        u32 initialWords = function.mArgWords + function.mLocalWords;
        u32 maxStackWords = 0;

        // Let the analyzer compute the max stack words based on the initial words.
        if (analyzer.computeMaxStackWords(functionBytecode.getCode(), initialWords, maxStackWords) == false)
        {
            SIMLANG_BREAK("Failed to compute bytecode stack depth.");
            return false;
        }

        function.mMaxStackWords = maxStackWords;
    }

    return true;
}

void BytecodeEmitter::emitCode(const BytecodeChunk& code, const std::vector<VMAddress>& labelAddresses)
{
    std::vector<u8>& bytes = mCtx.mBackend.mBytes;
    const std::vector<Op>& ops = code.getOps();
    const std::vector<SourceRange>& sourceRanges = code.getSourceRanges();

    // Emit all the ops in the chunk.
    for (usize i = 0; i < ops.size(); ++i)
    {
        const Op& op = ops[i];

        // If this is a label, skip it.
        if (op.mOpCode == OpCode::cLabel)
        {
            continue;
        }

        // Track where the op starts.
        VMAddress startAddress = getCurrentAddress();

        // Emit the opcode.
        emitBytes(bytes, op.mOpCode);

        // If we have a jump, get the label index and look up the address we have to jump to.
        VMAddress labelIndex = 0;
        if (getJumpTarget(op, labelIndex))
        {
            VMAddress labelAddress = labelAddresses[labelIndex];
            // We then directly emit the jump address and are already done.
            // Note that this would need patching if we have a jump op with more than 1 arg.
            emitBytes(bytes, labelAddress);
        }
        else
        {
            // Otherwise, do the full emission for the op args.
            switch (op.mOpCode)
            {
#define X(NAME, COUNT, T1, N1, T2, N2) \
    case OpCode::c##NAME: \
    { \
        SIMLANG_OP_IFGE1(COUNT, emitBytes(bytes, op.as.m##NAME.N1)); \
        SIMLANG_OP_IF2(COUNT, emitBytes(bytes, op.as.m##NAME.N2)); \
        break; \
    }

#include "runtime/op/opcodes.def"

#undef X
            }
        }

        // If this is an op that could potentially cause a RTE, add it to the source map.
        // If you ever want to hook up a proper debugger and need more information, this would be a good place for it.
        if (canOpReportRuntimeError(op.mOpCode))
        {
            VMAddress endAddress = getCurrentAddress();
            addRuntimeSourceMapEntry(sourceRanges[i], startAddress, endAddress);
        }
    }
}

void BytecodeEmitter::emitFunction(const FunctionBytecode& function, const std::vector<VMAddress>& labelAddresses)
{
    FunctionInfo& fi = mCtx.mBackend.mFunctionInfos[function.getFunctionIdx()];

    // Don't forget to set the entry address for the function.
    fi.mEntryAddress = getCurrentAddress();

    emitCode(function.getCode(), labelAddresses);
}

bool BytecodeEmitter::emit()
{
    // Resolve the labels for every function.
    if (resolveLabels() == false)
    {
        return false;
    }

    // Compute the stack depths for every function.
    if (computeStackDepths() == false)
    {
        return false;
    }

    mCtx.mBackend.mBytes.clear();
    mCtx.mBackend.mBytes.reserve(mBytecodeSize);

    mCtx.mBackend.mDebugInfo.clear();

    for (usize i = 0; i < mCtx.mSources.getSourceCount(); ++i)
    {
        RuntimeSourceID runtimeSourceID = static_cast<RuntimeSourceID>(i);
        SourceID sourceID = static_cast<SourceID>(i);
        const Source* source = mCtx.mSources.getSource(sourceID);

        // Set the source files so we have them for the debug info.
        mCtx.mBackend.mDebugInfo.setSourceFile(runtimeSourceID, source->getFilename());
    }

    // Emit the entry.
    emitCode(mCtx.mBackend.mProgramBytecode.getEntryCode(), mEntryLabelAddresses);

    // Emit the functions.
    const std::vector<FunctionBytecode>& functions = mCtx.mBackend.mProgramBytecode.getFunctions();
    for (usize i = 0; i < functions.size(); ++i)
    {
        emitFunction(functions[i], mFunctionLabelAddresses[i]);
    }

    return true;
}

} // namespace simlang
