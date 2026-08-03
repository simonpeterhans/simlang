#include "runtime/vm/vm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "runtime/callinfo.h"
#include "runtime/executableimage.h"
#include "runtime/memory/typelayout.h"
#include "runtime/memory/typelayouttable.h"
#include "runtime/runtimedebuginfo.h"
#include "runtime/runtimeerrorsink.h"
#include "runtime/stringdata.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/typeids.h"
#include "util/arrayview.h"
#include "util/asserts.h"
#include "util/hash.h"
#include "util/meta.h"
#include "util/numeric.h"
#include "util/scoping.h"
#include "util/textwriter.h"

namespace simlang
{

static bool isHeapAddress(VMWord a)
{
    return (a & cHeapKindMask) == cHeapAddressTag;
}

static HeapIndex getHeapPayload(VMWord a)
{
    return a & cHeapPayloadMask;
}

static VMWord makeHeapAddress(HeapIndex heapIdx)
{
    return cHeapAddressTag | heapIdx;
}

static bool isGlobalRefAddress(VMWord a)
{
    return (a & cRefAddressKindMask) == cGlobalRefAddressTag;
}

static bool isStackRefAddress(VMWord a)
{
    return (a & cRefAddressKindMask) == cStackRefAddressTag;
}

static u32 getRefAddressIndex(VMWord a)
{
    return a & cRefAddressPayloadMask;
}

static VMWord makeGlobalRefAddress(u32 globalIdx)
{
    return cGlobalRefAddressTag | globalIdx;
}

static VMWord makeStackRefAddress(u32 stackIdx)
{
    return cStackRefAddressTag | stackIdx;
}

static bool isStaticStringHandle(VMWord a)
{
    return (a & cStaticStringTagMask) == cStaticStringTag;
}

static StringLiteralIdx getStaticStringLiteralIndex(VMWord a)
{
    return a & cStaticStringPayloadMask;
}

static VMWord makeStaticStringHandle(StringLiteralIdx index)
{
    return cStaticStringTag | index;
}

static void copyWords(VMWord* dst, const VMWord* src, u32 wordCount)
{
    if (dst == src || wordCount == 0)
    {
        return;
    }

    // Some (possibly) optimized paths for smaller counts before the usual memcpy.
    switch (wordCount)
    {
        case 1:
        {
            dst[0] = src[0];
            return;
        }
        case 2:
        {
            dst[0] = src[0];
            dst[1] = src[1];
            return;
        }
        case 3:
        {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            return;
        }
        case 4:
        {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
            return;
        }
        default:
        {
            std::memcpy(dst, src, wordCount * sizeof(VMWord));
            return;
        }
    }
}

static bool cmpWords(const VMWord* lhs, const VMWord* rhs, u32 wordCount)
{
    if (lhs == rhs || wordCount == 0)
    {
        return true;
    }

    switch (wordCount)
    {
        case 1: return lhs[0] == rhs[0];
        case 2: return lhs[0] == rhs[0] && lhs[1] == rhs[1];
        case 3: return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2];
        case 4: return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2] && lhs[3] == rhs[3];
        default: return std::memcmp(lhs, rhs, wordCount * sizeof(VMWord)) == 0;
    }
}

static i32 findListElement(const ListBlock& block, const VMWord* value)
{
    u32 elementWords = block.getElementWords();
    for (u32 i = 0; i < block.mHeader->mLength; ++i)
    {
        if (cmpWords(block.getElement(i), value, elementWords))
        {
            return static_cast<i32>(i);
        }
    }

    return -1;
}

static VMWord offsetRefAddress(VMWord base, u32 wordOffset)
{
    if (isHeapAddress(base))
    {
        u64 heapIndex = static_cast<u64>(getHeapPayload(base)) + wordOffset;
        if (heapIndex > cMaxHeapPayloadIndex)
        {
            return cInvalidAddress;
        }

        return makeHeapAddress(static_cast<HeapIndex>(heapIndex));
    }

    if (isGlobalRefAddress(base) || isStackRefAddress(base))
    {
        u64 refIndex = static_cast<u64>(getRefAddressIndex(base)) + wordOffset;
        if (refIndex > cRefAddressPayloadMask)
        {
            return cInvalidAddress;
        }

        VMWord tag = base & cRefAddressKindMask;
        return tag | static_cast<VMWord>(refIndex);
    }

    return cInvalidAddress;
}

template <typename T>
static T readPC(const u8*& pc)
{
    static_assert(std::is_trivially_copyable_v<T>, "readPC<T> requires a trivially copyable T!");

    if constexpr (sizeof(T) == 1)
    {
        return static_cast<T>(*pc++);
    }
    else
    {
        T value = bits::readUnaligned<T>(pc);
        pc += sizeof(T);
        return value;
    }
}

static constexpr char cTrue[] = "true";
static constexpr char cFalse[] = "false";
static constexpr u32 cTrueLen = sizeof(cTrue) - 1;
static constexpr u32 cFalseLen = sizeof(cFalse) - 1;

static u32 getBoolFormatLen(u8 value)
{
    return value != 0 ? cTrueLen : cFalseLen;
}

static char* writeBoolChars(char* dst, u8 value)
{
    // Writes a bool name into dst and advances the pointer.
    if (value != 0)
    {
        std::memcpy(dst, cTrue, cTrueLen);
        return dst + cTrueLen;
    }
    std::memcpy(dst, cFalse, cFalseLen);
    return dst + cFalseLen;
}

static u32 getIntFormatLen(i32 value)
{
    char buf[16];
    int n = std::snprintf(buf, sizeof(buf), "%d", value);
    return n > 0 ? static_cast<u32>(n) : 0U;
}

static char* writeIntChars(char* dst, i32 value)
{
    // Writes an int into dst and advances the pointer.
    int n = std::snprintf(dst, 16, "%d", value);
    return dst + (n > 0 ? static_cast<u32>(n) : 0U);
}

static u32 getFloatFormatLen(f32 value)
{
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%f", value);
    return n > 0 ? static_cast<u32>(n) : 0U;
}

static char* writeFloatChars(char* dst, f32 value)
{
    // Writes a float into dst and advances the pointer.
    int n = std::snprintf(dst, 64, "%f", value);
    return dst + (n > 0 ? static_cast<u32>(n) : 0U);
}

VM::VM(const ExecutableImage& image)
    : mStack(8192)
    , mImage(image)
    , mOutput(nullptr)
{
    initialize();
}

void VM::initialize()
{
    // Prepare the call stack.
    mCallStack.reserve(512);

    // Initialize the global variables.
    mGlobals = mImage.mInitialGlobals;

    // Set up the type layout table for GC.
    mHeap.setTypeLayoutTable(&mImage.mTypeLayoutTable);
}

static SIMLANG_NOINLINE VMAddress getRuntimeErrorAddress(const u8* pc, const u8* code)
{
    // Runtime errors are reported after the opcode byte and any already-read operands.
    if (pc == code)
    {
        return cInvalidVMAddress;
    }

    // Use the last consumed byte so the address still falls inside the current instruction's source-map range.
    usize offset = static_cast<usize>((pc - code) - 1);
    if (offset > cMaxValidVMAddress)
    {
        return cInvalidVMAddress;
    }

    return static_cast<VMAddress>(offset);
}

static SIMLANG_NOINLINE i64 toRuntimeErrorValue(u64 value)
{
    // We want to convert to i64, so clamp if the value is more than that.
    if (value > static_cast<u64>(std::numeric_limits<i64>::max()))
    {
        return std::numeric_limits<i64>::max();
    }

    return static_cast<i64>(value);
}

static SIMLANG_NOINLINE i64 toRuntimeErrorByteCount(u64 wordCount)
{
    // This is probably a bit overkill, BUT we account for word size * word count not overflowing.
    static constexpr u64 cMaxWordCount = std::numeric_limits<u64>::max() / sizeof(VMWord);
    if (wordCount > cMaxWordCount)
    {
        return toRuntimeErrorValue(std::numeric_limits<u64>::max());
    }

    return toRuntimeErrorValue(wordCount * sizeof(VMWord));
}

SIMLANG_NOINLINE void VM::reportRuntimeError(VMAddress address, RuntimeErrorKind kind, i64 value0, i64 value1)
{
    ++mRuntimeErrorCount;

    RuntimeError error;
    error.mKind = kind;
    error.mBytecodeAddress = address;
    error.mSourceLocation = mImage.mDebugInfo.findSourceLocation(address);
    error.mValue0 = value0;
    error.mValue1 = value1;

    if (mRuntimeErrorSink != nullptr)
    {
        mRuntimeErrorSink->reportRuntimeError(error, mImage);
    }

    if (mRuntimeErrorMode == RuntimeErrorMode::cHaltOnError)
    {
        mHalted = true;
    }
}

SIMLANG_NOINLINE void VM::reportReferenceRuntimeError(VMAddress address, VMWord value)
{
    // If the reference address is invalid, we must have already reported it earlier.
    if (value == cInvalidAddress)
    {
        return;
    }

    RuntimeErrorKind kind = value == cNullRef ? RuntimeErrorKind::cNullReference : RuntimeErrorKind::cInvalidReference;
    reportRuntimeError(address, kind, value);
}

SIMLANG_NOINLINE void VM::reportStringRuntimeError(VMAddress address, VMWord value)
{
    reportRuntimeError(address, RuntimeErrorKind::cInvalidStringHandle, value);
}

SIMLANG_NOINLINE void VM::reportStackOverflow(VMAddress address, u64 requiredWords)
{
    reportRuntimeError(address, RuntimeErrorKind::cStackOverflow, toRuntimeErrorValue(requiredWords), cMaxVMStackWords);
    // This is always fatal at the moment.
    // We could probably recover from this at call time and push a default value instead.
    mHalted = true;
}

Heap::Roots VM::makeRoots() const
{
    // Our current GC roots are the globals, the stack, and VM-internal temporary roots.
    Heap::Roots r;
    r.mGlobals = ArrayView{mGlobals.data(), mGlobals.size()};
    r.mStack = ArrayView{mStack.getData(), mStack.getSize()};
    r.mTemp = ArrayView{mTempRoots.data(), mTempRoots.size()};
    return r;
}

bool VM::makeListRef(TypeID typeID, const VMWord* words, u32 length, VMWord& outRef)
{
    // Note that this doesn't push the created list onto the ToS -- the caller should do that.
    outRef = cNullRef;

    // We need the runtime info for the type.
    const TypeLayout& ti = mImage.mTypeLayoutTable.getLayout(typeID);

    // From that, get the size of the elements.
    u32 elementWords = ti.getSizeAsValue();

    Heap::ListAlloc alloc = mHeap.allocateList(typeID, elementWords, length, *this);
    if (alloc.mRef == cNullRef)
    {
        // If this somehow failed, push null and error.
        u64 requestedWords = static_cast<u64>(length) * elementWords;
        reportRuntimeError(cInvalidVMAddress,
                           RuntimeErrorKind::cAllocationFailed,
                           toRuntimeErrorByteCount(requestedWords));
        return false;
    }

    ListBlock block = alloc.mBlock;
    block.mHeader->mLength = length;

    u32 wordCount = length * elementWords;

    // Set the data.
    if (wordCount > 0 && words != nullptr)
    {
        // Copy from another place.
        copyWords(block.getElement(0), words, wordCount);
    }
    else if (wordCount > 0)
    {
        // Zero out.
        std::memset(block.getElement(0), 0, wordCount * sizeof(VMWord));
    }

    outRef = alloc.mRef;

    return true;
}

bool VM::makeMapRef(TypeID keyTypeID, TypeID valueTypeID, u32 capacity, VMWord& outRef)
{
    // Note that this doesn't push the created map onto the ToS -- the caller should do that.
    outRef = cNullRef;

    const TypeLayout& valueLayout = mImage.mTypeLayoutTable.getLayout(valueTypeID);
    u32 valueWords = valueLayout.getSizeAsValue();

    Heap::MapAlloc alloc = mHeap.allocateMap(keyTypeID, valueTypeID, valueWords, capacity, *this);
    if (alloc.mRef == cNullRef)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cAllocationFailed, toRuntimeErrorValue(capacity));
        return false;
    }

    outRef = alloc.mRef;

    return true;
}

bool VM::getCheckedListBlock(VMWord handle, ListBlock& out, VMAddress address)
{
    out = ListBlock{};

    if (mHeap.resolveListHandle(handle, out) == false)
    {
        reportReferenceRuntimeError(address, handle);
        return false;
    }

    return true;
}

bool VM::ensureListCapacity(VMWord handle, ListBlock& block, u32 minCapacity, VMAddress address)
{
    // If we still have space, we're good.
    u32 oldCapacity = block.getCapacity();
    if (oldCapacity >= minCapacity)
    {
        return true;
    }

    // Otherwise, we need to grow.
    // Go with *2 until we've reached our target capacity.
    u32 newCapacity = oldCapacity == 0 ? 4U : oldCapacity;
    while (newCapacity < minCapacity)
    {
        if (newCapacity > std::numeric_limits<u32>::max() / 2U)
        {
            newCapacity = minCapacity;
            break;
        }

        newCapacity *= 2U;
    }

    // Do the resize.
    if (mHeap.resizeList(handle, newCapacity, *this) == false)
    {
        reportRuntimeError(address, RuntimeErrorKind::cAllocationFailed, toRuntimeErrorValue(newCapacity));
        return false;
    }

    // Get the data.
    if (getCheckedListBlock(handle, block, address) == false)
    {
        return false;
    }

    return true;
}

bool VM::getListSize(VMWord handle, u32& outSize)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        outSize = 0;
        return false;
    }

    outSize = block.mHeader->mLength;

    return true;
}

bool VM::reserveList(VMWord handle, u32 capacity)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    return ensureListCapacity(handle, block, capacity, cInvalidVMAddress);
}

bool VM::clearList(VMWord handle)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    block.clear();

    return true;
}

bool VM::readListElementWords(VMWord handle, u32 index, VMWord* dst, u32 wordCount)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    if (index >= block.mHeader->mLength)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cIndexOutOfBounds, index, block.mHeader->mLength);
        return false;
    }

    // Get the element we're interested in and copy stuff out.
    VMWord* element = block.getElement(index);
    if (wordCount == 1)
    {
        dst[0] = element[0];
    }
    else
    {
        copyWords(dst, element, wordCount);
    }

    return true;
}

bool VM::writeListElementWords(VMWord handle, u32 index, const VMWord* src, u32 wordCount)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    if (index >= block.mHeader->mLength)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cIndexOutOfBounds, index, block.mHeader->mLength);
        return false;
    }

    // Get the element we're interested in and copy stuff in.
    VMWord* element = block.getElement(index);
    if (wordCount == 1)
    {
        element[0] = src[0];
    }
    else
    {
        copyWords(element, src, wordCount);
    }

    return true;
}

bool VM::pushListElementWords(VMWord handle, const VMWord* src, u32 wordCount)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    // Make space for the new element.
    if (ensureListCapacity(handle, block, block.mHeader->mLength + 1U, cInvalidVMAddress) == false)
    {
        return false;
    }

    // Get the slot at the end of the list and copy stuff in.
    VMWord* element = block.getAppendSlot();
    if (wordCount == 1)
    {
        element[0] = src[0];
    }
    else
    {
        copyWords(element, src, wordCount);
    }

    ++block.mHeader->mLength;

    return true;
}

bool VM::insertListElementWords(VMWord handle, u32 index, const VMWord* src, u32 wordCount)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    if (index > block.mHeader->mLength)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cIndexOutOfBounds, index, block.mHeader->mLength);
        return false;
    }

    // Grow the list by one element.
    if (ensureListCapacity(handle, block, block.mHeader->mLength + 1U, cInvalidVMAddress) == false)
    {
        return false;
    }

    VMWord* element = block.insertSlot(index);

    // Store the words.
    if (wordCount == 1)
    {
        element[0] = src[0];
    }
    else
    {
        copyWords(element, src, wordCount);
    }

    return true;
}

bool VM::removeListElementWords(VMWord handle, u32 index, VMWord* dst, u32 wordCount)
{
    ListBlock block;
    if (getCheckedListBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    if (index >= block.mHeader->mLength)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cIndexOutOfBounds, index, block.mHeader->mLength);
        return false;
    }

    // Store the words (since they will be overwritten as the element is removed).
    VMWord* element = block.getElement(index);
    if (wordCount == 1)
    {
        dst[0] = element[0];
    }
    else
    {
        copyWords(dst, element, wordCount);
    }

    block.removeSlot(index);

    return true;
}

bool VM::getCheckedMapBlock(VMWord handle, MapBlock& out, VMAddress address)
{
    out = MapBlock{};

    if (mHeap.resolveMapHandle(handle, out) == false)
    {
        reportReferenceRuntimeError(address, handle);
        return false;
    }

    return true;
}

bool VM::ensureMapCapacity(VMWord handle, MapBlock& block, u32 minCapacity, VMAddress address)
{
    // If we have enough capacity, bail.
    u32 oldEntryCapacity = block.getEntryCapacity();
    if (oldEntryCapacity >= minCapacity)
    {
        return true;
    }

    // Otherwise, compute the new capacity.
    u32 newCapacity = oldEntryCapacity == 0 ? 1U : oldEntryCapacity;
    while (newCapacity < minCapacity)
    {
        // Don't overshoot.
        if (newCapacity > std::numeric_limits<u32>::max() / 2U)
        {
            // If we would overflow, take the requested capacity (since that's a u32 anyway).
            newCapacity = minCapacity;
            break;
        }

        newCapacity *= 2U;
    }

    // Do the resize.
    if (mHeap.resizeMap(handle, newCapacity, *this) == false)
    {
        reportRuntimeError(address, RuntimeErrorKind::cAllocationFailed, toRuntimeErrorValue(newCapacity));
        return false;
    }

    // Get the map block of the map that we just resized.
    if (getCheckedMapBlock(handle, block, address) == false)
    {
        return false;
    }

    return true;
}

bool VM::hashMapKey(TypeID keyTypeID, VMWord key, VMAddress address, u32& outHash)
{
    outHash = 0;

    if (keyTypeID == cStringTypeID)
    {
        // If we have a string as key, get the string first.
        std::string_view value;
        if (getCheckedStringView(key, value, address) == false)
        {
            return false;
        }

        // Hash it.
        outHash = hashBytes32(value) & cMapHashMask;

        return true;
    }

    if (keyTypeID == cFloatTypeID)
    {
        // If we have a float, reinterpret the bytes as such and hash them.
        f32 value = bits::bitCast<f32>(key);
        outHash = mixHash32(value == 0.0f ? 0U : key) & cMapHashMask;
        return true;
    }

    // Bools and ints are hashed normally.
    outHash = mixHash32(key) & cMapHashMask;

    return true;
}

bool VM::mapKeysEqual(TypeID keyTypeID, VMWord lhs, VMWord rhs, VMAddress address, bool& out)
{
    out = false;

    // String key.
    if (keyTypeID == cStringTypeID)
    {
        return stringsEqualChecked(lhs, rhs, address, out);
    }

    // Float key.
    if (keyTypeID == cFloatTypeID)
    {
        out = bits::bitCast<f32>(lhs) == bits::bitCast<f32>(rhs);
        return true;
    }

    // For everything else (int and bool) compare directly.
    out = lhs == rhs;

    return true;
}

bool VM::findMapEntry(const MapBlock& block, VMWord key, u32 hash, VMAddress address, u32& outIndex, bool& outFound)
{
    // Simple wrapper around the map block's findEntry.
    // We pass in our key equal function as comparator.
    return block.findEntry(
        hash,
        [&](VMWord entryKey, bool& equal)
        {
            return mapKeysEqual(block.mHeader->mTypeID, entryKey, key, address, equal);
        },
        outIndex,
        outFound);
}

bool VM::getMapSize(VMWord handle, u32& outSize)
{
    MapBlock block;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false)
    {
        outSize = 0;
        return false;
    }

    outSize = block.mHeader->mLength;

    return true;
}

bool VM::reserveMap(VMWord handle, u32 capacity)
{
    MapBlock block;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    return ensureMapCapacity(handle, block, capacity, cInvalidVMAddress);
}

bool VM::clearMap(VMWord handle)
{
    MapBlock block;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false)
    {
        return false;
    }

    block.clearBuckets();
    block.mHeader->mLength = 0;

    return true;
}

bool VM::containsMapKeyWord(VMWord handle, VMWord key, bool& out)
{
    out = false;

    MapBlock block;
    u32 hash = 0;
    u32 index = 0;
    bool found = false;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false ||
        hashMapKey(block.mHeader->mTypeID, key, cInvalidVMAddress, hash) == false ||
        findMapEntry(block, key, hash, cInvalidVMAddress, index, found) == false)
    {
        return false;
    }

    out = found;

    return true;
}

bool VM::readMapValueWords(VMWord handle, VMWord key, VMWord* dst, u32 wordCount)
{
    MapBlock block;
    u32 hash = 0;
    u32 index = 0;
    bool found = false;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false ||
        hashMapKey(block.mHeader->mTypeID, key, cInvalidVMAddress, hash) == false ||
        findMapEntry(block, key, hash, cInvalidVMAddress, index, found) == false)
    {
        return false;
    }

    if (found == false)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cMissingMapKey);
        return false;
    }

    VMWord* value = block.getValue(index);
    if (wordCount == 1)
    {
        dst[0] = value[0];
    }
    else
    {
        copyWords(dst, value, wordCount);
    }

    return true;
}

bool VM::writeMapValueWords(VMWord handle, VMWord key, const VMWord* src, u32 wordCount)
{
    MapBlock block;
    u32 hash = 0;
    u32 index = 0;
    bool found = false;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false ||
        hashMapKey(block.mHeader->mTypeID, key, cInvalidVMAddress, hash) == false ||
        findMapEntry(block, key, hash, cInvalidVMAddress, index, found) == false)
    {
        return false;
    }

    if (found == false)
    {
        if (ensureMapCapacity(handle, block, block.mHeader->mLength + 1U, cInvalidVMAddress) == false ||
            findMapEntry(block, key, hash, cInvalidVMAddress, index, found) == false)
        {
            return false;
        }

        SIMLANG_ASSERTM(found == false && index < block.getBucketCapacity(),
                        "Map insertion must find an empty bucket.");
        block.insertEntry(index, hash, key);
    }

    VMWord* value = block.getValue(index);
    if (wordCount == 1)
    {
        value[0] = src[0];
    }
    else
    {
        copyWords(value, src, wordCount);
    }

    return true;
}

bool VM::removeMapKeyWord(VMWord handle, VMWord key, bool& outRemoved)
{
    outRemoved = false;

    MapBlock block;
    u32 hash = 0;
    u32 index = 0;
    bool found = false;
    if (getCheckedMapBlock(handle, block, cInvalidVMAddress) == false ||
        hashMapKey(block.mHeader->mTypeID, key, cInvalidVMAddress, hash) == false ||
        findMapEntry(block, key, hash, cInvalidVMAddress, index, found) == false)
    {
        return false;
    }

    if (found == false)
    {
        return true;
    }

    block.removeEntry(index);
    outRemoved = true;

    return true;
}

bool VM::makeStringAlloc(std::string_view str, VMWord& outRef)
{
    outRef = cNullRef;

    if (str.data() == nullptr && str.empty() == false)
    {
        SIMLANG_BREAK("Trying to create a string from null bytes.");
        return false;
    }

    if (str.size() > std::numeric_limits<u32>::max())
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cAllocationFailed, toRuntimeErrorValue(str.size()));
        return false;
    }

    u32 len32 = static_cast<u32>(str.size());

    // Make a new string.
    // Note that the heap already reserves space for the null-terminator.
    Heap::StringAlloc alloc = mHeap.allocateString(len32, *this);
    if (alloc.mBytes == nullptr)
    {
        reportRuntimeError(cInvalidVMAddress, RuntimeErrorKind::cAllocationFailed, toRuntimeErrorValue(len32));
        return false;
    }

    if (len32 != 0)
    {
        std::memcpy(alloc.mBytes, str.data(), len32);
    }
    alloc.mBytes[len32] = '\0';

    outRef = alloc.mRef;

    return true;
}

bool VM::makeStringListAlloc(const std::string_view* strings, u32 length, VMWord& outRef)
{
    outRef = cNullRef;

    if (strings == nullptr && length != 0)
    {
        SIMLANG_BREAK("Trying to create a null string list.");
        return false;
    }

    // Allocate the list.
    if (makeListRef(cStringTypeID, nullptr, length, outRef) == false)
    {
        return false;
    }

    // If the list is empty, we're already done.
    if (length == 0)
    {
        return true;
    }

    // We need to root the list handle.
    // Otherwise, GC might collect it while we're allocating the string elements on the heap.
    mTempRoots.push_back(outRef);
    OnScopeEnd popRoot{[this]
                       {
                           mTempRoots.pop_back();
                       }};

    for (u32 i = 0; i < length; ++i)
    {
        // Create the string.
        VMWord stringRef = cNullRef;
        if (makeStringAlloc(strings[i], stringRef) == false)
        {
            outRef = cNullRef;
            return false;
        }

        // We need to reacquire the list block every time in case the heap grew.
        ListBlock block;
        if (getCheckedListBlock(outRef, block, cInvalidVMAddress) == false)
        {
            outRef = cNullRef;
            return false;
        }
        // Set the string reference in the list.
        *block.getElement(i) = stringRef;
    }

    return true;
}

bool VM::getUncheckedStringView(VMWord value, std::string_view& out)
{
    out = {};

    if (isStaticStringHandle(value))
    {
        // If this is a static string handle, it means we can look it up in the literals.
        StringLiteralIdx index = getStaticStringLiteralIndex(value);
        if (index >= mImage.mStrings.mLiterals.size())
        {
            return false;
        }

        // If it is a valid string, fetch it.
        const StringLiteral& lit = mImage.mStrings.mLiterals[index];
        // If the requested byte range is oob, this is an error.
        u64 requiredEnd = static_cast<u64>(lit.mOffset) + static_cast<u64>(lit.mLength);
        if (requiredEnd > mImage.mStrings.mBytes.size())
        {
            return false;
        }

        // Otherwise, we can reinterpret the bytes as char* at the start of the offset.
        auto str = reinterpret_cast<const char*>(mImage.mStrings.mBytes.data() + lit.mOffset);
        out = std::string_view{str, lit.mLength};
        return true;
    }

    // If we're here, this is a dynamic string, so we need to look it up on the heap.
    Block block;
    if (mHeap.resolvePayloadAddress(value, block) == false)
    {
        return false;
    }

    if (block.mHeader->mBlockType != BlockType::cString)
    {
        return false;
    }

    out = StringBlock{block.mHeader, block.mPayload}.getView();

    return true;
}

bool VM::getCheckedStringView(VMWord value, std::string_view& out, VMAddress address)
{
    // Same as unchecked but with error reporting (is this smart?).
    if (getUncheckedStringView(value, out))
    {
        return true;
    }

    if (value == cNullRef)
    {
        reportStringRuntimeError(address, value);
    }
    else
    {
        SIMLANG_BREAK("Invalid non-null string value.");
    }

    return false;
}

bool VM::stringsEqualChecked(VMWord a, VMWord b, VMAddress address, bool& out)
{
    out = false;

    // If we're comparing the same string value, we're already done.
    if (a == b)
    {
        out = true;
        return true;
    }

    // Do the actual comparison for the word we got.
    std::string_view sa;
    std::string_view sb;
    bool aOk = getCheckedStringView(a, sa, address);
    if (aOk == false && mHalted)
    {
        return false;
    }

    bool bOk = getCheckedStringView(b, sb, address);
    if (aOk == false || bOk == false)
    {
        return false;
    }

    out = sa == sb;

    return true;
}

bool VM::stringsEqual(VMWord a, VMWord b, VMAddress address)
{
    bool equal = false;
    return stringsEqualChecked(a, b, address, equal) && equal;
}

bool VM::run()
{
    // Our bytecode to be executed.
    const u8* code = mImage.mBytes.data();
    // The program counter pointing to the next bytecode op to be executed.
    const u8* pc = code;

    // Stack stuff.
    VMStack& stack = mStack;
    // These local pointers need to be kept in sync if modified externally.
    // That can happen if e.g. syscalls push/pop from the stack.
    VMWord* stackBase = stack.getData();
    VMWord* sp = stackBase + stack.getSize();
    // The function pointer is an index into the stack.
    u32 fp = 0;

    // Common idioms based on the above:
    // The stack pointer always points to the next free slot.
    // Push: *sp++ = value
    // -> Write at the current stack pointer slot and then increment it.
    // Pop: value = *--sp
    // -> Decrement the stack pointer and then read (pop) the value from the ToS.
    // Read/Write ToS: sp[-1] [= value]
    // -> Read/Write the value right below the stack pointer, which is the ToS.
    // -1 is the ToS, -2 the slot below that, etc.

    // We're using a Lua idea here since it provides great performance.
    // For GCC and Clang we use a goto dispatch table since that outperforms switch by a good margin.
    // MSVC doesn't support that, so we use a traditional switch alternative.
    // For now, this is great for performance, hooking up a proper debugger may require a different approach though.

    // ====================
    // Loop start:

    // while(mHalted == false) {
    //     switch(static_cast<OpCode>(*pc++)) {

    // goto *dispatch[*pc++]

    // ====================
    // Op:

    // case OpCode::cOp:
    // ...
    // break;

    // opLabel:
    // ...
    // For ops that can trigger halt: if(mHalted) goto endLabel
    // goto *dispatch[*pc++]

    // ====================
    // Loop end:

    // } (for the switch)

    // endLabel: (for the goto)

#if (defined(__GNUC__) || defined(__clang__))
    #define VM_USE_GOTO 1
#else
    #define VM_USE_GOTO 0
#endif

#if VM_USE_GOTO

    #define VM_LABEL(NAME) LABEL_##NAME
    #define VM_END_LABEL VM_LABEL(End)

    // Build the goto dispatch table for each op.
    // The labels are defined as LABEL_OpCode
    static void* const cDispatch[] = {

    #define X(NAME, COUNT, T1, N1, T2, N2) &&VM_LABEL(NAME),

    #include "runtime/op/opcodes.def"

    #undef X

    };

    // The start of an op is always the label.
    #define VM_OP(NAME) \
        VM_LABEL(NAME) \
            :

    // If we're at the end, we need to look up the next one to go to.
    // Only use this if the op we executed cannot cause a halt.
    #define VM_END_OP() VM_DISPATCH()
    // Use this if the op may have caused a halt and we're done.
    #define VM_END_OP_CHECKED() \
        do \
        { \
            if (mHalted) \
            { \
                goto VM_END_LABEL; \
            } \
            VM_DISPATCH(); \
        } while (false)

    #define VM_HALT() \
        mHalted = true; \
        goto VM_END_LABEL

    // Go to the first instruction, if we somehow halted already (?) we're done.
    #define VM_DISPATCH_START() \
        do \
        { \
            if (mHalted == false) \
            { \
                VM_DISPATCH(); \
            } \
            goto VM_END_LABEL; \
        } while (false)

    // This is the normal dispatch case when it's impossible (fingers crossed) to encounter an error.
    #define VM_DISPATCH() goto*(cDispatch[static_cast<u8>(*pc++)])

    #define VM_DISPATCH_END() \
    VM_END_LABEL:

#else

    // The start of an op is always a case.
    #define VM_OP(NAME) case OpCode::c##NAME:

    // The end of an op is always a break in both cases.
    #define VM_END_OP() break
    #define VM_END_OP_CHECKED() break

    #define VM_HALT() \
        mHalted = true; \
        break

    #define VM_DISPATCH_START() \
        while (mHalted == false) \
        { \
            switch (static_cast<OpCode>(*pc++)) \
            {

    #define VM_DISPATCH()

    // clang-format off
    #define VM_DISPATCH_END() \
            } \
        }
    // clang-format on

#endif

// Synchronizes the stack size with the current stack pointer.
// Effectively sets the size of mStack to sp - stackBase (without writing anything to the stack).
// Must be used BEFORE anything outside this method accesses the stack.
// (You have been warned.)
#define STACK_SYNC() stack.setSize(static_cast<u32>(sp - stackBase))

// Reloads the stack base and pointer after a potential stack resize.
// Effectively sets the local stackBase and sp to the values of mStack.
// Must be used AFTER anything outside of this method accesses the stack.
// (You have been warned.)
#define STACK_RELOAD() \
    do \
    { \
        stackBase = stack.getData(); \
        sp = stackBase + stack.getSize(); \
    } while (false)

    // Applies a binary operator to the top 2 words on the stack.
    // 1. Decrement the stack pointer and read the top word.
    // 2. Read the word just below that.
    // 3. Apply the operator and write back the result on the new ToS.
    // This implicitly pops the top 2 words and writes back the result.
#define VM_BINARY_WORD(OP) \
    do \
    { \
        VMWord rhs = *--sp; \
        VMWord lhs = sp[-1]; \
        sp[-1] = lhs OP rhs; \
    } while (false)

    // Applies a comparison operator to the top 2 words on the stack.
    // 1. Decrement the stack pointer and read the top word.
    // 2. Read the word just below that.
    // 3. Apply the operator and write back the result on the new ToS.
    // This implicitly pops the top 2 words and writes back the result.
#define VM_COMPARE_WORD(OP) \
    do \
    { \
        VMWord rhs = *--sp; \
        VMWord lhs = sp[-1]; \
        sp[-1] = (lhs OP rhs) ? 1U : 0U; \
    } while (false)

    // Applies a comparison operator to the top 2 words on the stack, interpreted as signed integers.
    // 1. Decrement the stack pointer and read the top word.
    // 2. Read the word just below that.
    // 3. Apply the operator and write back the result on the new ToS.
    // This implicitly pops the top 2 words and writes back the result.
#define VM_COMPARE_SIGNED(OP) \
    do \
    { \
        i32 rhs = static_cast<i32>(*--sp); \
        i32 lhs = static_cast<i32>(sp[-1]); \
        sp[-1] = (lhs OP rhs) ? 1U : 0U; \
    } while (false)

    // Applies a binary operator to the top 2 words on the stack, interpreted as floats.
    // 1. Decrement the stack pointer and read the top word.
    // 2. Read the word just below that.
    // 3. Apply the operator and write back the result on the new ToS.
    // This implicitly pops the top 2 words and writes back the result.
#define VM_BINARY_FLOAT(OP) \
    do \
    { \
        f32 rhs = bits::bitCast<f32>(*--sp); \
        f32 lhs = bits::bitCast<f32>(sp[-1]); \
        sp[-1] = bits::bitCast<VMWord>(lhs OP rhs); \
    } while (false)

    // Applies a comparison operator to the top 2 words on the stack, interpreted as floats.
    // 1. Decrement the stack pointer and read the top word.
    // 2. Read the word just below that.
    // 3. Apply the operator and write back the result on the new ToS.
    // This implicitly pops the top 2 words and writes back the result.
#define VM_COMPARE_FLOAT(OP) \
    do \
    { \
        f32 rhs = bits::bitCast<f32>(*--sp); \
        f32 lhs = bits::bitCast<f32>(sp[-1]); \
        sp[-1] = (lhs OP rhs) ? 1U : 0U; \
    } while (false)

    // Resolves a VM word address to a pointer, handling heap, global, and stack references.
    auto resolveRef = [&](VMWord address) -> VMWord*
    {
        // Any reference is either a heap address, a global reference, or a stack reference.
        if (isHeapAddress(address))
        {
            HeapIndex index = getHeapPayload(address);
            return &mHeap.wordAt(index);
        }

        if (isGlobalRefAddress(address))
        {
            u32 index = getRefAddressIndex(address);
            return &mGlobals[index];
        }

        if (isStackRefAddress(address))
        {
            u32 index = getRefAddressIndex(address);
            return stackBase + index;
        }

        return nullptr;
    };

    // Same as above, but with an offset.
    auto resolveRefOffset = [&](VMWord address, u32 wordOffset) -> VMWord*
    {
        // Resolve the base reference, then offset it in the resolved address space.
        VMWord* base = resolveRef(address);
        return base == nullptr ? nullptr : base + wordOffset;
    };

    // Resolve a list handle and an index to the list element (if it exists).
    auto resolveListElementPayload = [&](VMWord listHandle, i32 index, VMAddress address) -> VMWord*
    {
        ListBlock block;
        if (getCheckedListBlock(listHandle, block, address) == false)
        {
            return nullptr;
        }

        // Check bounds.
        if (index < 0 || static_cast<u32>(index) >= block.mHeader->mLength)
        {
            reportRuntimeError(address, RuntimeErrorKind::cIndexOutOfBounds, index, block.mHeader->mLength);
            return nullptr;
        }

        return block.getElement(static_cast<u32>(index));
    };

    VM_DISPATCH_START();

    VM_OP(Halt)
    {
        // Code:  Halt()
        // Stack: [...] -> [...]
        VM_HALT();
    }
    VM_OP(Call)
    {
        // Code:  Call(index: FunctionIdx)
        // Stack: [argN-1, ..., arg0, ...] -> [localN-1, ..., local0, argN-1, ..., arg0, ...]
        // Get the index and function info.
        FunctionIdx index = readPC<FunctionIdx>(pc);
        const FunctionInfo& fi = mImage.mFunctionInfos[index];

        // Compute the stack size from the base.
        u32 stackSize = static_cast<u32>(sp - stackBase);
        // The caller SP is just before the first arg.
        u32 callerSP = stackSize - fi.mArgWords;
        // Reserve our words if needed.
        u64 requiredStackWords = static_cast<u64>(callerSP) + fi.mMaxStackWords;
        if (requiredStackWords > stack.getCapacity())
        {
            // Get the stack up to date since we might reallocate.
            STACK_SYNC();

            // Reserve the words and check for overflow.
            if (stack.reserve(requiredStackWords) == false)
            {
                reportStackOverflow(getRuntimeErrorAddress(pc, code), requiredStackWords);
                VM_END_OP_CHECKED();
            }

            // Reload the stack into the locals.
            STACK_RELOAD();
        }

        // Store the old stuff.
        CallFrame frame;
        frame.mFunctionIdx = index;
        // Already pointing to the next instruction after the call.
        frame.mReturnPC = static_cast<VMAddress>(pc - code);
        // The caller frame pointer is the current frame pointer (duh).
        frame.mCallerFP = fp;
        // The caller stack pointer is the current stack pointer without the pushed params.
        frame.mCallerSP = callerSP;
        mCallStack.push_back(frame);

        // Jump to the function entry address.
        pc = code + fi.mEntryAddress;
        // The new frame pointer starts at the first param.
        fp = callerSP;
        // Now we can push the locals on top of the params (if there are any).
        // Since we force initialization on declaration, we can do the raw resize.
        sp += fi.mLocalWords;

        VM_END_OP();
    }
    VM_OP(CallMethod)
    {
        // Code:  CallMethod(index: FunctionIdx)
        // Stack: [argN-1, ..., arg0, receiver, ...] -> [localN-1, ..., local0, argN-1, ..., arg0, receiver, ...]
        // Invalid receiver: [argN-1, ..., arg0, receiver, ...] -> [retN-1, ..., ret0, ...]
        // Get the index and function info.
        FunctionIdx index = readPC<FunctionIdx>(pc);
        const FunctionInfo& fi = mImage.mFunctionInfos[index];

        // Compute the stack size from the base.
        u32 stackSize = static_cast<u32>(sp - stackBase);
        // The caller SP is just before the first arg.
        u32 callerSP = stackSize - fi.mArgWords;

        // Get the receiver, which is the first arg.
        VMWord receiver = stackBase[callerSP];
        if (isHeapAddress(receiver) == false)
        {
            // If this is not a heap address, this is an error (likely null).
            // Pop everything so we can return.
            sp = stackBase + callerSP;
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            // Clean up the stack and push 0.
            if (fi.mReturnWords > 0)
            {
                // Reserve space for the return values.
                VMWord* returnWords = sp;
                sp += fi.mReturnWords;
                // Write them starting at the callerSP.
                std::memset(returnWords, 0, fi.mReturnWords * sizeof(VMWord));
            }

            // We may have halted, so handle that.
            VM_END_OP_CHECKED();
        }

        // Reserve our words if needed.
        u64 requiredStackWords = static_cast<u64>(callerSP) + fi.mMaxStackWords;
        if (requiredStackWords > stack.getCapacity())
        {
            // Get the stack up to date since we might reallocate.
            STACK_SYNC();

            // Reserve the words and check for overflow.
            if (stack.reserve(requiredStackWords) == false)
            {
                reportStackOverflow(getRuntimeErrorAddress(pc, code), requiredStackWords);
                VM_END_OP_CHECKED();
            }

            // Reload the stack into the locals.
            STACK_RELOAD();
        }

        // Execute the call, same as for a normal call.
        CallFrame frame;
        frame.mFunctionIdx = index;
        frame.mReturnPC = static_cast<VMAddress>(pc - code);
        frame.mCallerFP = fp;
        frame.mCallerSP = callerSP;
        mCallStack.push_back(frame);

        pc = code + fi.mEntryAddress;
        fp = callerSP;
        sp += fi.mLocalWords;

        VM_END_OP();
    }
    VM_OP(CallInterface)
    {
        // Code:  CallInterface(index: InterfaceCallIdx)
        // Stack: [argN-1, ..., arg0, table, receiver, ...] -> [localN-1, ..., local0, argN-1, ..., arg0, receiver, ...]
        // Get the index and all required interface call info.
        InterfaceCallIdx index = readPC<InterfaceCallIdx>(pc);
        const InterfaceCallInfo& info = mImage.mInterfaceCallInfos[index];

        // Compute the stack size from the base.
        u32 stackSize = static_cast<u32>(sp - stackBase);
        // Get the stack index of the table index and the receiver object.
        // These are right below the args that the caller pushed.
        u32 tablePos = stackSize - static_cast<u32>(info.mArgWords) - 1U;
        u32 receiverPos = tablePos - 1U;

        // Check the receiver.
        VMWord receiver = stackBase[receiverPos];
        if (isHeapAddress(receiver) == false)
        {
            // If this is not a heap address, this is an error (likely null).
            // Pop everything so we can return.
            sp = stackBase + receiverPos;
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            // Clean up the stack and push 0.
            if (info.mReturnWords > 0)
            {
                // Reserve space for the return values.
                VMWord* returnWords = sp;
                sp += info.mReturnWords;
                // Write them starting at the callerSP.
                std::memset(returnWords, 0, info.mReturnWords * sizeof(VMWord));
            }

            // We may have halted, so handle that.
            VM_END_OP_CHECKED();
        }

        // Get the table index from the stack.
        VMWord tableIndexWord = stackBase[tablePos];
        // The table index from the stack plus the slot from the call info gives us the method index to call.
        u64 methodIndex = static_cast<u64>(tableIndexWord) + static_cast<u64>(info.mSlot);
        FunctionIdx functionIndex = mImage.mInterfaceMethods[methodIndex];

        // We have all info, now manipulate the stack to get the normal call shape (no table index).
        // Move everything after the table index down one slot.
        for (u32 i = tablePos; i + 1U < stackSize; ++i)
        {
            stackBase[i] = stackBase[i + 1U];
        }
        // Then pop and update the stack pointer.
        --stackSize;
        sp = stackBase + stackSize;

        // Now do normal calling like in the above ops.
        const FunctionInfo& fi = mImage.mFunctionInfos[functionIndex];
        // The caller SP is just before the first arg.
        u32 callerSP = stackSize - fi.mArgWords;
        // Reserve our words if needed.
        u64 requiredStackWords = static_cast<u64>(callerSP) + fi.mMaxStackWords;
        if (requiredStackWords > stack.getCapacity())
        {
            // Get the stack up to date since we might reallocate.
            STACK_SYNC();

            // Reserve the words and check for overflow.
            if (stack.reserve(requiredStackWords) == false)
            {
                reportStackOverflow(getRuntimeErrorAddress(pc, code), requiredStackWords);
                VM_END_OP_CHECKED();
            }

            // Reload the stack into the locals.
            STACK_RELOAD();
        }

        // Execute the call, same as for a normal call.
        CallFrame frame;
        frame.mFunctionIdx = functionIndex;
        frame.mReturnPC = static_cast<VMAddress>(pc - code);
        frame.mCallerFP = fp;
        frame.mCallerSP = callerSP;
        mCallStack.push_back(frame);

        pc = code + fi.mEntryAddress;
        fp = callerSP;
        sp += fi.mLocalWords;

        VM_END_OP();
    }
    VM_OP(Syscall)
    {
        // Code:  Syscall(index: SyscallIdx)
        // Stack: [argN-1, ..., arg0, ...] -> [retN-1, ..., ret0, ...]
        SyscallIdx index = readPC<SyscallIdx>(pc);

        // Sync the stack before we call (in case the syscall manipulates the VM).
        STACK_SYNC();

        // Look up the syscall.
        const SyscallEntry& scEntry = mImage.mSyscallInfos[index];
        // Call it.
        bool success = scEntry.mCaller(*this, scEntry);

        // Reload to make sure the stack is in sync.
        STACK_RELOAD();

        // Handle error stuff.
        if (success == false && mHalted == false)
        {
            // If this failed, the stack got corrupted, so abort.
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cSyscallFailed, index);
            mHalted = true;
        }

        // End checked in case we halted.
        VM_END_OP_CHECKED();
    }
    VM_OP(Return)
    {
        // Code:  Return()
        // Stack: [retN-1, ..., ret0, localN-1, ..., local0, argN-1, ..., arg0, ...] -> [retN-1, ..., ret0, ...]
        // Pop the frame immediately.
        CallFrame frame = mCallStack.back();
        mCallStack.pop_back();

        // Find out how many return values there are.
        const FunctionInfo& fi = mImage.mFunctionInfos[frame.mFunctionIdx];
        ReturnWordCount retWords = fi.mReturnWords;
        if (retWords > 0)
        {
            // If there are any, move them from the ToS right above the caller SP.
            // This will result in the return values being on the new ToS.
            u32 currentSize = static_cast<u32>(sp - stackBase);
            u32 retBase = currentSize - retWords;
            copyWords(stackBase + frame.mCallerSP, stackBase + retBase, retWords);
            // Shrink the stack accordingly.
            sp = stackBase + (frame.mCallerSP + retWords);
        }
        else
        {
            // No return value, the stack now is at the old SP.
            sp = stackBase + frame.mCallerSP;
        }

        // Also restore the old FP and PC.
        fp = frame.mCallerFP;
        pc = code + frame.mReturnPC;

        VM_END_OP();
    }
    VM_OP(Jump)
    {
        // Code:  Jump(address: VMAddress)
        // Stack: [...] -> [...]
        VMAddress address = readPC<VMAddress>(pc);
        pc = code + address;
        VM_END_OP();
    }
    VM_OP(JumpZ)
    {
        // Code:  JumpZ(address: VMAddress)
        // Stack: [cond, ...] -> [...]
        VMAddress address = readPC<VMAddress>(pc);
        VMWord value = *--sp;
        if (value == 0)
        {
            pc = code + address;
        }
        VM_END_OP();
    }
    VM_OP(JumpNZ)
    {
        // Code:  JumpNZ(address: VMAddress)
        // Stack: [cond, ...] -> [...]
        VMAddress address = readPC<VMAddress>(pc);
        VMWord value = *--sp;
        if (value != 0)
        {
            pc = code + address;
        }
        VM_END_OP();
    }
    VM_OP(TestZ)
    {
        // Code:  TestZ(address: VMAddress)
        // Stack: [cond, ...] -> [cond, ...] if zero; [...] otherwise
        VMAddress address = readPC<VMAddress>(pc);
        VMWord value = sp[-1];
        if (value == 0)
        {
            // Condition false: Keep and jump.
            pc = code + address;
        }
        else
        {
            // Condition true: Pop and continue.
            sp -= 1;
        }
        VM_END_OP();
    }
    VM_OP(TestNZ)
    {
        // Code:  TestNZ(address: VMAddress)
        // Stack: [cond, ...] -> [cond, ...] if nonzero; [...] otherwise
        VMAddress address = readPC<VMAddress>(pc);
        VMWord value = sp[-1];
        if (value != 0)
        {
            // Condition true: Keep and jump.
            pc = code + address;
        }
        else
        {
            // Condition false: Pop and continue.
            sp -= 1;
        }
        VM_END_OP();
    }
    VM_OP(Pop)
    {
        // Code:  Pop()
        // Stack: [word, ...] -> [...]
        sp -= 1;
        VM_END_OP();
    }
    VM_OP(PopN)
    {
        // Code:  PopN(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, ...] -> [...]
        OpWordCount size = readPC<OpWordCount>(pc);
        sp -= size;
        VM_END_OP();
    }
    VM_OP(Dup)
    {
        // Code:  Dup()
        // Stack: [word, ...] -> [word, word, ...]
        // Duplicate the word right below the ToS, then grow.
        *sp++ = sp[-1];
        VM_END_OP();
    }
    VM_OP(DupN)
    {
        // Code:  DupN(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, ...] -> [wordN-1, ..., word0, wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);
        // Copy the top N words onto the ToS.
        copyWords(sp, sp - size, size);
        // Update the stack pointer to the new size.
        sp += size;
        VM_END_OP();
    }
    VM_OP(Push8)
    {
        // Code:  Push8(int8: u8)
        // Stack: [...] -> [word, ...]
        u8 value = readPC<u8>(pc);
        *sp++ = value;
        VM_END_OP();
    }
    VM_OP(Push8S)
    {
        // Code:  Push8S(int8: i8)
        // Stack: [...] -> [word, ...]
        i8 value = readPC<i8>(pc);
        // Push as i32 since we need to keep the sign.
        *sp++ = static_cast<VMWord>(static_cast<i32>(value));
        VM_END_OP();
    }
    VM_OP(Push16)
    {
        // Code:  Push16(int16: u16)
        // Stack: [...] -> [word, ...]
        u16 value = readPC<u16>(pc);
        *sp++ = value;
        VM_END_OP();
    }
    VM_OP(Push16S)
    {
        // Code:  Push16S(int16: i16)
        // Stack: [...] -> [word, ...]
        i16 value = readPC<i16>(pc);
        // Push as i32 since we need to keep the sign.
        *sp++ = static_cast<VMWord>(static_cast<i32>(value));
        VM_END_OP();
    }
    VM_OP(Push32)
    {
        // Code:  Push32(int32: u32)
        // Stack: [...] -> [word, ...]
        u32 value = readPC<u32>(pc);
        *sp++ = value;
        VM_END_OP();
    }
    VM_OP(PushString)
    {
        // Code:  PushString(index: StringLiteralIdx)
        // Stack: [...] -> [string, ...]
        StringLiteralIdx index = readPC<StringLiteralIdx>(pc);
        // Make a string literal address and push that.
        *sp++ = makeStaticStringHandle(index);
        VM_END_OP();
    }
    VM_OP(NewObject)
    {
        // Code:  NewObject(typeID: TypeID)
        // Stack: [...] -> [object, ...]
        TypeID typeID = readPC<TypeID>(pc);

        // Get the type.
        const TypeLayout& ti = mImage.mTypeLayoutTable.getLayout(typeID);
        // Find out the payload size.
        u32 elementWords = ti.getSizeOnHeap();

        // We need to sync here so the heap gets the correct stack roots (!).
        STACK_SYNC();

        // Request the memory.
        VMWord ref = mHeap.allocateObject(typeID, elementWords, *this);
        if (ref == cNullRef)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cAllocationFailed,
                               toRuntimeErrorByteCount(elementWords));
            *sp++ = cNullRef;
            VM_END_OP_CHECKED();
        }

        // Push the ref onto the stack.
        *sp++ = ref;

        VM_END_OP();
    }
    VM_OP(NewList)
    {
        // Code:  NewList(typeID: TypeID)
        // Stack: [...] -> [list, ...]
        TypeID typeID = readPC<TypeID>(pc);

        // Get the type.
        const TypeLayout& ti = mImage.mTypeLayoutTable.getLayout(typeID);
        // Find out the element type size.
        u32 elementWords = ti.getSizeAsValue();

        // We need to sync here so the heap gets the correct stack roots (!).
        STACK_SYNC();

        Heap::ListAlloc alloc = mHeap.allocateList(typeID, elementWords, 0, *this);
        if (alloc.mRef == cNullRef)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cAllocationFailed,
                               toRuntimeErrorByteCount(1));
            *sp++ = cNullRef;
            VM_END_OP_CHECKED();
        }

        // Push the list handle.
        *sp++ = alloc.mRef;

        VM_END_OP();
    }
    VM_OP(NewMap)
    {
        // Code:  NewMap(keyTypeID: TypeID, valueTypeID: TypeID)
        // Stack: [...] -> [map, ...]
        TypeID keyTypeID = readPC<TypeID>(pc);
        TypeID valueTypeID = readPC<TypeID>(pc);

        // Get the type.
        const TypeLayout& valueLayout = mImage.mTypeLayoutTable.getLayout(valueTypeID);
        // Find out the element type size.
        u32 valueWords = valueLayout.getSizeAsValue();

        // We need to sync here so the heap gets the correct stack roots (!).
        STACK_SYNC();

        Heap::MapAlloc alloc = mHeap.allocateMap(keyTypeID, valueTypeID, valueWords, 0, *this);
        if (alloc.mRef == cNullRef)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cAllocationFailed,
                               toRuntimeErrorByteCount(1));
            *sp++ = cNullRef;
            VM_END_OP_CHECKED();
        }

        // Push the map handle.
        *sp++ = alloc.mRef;

        VM_END_OP();
    }
    VM_OP(RefLocal)
    {
        // Code:  RefLocal(localIdx: LocalIdx)
        // Stack: [...] -> [addr, ...]
        LocalIdx localIdx = readPC<LocalIdx>(pc);
        *sp++ = makeStackRefAddress(fp + localIdx);
        VM_END_OP();
    }
    VM_OP(RefGlobal)
    {
        // Code:  RefGlobal(globalIdx: GlobalIdx)
        // Stack: [...] -> [addr, ...]
        GlobalIdx globalIdx = readPC<GlobalIdx>(pc);
        *sp++ = makeGlobalRefAddress(globalIdx);
        VM_END_OP();
    }
    VM_OP(RefField)
    {
        // Code:  RefField(fieldOffset: FieldOffset)
        // Stack: [addr, ...] -> [fieldAddr, ...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);
        VMWord base = sp[-1];
        sp[-1] = offsetRefAddress(base, fieldOffset);
        VM_END_OP();
    }
    VM_OP(RefObjField)
    {
        // Code:  RefObjField(fieldOffset: FieldOffset)
        // Stack: [receiver, ...] -> [fieldAddr, ...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);

        // Check whether we're getting a valid heap address.
        VMWord receiver = sp[-1];
        if (isHeapAddress(receiver) == false)
        {
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            sp[-1] = cInvalidAddress;
            VM_END_OP_CHECKED();
        }

        // Get the address of the field and write it back to the stack.
        HeapIndex fieldIndex = getHeapPayload(receiver) + fieldOffset;
        sp[-1] = makeHeapAddress(fieldIndex);

        VM_END_OP();
    }
    VM_OP(LoadLocal)
    {
        // Code:  LoadLocal(localIdx: LocalIdx)
        // Stack: [...] -> [local, ...]
        LocalIdx localIdx = readPC<LocalIdx>(pc);
        *sp++ = stackBase[fp + localIdx];
        VM_END_OP();
    }
    VM_OP(StoreLocal)
    {
        // Code:  StoreLocal(localIdx: LocalIdx)
        // Stack: [value, ...] -> [...]
        LocalIdx localIdx = readPC<LocalIdx>(pc);
        stackBase[fp + localIdx] = *--sp;
        VM_END_OP();
    }
    VM_OP(LoadLocalN)
    {
        // Code:  LoadLocalN(localIdx: LocalIdx, size: OpWordCount)
        // Stack: [...] -> [localN-1, ..., local0, ...]
        LocalIdx localIdx = readPC<LocalIdx>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // The destination starts at the ToS.
        VMWord* dst = sp;
        // Push N words.
        sp += size;

        // Copy the locals into the pushed words.
        copyWords(dst, stackBase + (fp + localIdx), size);

        VM_END_OP();
    }
    VM_OP(StoreLocalN)
    {
        // Code:  StoreLocalN(localIdx: LocalIdx, size: OpWordCount)
        // Stack: [valueN-1, ..., value0, ...] -> [...]
        LocalIdx localIdx = readPC<LocalIdx>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // Pop N words.
        sp = src;

        // Copy the value words into the locals.
        copyWords(stackBase + (fp + localIdx), src, size);

        VM_END_OP();
    }
    VM_OP(IncLocal)
    {
        // Code:  IncLocal(localIdx: LocalIdx)
        // Stack: [] -> []
        LocalIdx localIdx = readPC<LocalIdx>(pc);
        ++stackBase[fp + localIdx];
        VM_END_OP();
    }
    VM_OP(LoadGlobal)
    {
        // Code:  LoadGlobal(globalIdx: GlobalIdx)
        // Stack: [...] -> [global, ...]
        GlobalIdx globalIdx = readPC<GlobalIdx>(pc);
        *sp++ = mGlobals[globalIdx];
        VM_END_OP();
    }
    VM_OP(StoreGlobal)
    {
        // Code:  StoreGlobal(globalIdx: GlobalIdx)
        // Stack: [value, ...] -> [...]
        GlobalIdx globalIdx = readPC<GlobalIdx>(pc);
        mGlobals[globalIdx] = *--sp;
        VM_END_OP();
    }
    VM_OP(LoadGlobalN)
    {
        // Code:  LoadGlobalN(globalIdx: GlobalIdx, size: OpWordCount)
        // Stack: [...] -> [globalN-1, ..., global0, ...]
        GlobalIdx globalIdx = readPC<GlobalIdx>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // The destination starts at the ToS.
        VMWord* dst = sp;
        // Push N words.
        sp += size;

        // Copy the globals into the pushed words.
        copyWords(dst, &mGlobals[globalIdx], size);

        VM_END_OP();
    }
    VM_OP(StoreGlobalN)
    {
        // Code:  StoreGlobalN(globalIdx: GlobalIdx, size: OpWordCount)
        // Stack: [valueN-1, ..., value0, ...] -> [...]
        GlobalIdx globalIdx = readPC<GlobalIdx>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // Pop N words.
        sp = src;

        // Copy the value words into the globals.
        copyWords(&mGlobals[globalIdx], src, size);

        VM_END_OP();
    }
    VM_OP(LoadRef)
    {
        // Code:  LoadRef()
        // Stack: [addr, ...] -> [word, ...]
        VMWord addr = sp[-1];

        // Resolve the reference and write its value to the stack.
        VMWord* refPtr = resolveRef(addr);
        sp[-1] = refPtr == nullptr ? 0U : *refPtr;

        VM_END_OP();
    }
    VM_OP(StoreRef)
    {
        // Code:  StoreRef()
        // Stack: [word, addr, ...] -> [...]
        VMWord value = *--sp;
        VMWord addr = *--sp;

        // Resolve the reference and write the value to it.
        VMWord* dst = resolveRef(addr);
        if (dst != nullptr)
        {
            *dst = value;
        }

        VM_END_OP();
    }
    VM_OP(LoadRefN)
    {
        // Code:  LoadRefN(size: OpWordCount)
        // Stack: [addr, ...] -> [wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // Pop the address.
        VMWord addr = *--sp;
        // The destination starts at the ToS.
        VMWord* dst = sp;
        // Push N words.
        sp += size;

        const VMWord* src = resolveRef(addr);
        if (src == nullptr)
        {
            // If the address is invalid, push zeros.
            std::memset(dst, 0, size * sizeof(VMWord));
        }
        else
        {
            // Copy the referenced words into the pushed words.
            copyWords(dst, src, size);
        }

        VM_END_OP();
    }
    VM_OP(StoreRefN)
    {
        // Code:  StoreRefN(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, addr, ...] -> [...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The address is right below the value words.
        VMWord addr = src[-1];
        // We're popping the address and N words.
        sp = src - 1;

        // Copy the value words into the referenced storage.
        VMWord* dst = resolveRef(addr);
        if (dst != nullptr)
        {
            copyWords(dst, src, size);
        }

        VM_END_OP();
    }
    VM_OP(LoadRefField)
    {
        // Code:  LoadRefField(fieldOffset: FieldOffset)
        // Stack: [addr, ...] -> [word, ...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);

        // Read the address.
        VMWord base = sp[-1];

        // Resolve the field and write its value to the stack.
        VMWord* src = resolveRefOffset(base, fieldOffset);
        sp[-1] = src == nullptr ? 0U : *src;

        VM_END_OP();
    }
    VM_OP(StoreRefField)
    {
        // Code:  StoreRefField(fieldOffset: FieldOffset)
        // Stack: [word, addr, ...] -> [...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);

        // The source starts at the value word.
        VMWord* src = sp - 1;
        // The address is right below the value word.
        VMWord base = src[-1];
        // Pop the address and value word.
        sp = src - 1;

        // Copy the value word into the referenced field.
        VMWord* dst = resolveRefOffset(base, fieldOffset);
        if (dst != nullptr)
        {
            *dst = *src;
        }

        VM_END_OP();
    }
    VM_OP(LoadRefFieldN)
    {
        // Code:  LoadRefFieldN(fieldOffset: FieldOffset, size: OpWordCount)
        // Stack: [addr, ...] -> [wordN-1, ..., word0, ...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // Pop the address.
        VMWord base = *--sp;
        // The destination starts at the ToS.
        VMWord* dst = sp;
        // Push N words.
        sp += size;

        const VMWord* src = resolveRefOffset(base, fieldOffset);
        if (src == nullptr)
        {
            // If the address is invalid, push zeros.
            std::memset(dst, 0, size * sizeof(VMWord));
        }
        else
        {
            // Copy the referenced field words into the pushed words.
            copyWords(dst, src, size);
        }

        VM_END_OP();
    }
    VM_OP(StoreRefFieldN)
    {
        // Code:  StoreRefFieldN(fieldOffset: FieldOffset, size: OpWordCount)
        // Stack: [wordN-1, ..., word0, addr, ...] -> [...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The base address is right below the value words.
        VMWord base = src[-1];
        // Pop the address and N words.
        sp = src - 1;

        if (isStackRefAddress(base))
        {
            // Small extra branch for stack refs.
            u32 dstBase = getRefAddressIndex(base) + fieldOffset;
            copyWords(stackBase + dstBase, src, size);
        }
        else
        {
            VMWord* dst = resolveRefOffset(base, fieldOffset);
            if (dst != nullptr)
            {
                // Copy the value words into the referenced field.
                copyWords(dst, src, size);
            }
        }

        VM_END_OP();
    }
    VM_OP(LoadObjField)
    {
        // Code:  LoadObjField(fieldOffset: FieldOffset)
        // Stack: [receiver, ...] -> [word, ...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);

        // Read the receiver.
        VMWord receiver = sp[-1];

        if (isHeapAddress(receiver) == false)
        {
            // If the receiver is invalid, write zero to the stack.
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            sp[-1] = 0;
            VM_END_OP_CHECKED();
        }

        // Compute the field address from the receiver.
        HeapIndex index = getHeapPayload(receiver) + fieldOffset;
        // Write the field value to the stack.
        sp[-1] = mHeap.wordAt(index);

        VM_END_OP();
    }
    VM_OP(StoreObjField)
    {
        // Code:  StoreObjField(fieldOffset: FieldOffset)
        // Stack: [word, receiver, ...] -> [...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);

        // The source starts at the value word.
        VMWord* src = sp - 1;
        // The receiver is right below the value word.
        VMWord receiver = src[-1];
        // Pop the receiver and value word.
        sp = src - 1;

        if (isHeapAddress(receiver) == false)
        {
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            VM_END_OP_CHECKED();
        }

        // Compute the field address from the receiver.
        HeapIndex index = getHeapPayload(receiver) + fieldOffset;
        // Copy the value word into the object field.
        mHeap.wordAt(index) = *src;

        VM_END_OP();
    }
    VM_OP(LoadObjFieldN)
    {
        // Code:  LoadObjFieldN(fieldOffset: FieldOffset, size: OpWordCount)
        // Stack: [receiver, ...] -> [wordN-1, ..., word0, ...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // Pop the receiver.
        VMWord receiver = *--sp;
        // The destination starts at the ToS.
        VMWord* dst = sp;
        // Push N words.
        sp += size;

        if (isHeapAddress(receiver) == false)
        {
            // If the receiver is invalid, push zeros.
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        // Compute the field address from the receiver.
        HeapIndex index = getHeapPayload(receiver) + fieldOffset;
        // Copy the object field words into the pushed words.
        copyWords(dst, &mHeap.wordAt(index), size);

        VM_END_OP();
    }
    VM_OP(StoreObjFieldN)
    {
        // Code:  StoreObjFieldN(fieldOffset: FieldOffset, size: OpWordCount)
        // Stack: [wordN-1, ..., word0, receiver, ...] -> [...]
        FieldOffset fieldOffset = readPC<FieldOffset>(pc);
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The receiver is right below the value words.
        VMWord receiver = src[-1];
        // Pop the receiver and N words.
        sp = src - 1;

        if (isHeapAddress(receiver) == false)
        {
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), receiver);
            VM_END_OP_CHECKED();
        }

        // Compute the field address from the receiver.
        HeapIndex index = getHeapPayload(receiver) + fieldOffset;
        // Copy the value words into the object field.
        copyWords(&mHeap.wordAt(index), src, size);

        VM_END_OP();
    }
    VM_OP(LoadListElement)
    {
        // Code:  LoadListElement(size: OpWordCount)
        // Stack: [index, list, ...] -> [wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The destination starts at the list word.
        VMWord* dst = sp - 2;
        // The list is the first consumed word.
        VMWord list = dst[0];
        // The index is right above the list.
        i32 index = static_cast<i32>(dst[1]);
        // Push N words.
        sp = dst + size;

        VMWord* element = resolveListElementPayload(list, index, getRuntimeErrorAddress(pc, code));
        if (element == nullptr)
        {
            // If the list element is invalid, push zeros.
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        if (size == 1)
        {
            dst[0] = element[0];
        }
        else
        {
            // Copy the element words into the pushed words.
            copyWords(dst, element, size);
        }

        VM_END_OP();
    }
    VM_OP(StoreListElement)
    {
        // Code:  StoreListElement(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, index, list, ...] -> [...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The index is right below the value words.
        i32 index = static_cast<i32>(src[-1]);
        // The list is right below the index.
        VMWord list = src[-2];
        // Pop the list, index, and N words.
        sp = src - 2;

        VMWord* element = resolveListElementPayload(list, index, getRuntimeErrorAddress(pc, code));
        if (element == nullptr)
        {
            VM_END_OP_CHECKED();
        }

        if (size == 1)
        {
            element[0] = src[0];
        }
        else
        {
            // Copy the value words into the list element.
            copyWords(element, src, size);
        }

        VM_END_OP();
    }
    VM_OP(ListSize)
    {
        // Code:  ListSize()
        // Stack: [list, ...] -> [size, ...]
        VMWord list = sp[-1];

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp[-1] = VMWord{};
            VM_END_OP_CHECKED();
        }

        sp[-1] = block.mHeader->mLength;

        VM_END_OP();
    }
    VM_OP(ListIsEmpty)
    {
        // Code:  ListIsEmpty()
        // Stack: [list, ...] -> [bool, ...]
        VMWord list = sp[-1];

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp[-1] = VMWord{};
            VM_END_OP_CHECKED();
        }

        sp[-1] = (block.mHeader->mLength == 0) ? 1U : 0U;

        VM_END_OP();
    }
    VM_OP(ListPush)
    {
        // Code:  ListPush(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, list, ...] -> [...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The list is right below the value words.
        VMWord list = src[-1];

        // Sync the stack since the list and value words have to be live for GC.
        STACK_SYNC();

        // Make sure the list has enough room for one more element.
        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false ||
            ensureListCapacity(list, block, block.mHeader->mLength + 1U, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp = src - 1;
            VM_END_OP_CHECKED();
        }

        VMWord* element = block.getAppendSlot();
        if (size == 1)
        {
            element[0] = src[0];
        }
        else
        {
            // Copy the value words into the new element.
            copyWords(element, src, size);
        }

        // Grow the list length.
        ++block.mHeader->mLength;

        // Pop the list and N words.
        sp = src - 1;

        VM_END_OP();
    }
    VM_OP(ListAddList)
    {
        // Code:  ListAddList()
        // Stack: [source, list, ...] -> [...]
        // The list is below the source list.
        VMWord list = sp[-2];
        // The source list is on top.
        VMWord source = sp[-1];

        // Sync the stack since both list handles have to be live for GC.
        STACK_SYNC();

        // Get the destination and source list blocks.
        ListBlock block;
        ListBlock sourceBlock;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false ||
            getCheckedListBlock(source, sourceBlock, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp -= 2;
            VM_END_OP_CHECKED();
        }

        // Capture the old lengths before possible growth.
        u32 oldLength = block.mHeader->mLength;
        u32 sourceLength = sourceBlock.mHeader->mLength;
        if (sourceLength == 0)
        {
            sp -= 2;
            VM_END_OP();
        }

        // Check for overflow before adding the lengths.
        if (oldLength > std::numeric_limits<u32>::max() - sourceLength)
        {
            u64 requestedWords = std::numeric_limits<u64>::max();
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cAllocationFailed,
                               toRuntimeErrorByteCount(requestedWords));
            sp -= 2;
            VM_END_OP_CHECKED();
        }

        // Make sure the destination list has enough room for all source elements.
        u32 newLength = oldLength + sourceLength;
        if (ensureListCapacity(list, block, newLength, getRuntimeErrorAddress(pc, code)) == false ||
            getCheckedListBlock(source, sourceBlock, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp -= 2;
            VM_END_OP_CHECKED();
        }

        u32 elementWords = block.getElementWords();
        u32 wordCount = sourceLength * elementWords;
        // The destination starts at the old end of the destination list.
        VMWord* dst = block.getElement(oldLength);
        // The source starts at the first source list element.
        const VMWord* src = sourceBlock.getElement(0);

        // Copy the source words into the destination list.
        copyWords(dst, src, wordCount);
        // Grow the list length.
        block.mHeader->mLength = newLength;

        // Pop the destination and source lists.
        sp -= 2;

        VM_END_OP();
    }
    VM_OP(ListPop)
    {
        // Code:  ListPop(size: OpWordCount)
        // Stack: [list, ...] -> [wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The destination starts at the list word.
        VMWord* dst = sp - 1;
        // Push N words.
        sp = dst + size;
        // The list is the consumed word.
        VMWord list = dst[0];

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        // Empty lists cannot be popped.
        if (block.mHeader->mLength == 0)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cEmptyList);
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        u32 lastIndex = block.mHeader->mLength - 1U;
        VMWord* element = block.getElement(lastIndex);
        if (size == 1)
        {
            dst[0] = element[0];
        }
        else
        {
            copyWords(dst, element, size);
        }

        // The new size is the old last index.
        block.mHeader->mLength = lastIndex;

        VM_END_OP();
    }
    VM_OP(ListBack)
    {
        // Code:  ListBack(size: OpWordCount)
        // Stack: [list, ...] -> [wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The destination starts at the list word.
        VMWord* dst = sp - 1;
        // Push N words.
        sp = dst + size;
        // The list is the consumed word.
        VMWord list = dst[0];

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        // Empty lists have no back element.
        if (block.mHeader->mLength == 0)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cEmptyList);
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        u32 lastIndex = block.mHeader->mLength - 1U;
        VMWord* element = block.getElement(lastIndex);
        if (size == 1)
        {
            dst[0] = element[0];
        }
        else
        {
            // Copy the last element words into the pushed words.
            copyWords(dst, element, size);
        }

        VM_END_OP();
    }
    VM_OP(ListInsert)
    {
        // Code:  ListInsert(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, index, list, ...] -> [...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The index is right below the value words.
        i32 index = static_cast<i32>(src[-1]);
        // The list is right below the index.
        VMWord list = src[-2];

        // Sync the stack since the list and value words have to be live for GC.
        STACK_SYNC();

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp = src - 2;
            VM_END_OP_CHECKED();
        }

        // Check for out of bounds.
        // We allow inserting at the end, so the index check is for > instead of >=.
        if (index < 0 || static_cast<u32>(index) > block.mHeader->mLength)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cIndexOutOfBounds,
                               index,
                               block.mHeader->mLength);
            sp = src - 2;
            VM_END_OP_CHECKED();
        }

        // Make sure the list has enough room for one more element.
        u32 insertIndex = static_cast<u32>(index);
        if (ensureListCapacity(list, block, block.mHeader->mLength + 1U, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp = src - 2;
            VM_END_OP_CHECKED();
        }

        VMWord* element = block.insertSlot(insertIndex);

        // Copy the element in.
        if (size == 1)
        {
            element[0] = src[0];
        }
        else
        {
            copyWords(element, src, size);
        }

        // Pop the list, index, and N words.
        sp = src - 2;

        VM_END_OP();
    }
    VM_OP(ListRemoveAt)
    {
        // Code:  ListRemoveAt(size: OpWordCount)
        // Stack: [index, list, ...] -> [wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The destination starts at the list word.
        VMWord* dst = sp - 2;
        // Push N words.
        sp = dst + size;
        // The list is the first consumed word.
        VMWord list = dst[0];
        // The index is right above the list.
        i32 index = static_cast<i32>(dst[1]);

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        // Make sure the index is valid.
        if (index < 0 || static_cast<u32>(index) >= block.mHeader->mLength)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cIndexOutOfBounds,
                               index,
                               block.mHeader->mLength);
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        u32 removeIndex = static_cast<u32>(index);
        VMWord* element = block.getElement(removeIndex);

        // Copy the element to the stack.
        if (size == 1)
        {
            dst[0] = element[0];
        }
        else
        {
            copyWords(dst, element, size);
        }

        block.removeSlot(removeIndex);

        VM_END_OP();
    }
    VM_OP(ListIndexOf)
    {
        // Code:  ListIndexOf(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, list, ...] -> [index, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The value starts at the first value word.
        VMWord* value = sp - size;
        // The result overwrites the list word.
        VMWord* result = value - 1;
        // The list is right below the value words.
        VMWord list = value[-1];

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            result[0] = static_cast<VMWord>(-1);
            sp = value;
            VM_END_OP_CHECKED();
        }

        // Find the index and write the result.
        result[0] = static_cast<VMWord>(findListElement(block, value));

        // Pop N words.
        sp = value;

        VM_END_OP();
    }
    VM_OP(ListContains)
    {
        // Code:  ListContains(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, list, ...] -> [bool, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The value starts at the first value word.
        VMWord* value = sp - size;
        // The result overwrites the list word.
        VMWord* result = value - 1;
        // The list is right below the value words.
        VMWord list = value[-1];

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            result[0] = VMWord{};
            sp = value;
            VM_END_OP_CHECKED();
        }

        // Do the lookup and write the result.
        i32 index = findListElement(block, value);
        result[0] = (index >= 0 ? 1U : 0U);

        // Pop N words.
        sp = value;

        VM_END_OP();
    }
    VM_OP(ListClear)
    {
        // Code:  ListClear()
        // Stack: [list, ...] -> [...]
        VMWord list = *--sp;

        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            VM_END_OP_CHECKED();
        }

        block.clear();

        VM_END_OP();
    }
    VM_OP(ListReserve)
    {
        // Code:  ListReserve()
        // Stack: [capacity, list, ...] -> [...]
        i32 capacitySigned = static_cast<i32>(sp[-1]);
        VMWord list = sp[-2];

        if (capacitySigned < 0)
        {
            sp -= 2;
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cNegativeListCapacity,
                               capacitySigned);
            VM_END_OP_CHECKED();
        }

        // Sync the stack since the list handle has to be live for GC.
        STACK_SYNC();

        // Make sure the list has the requested capacity.
        ListBlock block;
        if (getCheckedListBlock(list, block, getRuntimeErrorAddress(pc, code)) == false ||
            ensureListCapacity(list, block, static_cast<u32>(capacitySigned), getRuntimeErrorAddress(pc, code)) ==
                false)
        {
            sp -= 2;
            VM_END_OP_CHECKED();
        }

        // Pop the stack.
        sp -= 2;

        VM_END_OP();
    }
    VM_OP(MapSize)
    {
        // Code:  MapSize()
        // Stack: [map, ...] -> [size, ...]
        VMWord map = sp[-1];

        MapBlock block;
        if (getCheckedMapBlock(map, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp[-1] = VMWord{};
            VM_END_OP_CHECKED();
        }

        sp[-1] = block.mHeader->mLength;

        VM_END_OP();
    }
    VM_OP(MapIsEmpty)
    {
        // Code:  MapIsEmpty()
        // Stack: [map, ...] -> [bool, ...]
        VMWord map = sp[-1];

        MapBlock block;
        if (getCheckedMapBlock(map, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            sp[-1] = VMWord{};
            VM_END_OP_CHECKED();
        }

        sp[-1] = block.mHeader->mLength == 0 ? 1U : 0U;

        VM_END_OP();
    }
    VM_OP(MapClear)
    {
        // Code:  MapClear()
        // Stack: [map, ...] -> [...]
        VMWord map = *--sp;

        MapBlock block;
        if (getCheckedMapBlock(map, block, getRuntimeErrorAddress(pc, code)) == false)
        {
            VM_END_OP_CHECKED();
        }

        block.clearBuckets();
        block.mHeader->mLength = 0;

        VM_END_OP();
    }
    VM_OP(MapContainsKey)
    {
        // Code:  MapContainsKey()
        // Stack: [key, map, ...] -> [bool, ...]
        // The result goes where the map address currently is.
        VMWord* result = sp - 2;
        // Get map and key.
        VMWord map = result[0];
        VMWord key = result[1];

        // Look up the block from the map handle, and then try to find the key.
        MapBlock block;
        VMAddress address = getRuntimeErrorAddress(pc, code);
        u32 hash = 0;
        u32 index = 0;
        bool found = false;
        if (getCheckedMapBlock(map, block, address) == false ||
            hashMapKey(block.mHeader->mTypeID, key, address, hash) == false ||
            findMapEntry(block, key, hash, address, index, found) == false)
        {
            result[0] = VMWord{};
            sp = result + 1;
            VM_END_OP_CHECKED();
        }

        // If we found the key, push 1, otherwise 0.
        result[0] = found ? 1U : 0U;

        // Pop the key (the map address was replaced with the result).
        sp = result + 1;

        VM_END_OP();
    }
    VM_OP(MapGet)
    {
        // Code:  MapGet(size: OpWordCount)
        // Stack: [key, map, ...] -> [wordN-1, ..., word0, ...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The result goes where the map address currently is.
        VMWord* dst = sp - 2;
        // Get map and key.
        VMWord map = dst[0];
        VMWord key = dst[1];
        // Push N words.
        sp = dst + size;

        // Look up the block from the map handle, and then try to find the key.
        MapBlock block;
        VMAddress address = getRuntimeErrorAddress(pc, code);
        u32 hash = 0;
        u32 index = 0;
        bool found = false;
        if (getCheckedMapBlock(map, block, address) == false ||
            hashMapKey(block.mHeader->mTypeID, key, address, hash) == false ||
            findMapEntry(block, key, hash, address, index, found) == false)
        {
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        // If we couldn't find the key, this is an error.
        if (found == false)
        {
            reportRuntimeError(address, RuntimeErrorKind::cMissingMapKey);
            std::memset(dst, 0, size * sizeof(VMWord));
            VM_END_OP_CHECKED();
        }

        // Otherwise, get the value from the bucket.
        VMWord* value = block.getValue(index);
        if (size == 1)
        {
            dst[0] = value[0];
        }
        else
        {
            copyWords(dst, value, size);
        }

        VM_END_OP();
    }
    VM_OP(MapSet)
    {
        // Code:  MapSet(size: OpWordCount)
        // Stack: [wordN-1, ..., word0, key, map, ...] -> [...]
        OpWordCount size = readPC<OpWordCount>(pc);

        // The source starts at the first value word.
        VMWord* src = sp - size;
        // The key and the map are right below that.
        VMWord key = src[-1];
        VMWord map = src[-2];

        // Sync the stack since the map, key, and value words have to be live for GC.
        STACK_SYNC();

        // Look up the block from the map handle, and then get the entry.
        MapBlock block;
        VMAddress address = getRuntimeErrorAddress(pc, code);
        u32 hash = 0;
        u32 index = 0;
        bool found = false;
        if (getCheckedMapBlock(map, block, address) == false ||
            hashMapKey(block.mHeader->mTypeID, key, address, hash) == false ||
            findMapEntry(block, key, hash, address, index, found) == false)
        {
            sp = src - 2;
            VM_END_OP_CHECKED();
        }

        // If we can't find the entry, we need to add a new one.
        if (found == false)
        {
            // Grow the map if needed, which may reallocate a new block to the handle.
            if (ensureMapCapacity(map, block, block.mHeader->mLength + 1U, address) == false ||
                findMapEntry(block, key, hash, address, index, found) == false)
            {
                sp = src - 2;
                VM_END_OP_CHECKED();
            }

            SIMLANG_ASSERTM(found == false && index < block.getBucketCapacity(),
                            "Map insertion must find an empty bucket.");
            block.insertEntry(index, hash, key);
        }

        // The destination is the entry value at the bucket.
        VMWord* dst = block.getValue(index);
        if (size == 1)
        {
            dst[0] = src[0];
        }
        else
        {
            copyWords(dst, src, size);
        }

        //  Pop everything.
        sp = src - 2;

        VM_END_OP();
    }
    VM_OP(MapRemove)
    {
        // Code:  MapRemove()
        // Stack: [key, map, ...] -> [bool, ...]
        // The result goes where the map address currently is.
        VMWord* result = sp - 2;
        // Get map and key.
        VMWord map = result[0];
        VMWord key = result[1];

        // Look up the block from the map handle, and then try to find the key.
        MapBlock block;
        VMAddress address = getRuntimeErrorAddress(pc, code);
        u32 hash = 0;
        u32 index = 0;
        bool found = false;
        if (getCheckedMapBlock(map, block, address) == false ||
            hashMapKey(block.mHeader->mTypeID, key, address, hash) == false ||
            findMapEntry(block, key, hash, address, index, found) == false)
        {
            result[0] = VMWord{};
            sp = result + 1;
            VM_END_OP_CHECKED();
        }

        // If we didn't find the key, then the result is false and we're done.
        if (found == false)
        {
            result[0] = VMWord{};
            sp = result + 1;
            VM_END_OP();
        }

        // Otherwise, remove the entry and close the probe cluster.
        block.removeEntry(index);

        // We found something.
        result[0] = 1U;

        // Pop the key, the map was overwritten with the result.
        sp = result + 1;

        VM_END_OP();
    }
    VM_OP(MapReserve)
    {
        // Code:  MapReserve()
        // Stack: [capacity, map, ...] -> [...]
        // Get capacity and map from the ToS.
        i32 capacitySigned = static_cast<i32>(sp[-1]);
        VMWord map = sp[-2];

        // If we have a negative capacity, that's an error.
        if (capacitySigned < 0)
        {
            sp -= 2;
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cNegativeMapCapacity,
                               capacitySigned);
            VM_END_OP_CHECKED();
        }

        // Sync the stack since we might garbage collect on alloc.
        STACK_SYNC();

        // Grow the map if needed.
        MapBlock block;
        if (getCheckedMapBlock(map, block, getRuntimeErrorAddress(pc, code)) == false ||
            ensureMapCapacity(map, block, static_cast<u32>(capacitySigned), getRuntimeErrorAddress(pc, code)) == false)
        {
            sp -= 2;
            VM_END_OP_CHECKED();
        }

        // Pop the capacity and map address.
        sp -= 2;

        VM_END_OP();
    }
    VM_OP(I2F)
    {
        // Code:  I2F()
        // Stack: [int, ...] -> [float, ...]
        i32 intValue = static_cast<i32>(sp[-1]);
        sp[-1] = bits::bitCast<VMWord>(static_cast<f32>(intValue));
        VM_END_OP();
    }
    VM_OP(F2I)
    {
        // Code:  F2I()
        // Stack: [float, ...] -> [int, ...]
        // Here, we try to make sure that the value fits into an i32.
        f32 f32Value = bits::bitCast<f32>(sp[-1]);
        i32 intValue = 0;
        if (checkedFloatToInt(f32Value, intValue) == false)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cInvalidCast);
            sp[-1] = 0;
            VM_END_OP_CHECKED();
        }

        // Replace the ToS with the result.
        sp[-1] = static_cast<VMWord>(intValue);

        VM_END_OP();
    }
    VM_OP(CheckCast)
    {
        // Code:  CheckCast(typeID: TypeID)
        // Stack: [object, ...] -> [object, ...]
        TypeID typeID = readPC<TypeID>(pc);

        // Get the object from the ToS.
        VMWord value = sp[-1];
        if (value == cNullRef)
        {
            // Casting null will always result in another null.
            VM_END_OP();
        }

        // If the address is invalid, bail.
        // We have to check the block here since this can be anything (even a list).
        Block block;
        if (mHeap.resolvePayloadAddress(value, block) == false)
        {
            reportReferenceRuntimeError(getRuntimeErrorAddress(pc, code), value);
            sp[-1] = cNullRef;
            VM_END_OP_CHECKED();
        }

        // If it's not an object and the target type is not the expected type, bail as well.
        if (block.mHeader->mBlockType != BlockType::cObject || block.mHeader->mTypeID != typeID)
        {
            i64 actualType =
                block.mHeader->mBlockType == BlockType::cObject ? static_cast<i64>(block.mHeader->mTypeID) : -1;
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cInvalidCast, actualType, typeID);
            sp[-1] = cNullRef;
            VM_END_OP_CHECKED();
        }

        VM_END_OP();
    }
    VM_OP(INeg)
    {
        // Code:  INeg()
        // Stack: [word, ...] -> [result, ...]
        sp[-1] = 0U - sp[-1];
        VM_END_OP();
    }
    VM_OP(INot)
    {
        // Code:  INot()
        // Stack: [word, ...] -> [result, ...]
        // We simply flip the bits of whatever is on top of the stack.
        sp[-1] = ~sp[-1];
        VM_END_OP();
    }
    VM_OP(IAdd)
    {
        // Code:  IAdd()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_WORD(+);
        VM_END_OP();
    }
    VM_OP(ISub)
    {
        // Code:  ISub()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_WORD(-);
        VM_END_OP();
    }
    VM_OP(IMul)
    {
        // Code:  IMul()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_WORD(*);
        VM_END_OP();
    }
    VM_OP(IDiv)
    {
        // Code:  IDiv()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        i32 rhs = static_cast<i32>(*--sp);
        i32 lhs = static_cast<i32>(sp[-1]);
        if (rhs == 0)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cDivisionByZero);
            sp[-1] = static_cast<VMWord>(std::numeric_limits<i32>::max());
            VM_END_OP_CHECKED();
        }

        i32 result;
        if (lhs == std::numeric_limits<i32>::min() && rhs == -1)
        {
            result = std::numeric_limits<i32>::min();
        }
        else
        {
            result = lhs / rhs;
        }

        sp[-1] = static_cast<VMWord>(result);

        VM_END_OP();
    }
    VM_OP(IMod)
    {
        // Code:  IMod()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        i32 rhs = static_cast<i32>(*--sp);
        i32 lhs = static_cast<i32>(sp[-1]);
        if (rhs == 0)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cDivisionByZero);
            sp[-1] = static_cast<VMWord>(std::numeric_limits<i32>::max());
            VM_END_OP_CHECKED();
        }

        i32 result;
        if (lhs == std::numeric_limits<i32>::min() && rhs == -1)
        {
            result = 0;
        }
        else
        {
            result = lhs % rhs;
        }

        sp[-1] = static_cast<VMWord>(result);

        VM_END_OP();
    }
    VM_OP(IAnd)
    {
        // Code:  IAnd()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_WORD(&);
        VM_END_OP();
    }
    VM_OP(IOr)
    {
        // Code:  IOr()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_WORD(|);
        VM_END_OP();
    }
    VM_OP(IXor)
    {
        // Code:  IXor()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_WORD(^);
        VM_END_OP();
    }
    VM_OP(IShL)
    {
        // Code:  IShL()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        u32 rhs = *--sp;
        u32 lhs = sp[-1];
        if (rhs >= 32)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cShiftOutOfRange, rhs);
            sp[-1] = 0;
            VM_END_OP_CHECKED();
        }
        sp[-1] = lhs << rhs;
        VM_END_OP();
    }
    VM_OP(IShR)
    {
        // Code:  IShR()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        u32 rhs = *--sp;
        u32 lhs = sp[-1];
        if (rhs >= 32)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cShiftOutOfRange, rhs);
            sp[-1] = 0;
            VM_END_OP_CHECKED();
        }
        sp[-1] = lhs >> rhs;
        VM_END_OP();
    }
    VM_OP(FNeg)
    {
        // Code:  FNeg()
        // Stack: [float, ...] -> [result, ...]
        // Confused? We're reading a f32 as an int, so we only have to flip the sign bit.
        sp[-1] ^= 1U << 31;
        VM_END_OP();
    }
    VM_OP(FAdd)
    {
        // Code:  FAdd()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_FLOAT(+);
        VM_END_OP();
    }
    VM_OP(FSub)
    {
        // Code:  FSub()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_FLOAT(-);
        VM_END_OP();
    }
    VM_OP(FMul)
    {
        // Code:  FMul()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_BINARY_FLOAT(*);
        VM_END_OP();
    }
    VM_OP(FDiv)
    {
        // Code:  FDiv()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        f32 rhs = bits::bitCast<f32>(*--sp);
        f32 lhs = bits::bitCast<f32>(sp[-1]);

        f32 result;
        if (rhs == 0.0f)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cDivisionByZero);
            sp[-1] = bits::bitCast<VMWord>(std::numeric_limits<f32>::max());
            VM_END_OP_CHECKED();
        }

        result = lhs / rhs;
        sp[-1] = bits::bitCast<VMWord>(result);

        VM_END_OP();
    }
    VM_OP(FMod)
    {
        // Code:  FMod()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        f32 rhs = bits::bitCast<f32>(*--sp);
        f32 lhs = bits::bitCast<f32>(sp[-1]);

        f32 result;
        if (rhs == 0.0f)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code), RuntimeErrorKind::cDivisionByZero);
            sp[-1] = bits::bitCast<VMWord>(std::numeric_limits<f32>::max());
            VM_END_OP_CHECKED();
        }

        f32 q = lhs / rhs;
        i32 n = static_cast<i32>(q);
        result = lhs - static_cast<f32>(n) * rhs;
        sp[-1] = bits::bitCast<VMWord>(result);

        VM_END_OP();
    }
    VM_OP(IEQ)
    {
        // Code:  IEQ()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_WORD(==);
        VM_END_OP();
    }
    VM_OP(INE)
    {
        // Code:  INE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_WORD(!=);
        VM_END_OP();
    }
    VM_OP(ILT)
    {
        // Code:  ILT()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_SIGNED(<);
        VM_END_OP();
    }
    VM_OP(ILE)
    {
        // Code:  ILE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_SIGNED(<=);
        VM_END_OP();
    }
    VM_OP(IGT)
    {
        // Code:  IGT()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_SIGNED(>);
        VM_END_OP();
    }
    VM_OP(IGE)
    {
        // Code:  IGE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_SIGNED(>=);
        VM_END_OP();
    }
    VM_OP(FEQ)
    {
        // Code:  FEQ()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_FLOAT(==);
        VM_END_OP();
    }
    VM_OP(FNE)
    {
        // Code:  FNE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_FLOAT(!=);
        VM_END_OP();
    }
    VM_OP(FLT)
    {
        // Code:  FLT()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_FLOAT(<);
        VM_END_OP();
    }
    VM_OP(FLE)
    {
        // Code:  FLE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_FLOAT(<=);
        VM_END_OP();
    }
    VM_OP(FGT)
    {
        // Code:  FGT()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_FLOAT(>);
        VM_END_OP();
    }
    VM_OP(FGE)
    {
        // Code:  FGE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VM_COMPARE_FLOAT(>=);
        VM_END_OP();
    }
    VM_OP(SEQ)
    {
        // Code:  SEQ()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VMWord rhs = *--sp;
        VMWord lhs = sp[-1];
        sp[-1] = stringsEqual(lhs, rhs, getRuntimeErrorAddress(pc, code)) ? 1U : 0U;
        VM_END_OP_CHECKED();
    }
    VM_OP(SNE)
    {
        // Code:  SNE()
        // Stack: [rhs, lhs, ...] -> [result, ...]
        VMWord rhs = *--sp;
        VMWord lhs = sp[-1];
        sp[-1] = stringsEqual(lhs, rhs, getRuntimeErrorAddress(pc, code)) ? 0U : 1U;
        VM_END_OP_CHECKED();
    }
    VM_OP(FormatString)
    {
        // Code:  FormatString(index: StringFormatIdx)
        // Stack: [argN-1, ..., arg0, ...] -> [string, ...]
        StringFormatIdx index = readPC<StringFormatIdx>(pc);

        // Get the format.
        const StringFormatTemplate& tmpl = mImage.mStringFormats[index];
        u32 argCount = static_cast<u32>(tmpl.mArgKinds.size());
        // Arguments stay on the stack during alloc so GC can still see them.
        u32 argBase = static_cast<u32>(sp - stackBase) - argCount;
        // First, compute the total byte output.
        u64 totalLen = tmpl.mLiteralByteCount;
        bool validFormat = true;

        // For this, we need to go over the args.
        for (u32 i = 0; i < argCount; ++i)
        {
            VMWord word = stackBase[argBase + i];
            StringFormatArgKind kind = tmpl.mArgKinds[i];
            switch (kind)
            {
                case StringFormatArgKind::cInt:
                {
                    totalLen += getIntFormatLen(static_cast<i32>(word));
                    break;
                }
                case StringFormatArgKind::cFloat:
                {
                    // Reinterpret the float from the stack.
                    f32 value = bits::bitCast<f32>(word);
                    totalLen += getFloatFormatLen(value);
                    break;
                }
                case StringFormatArgKind::cBool:
                {
                    totalLen += getBoolFormatLen(static_cast<u8>(word));
                    break;
                }
                case StringFormatArgKind::cString:
                {
                    // Look up the string from the ref.
                    std::string_view sv;
                    if (getCheckedStringView(word, sv, getRuntimeErrorAddress(pc, code)))
                    {
                        totalLen += sv.size();
                    }
                    else if (mHalted)
                    {
                        sp = stackBase + argBase;
                        *sp++ = cNullRef;

                        VM_END_OP_CHECKED();
                    }
                    break;
                }
                default:
                {
                    validFormat = false;
                    break;
                }
            }

            if (validFormat == false)
            {
                break;
            }
        }

        if (validFormat == false)
        {
            VM_END_OP();
        }

        if (totalLen > std::numeric_limits<u32>::max())
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cAllocationFailed,
                               toRuntimeErrorValue(totalLen));

            sp = stackBase + argBase;
            *sp++ = cNullRef;

            VM_END_OP_CHECKED();
        }

        u32 totalLen32 = static_cast<u32>(totalLen);

        // Sync the stack since the heap might need it up-to-date for GC.
        STACK_SYNC();

        // Now that we know the visible length, make the string.
        Heap::StringAlloc alloc = mHeap.allocateString(totalLen32, *this);
        if (alloc.mBytes == nullptr)
        {
            reportRuntimeError(getRuntimeErrorAddress(pc, code),
                               RuntimeErrorKind::cAllocationFailed,
                               toRuntimeErrorValue(totalLen32));

            sp = stackBase + argBase;
            *sp++ = cNullRef;

            VM_END_OP_CHECKED();
        }

        // Get the buffer.
        char* dst = alloc.mBytes;

        // Append to the string and return the new position.
        auto appendStringValue = [&](VMWord value) -> char*
        {
            std::string_view sv;
            if (getUncheckedStringView(value, sv) == false || sv.empty())
            {
                return dst;
            }

            u32 byteCount = static_cast<u32>(sv.size());
            std::memcpy(dst, sv.data(), byteCount);

            return dst + byteCount;
        };

        // String literals need to be converted to their static string value first.
        auto appendStringLiteral = [&](StringLiteralIdx literalIndex) -> char*
        {
            return appendStringValue(makeStaticStringHandle(literalIndex));
        };

        // Go again, this time filling it.
        for (u32 i = 0; i < argCount; ++i)
        {
            // We have an alternating sequence of strings and values.
            // String first.
            dst = appendStringLiteral(tmpl.mLiteralIndices[i]);

            // Now the value.
            VMWord word = stackBase[argBase + i];

            // All of these write and then return the new position of the string.
            StringFormatArgKind kind = tmpl.mArgKinds[i];
            switch (kind)
            {
                case StringFormatArgKind::cInt:
                {
                    dst = writeIntChars(dst, static_cast<i32>(word));
                    break;
                }
                case StringFormatArgKind::cFloat:
                {
                    // The usual reinterpret.
                    f32 value = bits::bitCast<f32>(word);
                    dst = writeFloatChars(dst, value);
                    break;
                }
                case StringFormatArgKind::cBool:
                {
                    dst = writeBoolChars(dst, static_cast<u8>(word));
                    break;
                }
                case StringFormatArgKind::cString:
                {
                    dst = appendStringValue(word);
                    break;
                }
                default:
                {
                    validFormat = false;
                    break;
                }
            }

            if (validFormat == false)
            {
                break;
            }
        }

        if (validFormat)
        {
            // Closing string.
            appendStringLiteral(tmpl.mLiteralIndices.back());
            // Null-terminator.
            alloc.mBytes[totalLen32] = '\0';
            // Pop the stuff on the stack.
            sp = stackBase + argBase;
            // Push it.
            *sp++ = alloc.mRef;
        }

        VM_END_OP();
    }
    VM_OP(Print)
    {
        // Code:  Print(kind: u8)
        // Stack: [value, ...] -> [...]
        u8 rawKind = readPC<u8>(pc);

        auto kind = static_cast<StringFormatArgKind>(rawKind);
        switch (kind)
        {
            case StringFormatArgKind::cInt:
            {
                // Pop the int from the stack and write it to the output.
                i32 value = static_cast<i32>(*--sp);
                if (mOutput != nullptr)
                {
                    TextWriter out{*mOutput};
                    out << value;
                    out.newline();
                }
                break;
            }
            case StringFormatArgKind::cFloat:
            {
                // Reinterpret the float from the stack.
                f32 value = bits::bitCast<f32>(*--sp);
                if (mOutput != nullptr)
                {
                    TextWriter out{*mOutput};
                    char buffer[64];
                    int n = std::snprintf(buffer, sizeof(buffer), "%f", value);
                    if (n > 0)
                    {
                        // If we exceed the buffer size, truncate the string.
                        usize textSize = static_cast<usize>(n);
                        if (textSize >= sizeof(buffer))
                        {
                            textSize = sizeof(buffer) - 1;
                        }
                        out.write(std::string_view{buffer, textSize});
                    }
                    out.newline();
                }
                break;
            }
            case StringFormatArgKind::cBool:
            {
                // Pop the bool from the stack and write it to the output.
                u8 value = static_cast<u8>(*--sp);
                if (mOutput != nullptr)
                {
                    TextWriter out{*mOutput};
                    out.write(value ? "true" : "false");
                    out.newline();
                }
                break;
            }
            case StringFormatArgKind::cString:
            {
                // Pop the string ref from the stack.
                VMWord ref = *--sp;
                std::string_view sv;
                // Look up the string from the ref.
                bool validString = getCheckedStringView(ref, sv, getRuntimeErrorAddress(pc, code));
                if (mOutput != nullptr)
                {
                    TextWriter out{*mOutput};
                    if (validString)
                    {
                        out.write(sv);
                        out.newline();
                    }
                    else if (mHalted == false)
                    {
                        out.write("<invalid string>");
                        out.newline();
                    }
                }

                if (validString == false)
                {
                    VM_END_OP_CHECKED();
                }
                break;
            }
            default:
            {
                break;
            }
        }

        VM_END_OP();
    }
    VM_OP(Label)
    {
        // Code:  Label(index: u32)
        // Stack: [...] -> [...]
        SIMLANG_BREAK("Trying to execute label instruction at runtime!");
        VM_END_OP();
    }

    // End label.
    VM_DISPATCH_END();

    // Sync one last time before we return.
    STACK_SYNC();

    return mRuntimeErrorCount == 0;

#undef VM_COMPARE_FLOAT
#undef VM_BINARY_FLOAT
#undef VM_COMPARE_SIGNED
#undef VM_COMPARE_WORD
#undef VM_BINARY_WORD
#undef STACK_RELOAD
#undef STACK_SYNC
#undef VM_DISPATCH_END
#undef VM_DISPATCH
#undef VM_DISPATCH_START
#undef VM_HALT
#undef VM_END_OP_CHECKED
#undef VM_END_OP
#undef VM_OP
#undef VM_END_LABEL
#undef VM_LABEL
#undef VM_USE_GOTO
}

} // namespace simlang
