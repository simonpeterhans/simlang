#include "runtime/memory/heap.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "runtime/memory/typelayout.h"
#include "runtime/memory/typelayouttable.h"
#include "runtime/typeids.h"
#include "util/asserts.h"

namespace simlang
{

static constexpr u32 cMinPayloadWords = 1;
static constexpr u32 cMinBlockWords = cBlockHeaderWords + cMinPayloadWords;

static constexpr f32 cMaxHeapGrowthFactor = 4.0f;
static constexpr f32 cMaxGCGrowthFactor = 16.0f;
static constexpr u64 cMinMapBucketCapacity = 4;

static bool isHeapHandleWord(VMWord w)
{
    return (w & cHeapKindMask) == cHeapHandleTag;
}

static bool isHeapAddressWord(VMWord w)
{
    return (w & cHeapKindMask) == cHeapAddressTag;
}

static HeapIndex heapPayloadFromWord(VMWord w)
{
    return (w & cHeapPayloadMask);
}

static u64 getListPayloadWords(u32 elementWords, u32 capacity)
{
    return static_cast<u64>(elementWords) * capacity;
}

static u32 getMapBucketWords(u32 valueWords)
{
    return cMapControlWords + cMapKeyWords + valueWords;
}

static u64 getMapPayloadWords(u32 valueWords, u32 usedCapacity)
{
    // Get the total number of buckets we need for the requested entry capacity.
    // As of now, that is basically 2 times the number of entries that we want to use.
    u64 bucketCapacity = std::max(static_cast<u64>(usedCapacity * 2U), cMinMapBucketCapacity);

    // Get the size of each bucket in words (control + key + value words).
    u64 bucketWords = getMapBucketWords(valueWords);

    return bucketCapacity * bucketWords;
}

Heap::Heap(HeapOptions options)
    : mOptions(options)
{
    // Sanitize options (we could also assert or warn here).
    mOptions.mMaxHeapWords = std::clamp(mOptions.mMaxHeapWords, cMinBlockWords, cMaxHeapWords);
    mOptions.mMinHeapGrowthWords = std::clamp(mOptions.mMinHeapGrowthWords, cMinBlockWords, mOptions.mMaxHeapWords);
    mOptions.mHeapGrowthFactor = std::clamp(mOptions.mHeapGrowthFactor, 1.0f, cMaxHeapGrowthFactor);
    mOptions.mMinGCThresholdWords = std::clamp(mOptions.mMinGCThresholdWords, 1U, mOptions.mMaxHeapWords);
    mOptions.mGCThresholdFactor = std::clamp(mOptions.mGCThresholdFactor, 1.0f, cMaxGCGrowthFactor);

    // Initialize the next GC threshold.
    mNextGCThresholdWords = mOptions.mMinGCThresholdWords;

    // If we have no initial alloc, we're done.
    if (mOptions.mInitialHeapWords == 0)
    {
        return;
    }

    // Otherwise, create the initial free block with the requested size.
    u32 initialHeapWords = std::min(mOptions.mInitialHeapWords, mOptions.mMaxHeapWords);
    if (initialHeapWords < cMinBlockWords)
    {
        initialHeapWords = cMinBlockWords;
    }

    appendFreeBlock(initialHeapWords);
}

VMWord Heap::allocateObject(TypeID typeID, u32 dataWords, const RootProvider& roots)
{
    HeapIndex base = alloc(BlockType::cObject, typeID, dataWords, 0, 0, roots, true);
    if (base == cInvalidHeapIndex)
    {
        return cNullRef;
    }

    // Return the heap address to the payload (not the block header!).
    return cHeapAddressTag | payloadIndexFromBase(base);
}

Heap::StringAlloc Heap::allocateString(u32 byteLen, const RootProvider& roots)
{
    // Allocate stuff.
    HeapIndex base = alloc(BlockType::cString, 0, StringBlock::getPayloadFromBytes(byteLen), byteLen, 0, roots, false);
    if (base == cInvalidHeapIndex)
    {
        return {};
    }

    // Here, we also return the heap address to the payload.
    StringAlloc ret;
    ret.mRef = cHeapAddressTag | payloadIndexFromBase(base);
    ret.mBytes = StringBlock::getBytes(payloadFromBase(base));
    return ret;
}

Heap::ListAlloc Heap::allocateList(TypeID elemTypeID, u32 elemWords, u32 capacity, const RootProvider& roots)
{
    // We need 1 data word for GC stuff.
    elemWords = std::max(elemWords, 1U);

    u64 payloadWords = getListPayloadWords(elemWords, capacity);

    // Allocate stuff.
    HeapIndex base = alloc(BlockType::cList, elemTypeID, payloadWords, 0, elemWords, roots, true);
    if (base == cInvalidHeapIndex)
    {
        return {};
    }

    // Here, we make a stable handle for the list in case the underlying memory is reallocated as the list grows.
    VMWord ref = makeHandle(base);
    if (ref == cNullRef)
    {
        return {};
    }

    ListAlloc ret;
    ret.mRef = ref;
    ret.mBlock = ListBlock{&headerFromBase(base), payloadFromBase(base)};
    return ret;
}

bool Heap::resizeList(VMWord handle, u32 capacity, const RootProvider& roots)
{
    ListBlock oldBlock;
    HeapIndex oldBase = cInvalidHeapIndex;
    if (resolveListHandle(handle, oldBase, oldBlock) == false)
    {
        return false;
    }

    // Copy the fields needed after allocation, since allocation may invalidate block pointers.
    u32 oldLength = oldBlock.mHeader->mLength;
    TypeID elementTypeID = oldBlock.mHeader->mTypeID;
    u32 elementWords = oldBlock.getElementWords();

    // No shrinking, require at least as many elements as we had before for now.
    capacity = std::max(capacity, oldLength);

    // If we already have enough storage, we don't have to do anything and we're done.
    if (oldBlock.getCapacity() >= capacity)
    {
        return true;
    }

    u64 payloadWords = getListPayloadWords(elementWords, capacity);

    // Create the new block with the desired capacity and the old length, and zero everything.
    HeapIndex newBase = alloc(BlockType::cList, elementTypeID, payloadWords, oldLength, elementWords, roots, true);
    if (newBase == cInvalidHeapIndex)
    {
        return false;
    }

    // If we have some old data, copy that in.
    if (oldLength != 0)
    {
        std::memcpy(payloadFromBase(newBase),
                    payloadFromBase(oldBase),
                    static_cast<usize>(oldLength) * elementWords * sizeof(VMWord));
    }

    // Update the handle to point to the new base.
    HeapIndex handleIndex = heapPayloadFromWord(handle);
    mHandles[handleIndex] = newBase;

    return true;
}

Heap::MapAlloc Heap::allocateMap(TypeID keyTypeID,
                                 TypeID valueTypeID,
                                 u32 valueWords,
                                 u32 capacity,
                                 const RootProvider& roots)
{
    // We need 1 data word for GC stuff.
    valueWords = std::max(valueWords, 1U);

    u64 payloadWords = getMapPayloadWords(valueWords, capacity);

    // Allocate stuff.
    HeapIndex base = alloc(BlockType::cMap, keyTypeID, payloadWords, 0, valueWords, roots, true, valueTypeID);
    if (base == cInvalidHeapIndex)
    {
        return {};
    }

    // Here, we make a stable handle for the map in case the underlying memory is reallocated as the map grows.
    VMWord ref = makeHandle(base);
    if (ref == cNullRef)
    {
        return {};
    }

    MapAlloc ret;
    ret.mRef = ref;
    ret.mBlock = MapBlock{&headerFromBase(base), payloadFromBase(base)};
    return ret;
}

bool Heap::resizeMap(VMWord handle, u32 capacity, const RootProvider& roots)
{
    MapBlock oldBlock;
    HeapIndex oldBase = cInvalidHeapIndex;
    if (resolveMapHandle(handle, oldBase, oldBlock) == false)
    {
        return false;
    }

    // Copy the fields needed after allocation, since allocation may invalidate block pointers.
    u32 oldLength = oldBlock.mHeader->mLength;
    TypeID keyTypeID = oldBlock.mHeader->mTypeID;
    TypeID valueTypeID = oldBlock.mHeader->getMapValueTypeID();
    u32 valueWords = oldBlock.mHeader->getMapValueWords();

    // No shrinking, require at least as many elements as we had before for now.
    capacity = std::max(capacity, oldLength);

    // If we already have enough storage, we don't have to do anything and we're done.
    if (oldBlock.getEntryCapacity() >= capacity)
    {
        return true;
    }

    u64 payloadWords = getMapPayloadWords(valueWords, capacity);

    // Create the new block with the desired capacity and the old length, and zero everything.
    HeapIndex newBase =
        alloc(BlockType::cMap, keyTypeID, payloadWords, oldLength, valueWords, roots, true, valueTypeID);
    if (newBase == cInvalidHeapIndex)
    {
        return false;
    }

    // If we have some old data, rehash it into the new bucket array.
    if (oldLength != 0)
    {
        oldBlock = MapBlock{&headerFromBase(oldBase), payloadFromBase(oldBase)};
        MapBlock newBlock{&headerFromBase(newBase), payloadFromBase(newBase)};
        oldBlock.copyOccupiedBucketsTo(newBlock);
    }

    // Update the handle to point to the new base.
    HeapIndex handleIndex = heapPayloadFromWord(handle);
    mHandles[handleIndex] = newBase;

    return true;
}

bool Heap::resolvePayloadAddress(VMWord address, Block& out)
{
    out = Block{};

    // If this is not a heap address, bail.
    if (isHeapAddressWord(address) == false)
    {
        return false;
    }

    // Get the heap index from the address.
    HeapIndex payloadIndex = heapPayloadFromWord(address);
    if (payloadIndex < cBlockHeaderWords || payloadIndex >= mOwner.size())
    {
        return false;
    }

    // Get the base from the payload index.
    HeapIndex base = payloadIndex - cBlockHeaderWords;
    if (mOwner[payloadIndex] != base)
    {
        return false;
    }

    // Get the block, make sure it's valid.
    BlockHeader& header = headerFromBase(base);
    if (header.mBlockType == BlockType::cFree)
    {
        return false;
    }

    // Write the header and payload pointers.
    out.mHeader = &header;
    out.mPayload = payloadFromBase(base);

    return true;
}

bool Heap::resolveHandle(VMWord handle, Block& out)
{
    out = Block{};

    HeapIndex base = cInvalidHeapIndex;
    if (resolveHandleToBase(handle, base) == false)
    {
        return false;
    }

    // Write the header and payload pointers.
    BlockHeader& header = headerFromBase(base);
    out.mHeader = &header;
    out.mPayload = payloadFromBase(base);

    return true;
}

bool Heap::resolveListHandle(VMWord handle, ListBlock& out)
{
    HeapIndex base = cInvalidHeapIndex;
    return resolveListHandle(handle, base, out);
}

bool Heap::resolveMapHandle(VMWord handle, MapBlock& out)
{
    HeapIndex base = cInvalidHeapIndex;
    return resolveMapHandle(handle, base, out);
}

bool Heap::resolveListHandle(VMWord handle, HeapIndex& outBase, ListBlock& out)
{
    outBase = cInvalidHeapIndex;
    out = ListBlock{};

    // Resolve the handle to a base index.
    HeapIndex base = cInvalidHeapIndex;
    if (resolveHandleToBase(handle, base) == false)
    {
        return false;
    }

    // Get the block header at that index and check the type.
    BlockHeader& header = headerFromBase(base);
    if (header.mBlockType != BlockType::cList)
    {
        return false;
    }

    // Make a list block view out of it.
    outBase = base;
    out = ListBlock{&header, payloadFromBase(base)};

    return true;
}

bool Heap::resolveMapHandle(VMWord handle, HeapIndex& outBase, MapBlock& out)
{
    outBase = cInvalidHeapIndex;
    out = MapBlock{};

    // Resolve the handle to a base index.
    HeapIndex base = cInvalidHeapIndex;
    if (resolveHandleToBase(handle, base) == false)
    {
        return false;
    }

    // Get the block header at that index and check the type.
    BlockHeader& header = headerFromBase(base);
    if (header.mBlockType != BlockType::cMap)
    {
        return false;
    }

    // Make a map block view out of it.
    outBase = base;
    out = MapBlock{&header, payloadFromBase(base)};

    return true;
}

void Heap::collect(const Roots& roots)
{
    if (mData.empty())
    {
        return;
    }

    // Mark stuff starting from the roots.
    markFromRoots(roots);

    // Sweep and merge stuff, get the number of live words.
    u32 liveWords = sweepAndMerge();

    // Drop handle table entries whose target block was collected.
    sweepHandles();

    // Rebuild the free list.
    rebuildFreeList();

    // Prepare the params for the next GC.
    mAllocWordsSinceGC = 0;

    // Find out after how many allocated words we do the next GC pass.
    // We make this depend on the live words and the configured growth factor.
    // In theory we don't have to clamp by mMaxHeapWords since alloc will try that anyway if nothing is free.
    f64 target =
        std::min(static_cast<f64>(liveWords) * mOptions.mGCThresholdFactor, static_cast<f64>(mOptions.mMaxHeapWords));
    // Require at least mMinGCThresholdWords (to prevent excessive GC for smaller heaps).
    mNextGCThresholdWords = std::max(static_cast<u32>(target), mOptions.mMinGCThresholdWords);
}

bool Heap::resolveHandleToBase(VMWord handle, HeapIndex& outBase) const
{
    outBase = cInvalidHeapIndex;

    // If this is not a heap handle, bail.
    if (isHeapHandleWord(handle) == false)
    {
        return false;
    }

    // Get the payload from the word (which is the handle index).
    HeapIndex handleIndex = heapPayloadFromWord(handle);
    if (handleIndex >= mHandles.size())
    {
        return false;
    }

    // Look up the base from the handle index.
    HeapIndex base = mHandles[handleIndex];
    if (base >= mData.size())
    {
        return false;
    }

    // Get the block, make sure it's not free.
    const BlockHeader& header = headerFromBase(base);
    if (header.mBlockType == BlockType::cFree)
    {
        return false;
    }

    outBase = base;

    return true;
}

HeapIndex Heap::alloc(BlockType t,
                      TypeID typeID,
                      u64 requestedPayloadWords,
                      u32 length,
                      u32 elementWords,
                      const RootProvider& roots,
                      bool zero,
                      TypeID mapValueTypeID)
{
    // If we requested more than the maximum payload size, bail.
    u32 maxPayloadWords = mOptions.mMaxHeapWords - cBlockHeaderWords;
    if (requestedPayloadWords > maxPayloadWords)
    {
        return cInvalidHeapIndex;
    }

    // Make sure we don't exceed the maximum element size allowed (which really shouldn't happen).
    if (elementWords > BlockHeader::cElementInfoWordCountMask)
    {
        return cInvalidHeapIndex;
    }

    // We enforce at least 1 word of payload.
    u32 payloadWords = static_cast<u32>(std::max(requestedPayloadWords, static_cast<u64>(cMinPayloadWords)));

    // The total size is the header size plus the payload size.
    u32 totalWords = cBlockHeaderWords + payloadWords;

    // If we're beyond the threshold for collection, collect.
    bool collected = static_cast<u64>(mAllocWordsSinceGC) + totalWords > mNextGCThresholdWords;
    if (collected)
    {
        collect(roots.makeRoots());
    }

    // Then, try to find some free block we can use.
    u32 allocatedWords = 0;
    HeapIndex base = tryAllocFromFreeList(totalWords, allocatedWords);
    if (base == cInvalidHeapIndex)
    {
        // If we couldn't find a free block, try to grow the heap and allocate from the new free block.
        if (growHeap(totalWords))
        {
            base = tryAllocFromFreeList(totalWords, allocatedWords);
        }
    }

    // If we failed to allocate and haven't collected yet, we need to collect and then do a last try.
    if (base == cInvalidHeapIndex && collected == false)
    {
        collect(roots.makeRoots());
        base = tryAllocFromFreeList(totalWords, allocatedWords);
        if (base == cInvalidHeapIndex)
        {
            if (growHeap(totalWords))
            {
                base = tryAllocFromFreeList(totalWords, allocatedWords);
            }
        }
    }

    // If we haven't found anything meaningful here, we failed.
    if (base == cInvalidHeapIndex)
    {
        return cInvalidHeapIndex;
    }

    // If we're here, we're good to go and can write the header and owner range.
    writeHeader(base, t, typeID, allocatedWords, length, elementWords, mapValueTypeID);
    setOwnerRange(base, allocatedWords);

    if (zero)
    {
        // Get the payload pointer.
        VMWord* dst = payloadFromBase(base);
        // Find out how many words that block is (minus the block header words).
        u32 allocatedPayloadWords = allocatedWords - cBlockHeaderWords;
        // Finally zero it.
        std::memset(dst, 0, allocatedPayloadWords * sizeof(VMWord));
    }

    // Don't forget to tell GC that we allocated some more stuff.
    // Note that this does not mean that the heap grew.
    u64 allocatedSinceGC = static_cast<u64>(mAllocWordsSinceGC) + allocatedWords;
    // Make sure we don't overflow.
    mAllocWordsSinceGC = (allocatedSinceGC > std::numeric_limits<u32>::max()) ? std::numeric_limits<u32>::max()
                                                                              : static_cast<u32>(allocatedSinceGC);

    return base;
}

HeapIndex Heap::tryAllocFromFreeList(u32 requestedWords, u32& allocatedWords)
{
    allocatedWords = 0;

    // Find out if we have some free stuff we can use.
    HeapIndex prev = cInvalidHeapIndex;
    HeapIndex curr = mFreeListHead;

    while (curr != cInvalidHeapIndex)
    {
        // Get the current block.
        BlockHeader& h = headerFromBase(curr);

        // If the block is large enough for what we requested, take it.
        if (h.mSize >= requestedWords)
        {
            u32 freeBlockWords = h.mSize;

            // We don't split the block if it is too small to create another header from it.
            u32 remainingWords = freeBlockWords - requestedWords;
            if (remainingWords < cMinBlockWords)
            {
                // Take the entire current block.
                // Link the next free block to the previous free block.
                HeapIndex next = getNextFreeBlock(curr);
                if (prev == cInvalidHeapIndex)
                {
                    mFreeListHead = next;
                }
                else
                {
                    setNextFreeBlock(prev, next);
                }

                // This is more than we requested, since the block we're taking was too small to split.
                allocatedWords = freeBlockWords;
                return curr;
            }

            // Otherwise, shrink the current block to the remaining words (and leave it untouched otherwise).
            h.mSize = remainingWords;
            // The address right after the shrunken current block is the start of the block we will use.
            HeapIndex allocBase = curr + remainingWords;
            // The allocated words are exactly what we requested.
            allocatedWords = requestedWords;
            return allocBase;
        }

        prev = curr;
        curr = getNextFreeBlock(curr);
    }

    return cInvalidHeapIndex;
}

bool Heap::growHeap(u32 minWords)
{
    // Note that minWords here includes the block's header, so the caller needs to account for that.
    // Get the current size and find out if we have enough remaining capacity.
    usize oldSize = mData.size();
    usize remainingCapacity = mOptions.mMaxHeapWords - oldSize;
    if (minWords > remainingCapacity)
    {
        return false;
    }

    // Grow by at least minWords or whatever we specified in the options.
    usize minGrowthWords = std::max(static_cast<usize>(minWords), static_cast<usize>(mOptions.mMinHeapGrowthWords));
    // Also grow by the configured factor.
    // Note that this expects a growth factor of at least 1 (!).
    usize factoredGrowthWords = static_cast<usize>(static_cast<f64>(oldSize) * (mOptions.mHeapGrowthFactor - 1.0f));
    // Pick whatever is larger of the minimum growth and the factored growth.
    usize growthWords = std::max(minGrowthWords, factoredGrowthWords);
    // But don't grow beyond the maximum heap size.
    growthWords = std::min(growthWords, remainingCapacity);

    return appendFreeBlock(static_cast<u32>(growthWords));
}

bool Heap::appendFreeBlock(u32 words)
{
    // The block needs to at least fit the header and 1 word of payload.
    if (words < cMinBlockWords)
    {
        return false;
    }

    // Make sure we have enough capacity.
    usize oldSize = mData.size();
    if (oldSize >= mOptions.mMaxHeapWords || words > mOptions.mMaxHeapWords - oldSize)
    {
        return false;
    }

    // Grow our data.
    usize newSize = oldSize + words;
    mData.resize(newSize);
    // Also grow the owner data.
    mOwner.resize(newSize, cInvalidHeapIndex);

    // We now create a new block starting from the end of the current heap index (== oldSize).
    HeapIndex growBase = static_cast<HeapIndex>(oldSize);
    // Write the header as a free block.
    writeHeader(growBase, BlockType::cFree, 0, words, 0, 0);
    // Link the new free block to the current free list head.
    setNextFreeBlock(growBase, mFreeListHead);
    // The new block becomes the free list head.
    mFreeListHead = growBase;

    return true;
}

void Heap::writeHeader(HeapIndex base,
                       BlockType t,
                       TypeID typeID,
                       u32 totalWords,
                       u32 length,
                       u32 elementWords,
                       TypeID mapValueTypeID)
{
    // Get the header address and write the data to it.
    SIMLANG_ASSERTM(elementWords <= BlockHeader::cElementInfoWordCountMask,
                    "Heap element size does not fit packed header.");

    BlockHeader& h = headerFromBase(base);
    h.mBlockType = t;
    h.mTypeID = typeID;
    h.mSize = totalWords;
    h.mLength = length;
    h.mMarked = false;
    switch (t)
    {
        case BlockType::cList:
        {
            h.setListElementInfo(elementWords);
            break;
        }
        case BlockType::cMap:
        {
            h.setMapValueInfo(elementWords, mapValueTypeID);
            break;
        }
        default:
        {
            h.clearElementInfo();
            break;
        }
    }
}

void Heap::setOwnerRange(HeapIndex base, u32 totalWords)
{
    for (HeapIndex i = base; i < base + totalWords; ++i)
    {
        mOwner[i] = base;
    }
}

VMWord Heap::makeHandle(HeapIndex base)
{
    HeapIndex handleIndex;

    if (mFreeHandles.empty())
    {
        // If we have no free handles, allocate a new one.
        handleIndex = static_cast<HeapIndex>(mHandles.size());
        if (handleIndex > cMaxHeapHandleIndex)
        {
            return cNullRef;
        }

        mHandles.push_back(base);
    }
    else
    {
        // Otherwise, use the last one in the free list.
        handleIndex = mFreeHandles.back();
        mFreeHandles.pop_back();
        mHandles[handleIndex] = base;
    }

    return cHeapHandleTag | handleIndex;
}

void Heap::markFromRoots(const Roots& roots)
{
    mMarkStack.clear();

    auto markWords = [&](ArrayView<const VMWord> words)
    {
        for (VMWord word : words)
        {
            markValue(word);
        }
    };

    // Go through all the globals, the stack, and temp roots (host-related, e.g. syscall-created objects).
    // This is currently quite conservative by picking up any value with MSB == 1.
    // As a result, we might be keeping some false positives later on, but that is okay for now.
    // Later on, we should improve this by using stack maps to identify what we really have to scan.
    markWords(roots.mGlobals);
    markWords(roots.mStack);
    markWords(roots.mTemp);

    // Process all the stuff we marked.
    // This is practically graph traversal as we're following references until we're done.
    while (mMarkStack.empty() == false)
    {
        HeapIndex base = mMarkStack.back();
        mMarkStack.pop_back();
        scanBlockPrecise(base);
    }
}

void Heap::markValue(VMWord maybeHeapAddr)
{
    HeapIndex base = cInvalidHeapIndex;

    if (isHeapHandleWord(maybeHeapAddr))
    {
        if (resolveHandleToBase(maybeHeapAddr, base) == false)
        {
            return;
        }
    }
    else if (isHeapAddressWord(maybeHeapAddr))
    {
        // Check if the address range could be valid.
        HeapIndex idx = heapPayloadFromWord(maybeHeapAddr);
        if (idx >= mOwner.size())
        {
            return;
        }

        // Find the base address of the block.
        base = mOwner[idx];
        if (base == cInvalidHeapIndex)
        {
            return;
        }
    }
    else
    {
        return;
    }

    // If this is a free block, this is not a live heap value.
    BlockHeader& header = headerFromBase(base);
    if (header.mBlockType == BlockType::cFree)
    {
        return;
    }

    // If it's already marked, we're done.
    if (header.mMarked)
    {
        return;
    }

    // Otherwise, mark it as used and push it to follow it.
    header.mMarked = true;
    mMarkStack.push_back(base);
}

void Heap::scanBlockPrecise(HeapIndex base)
{
    // Get the block header.
    BlockHeader& header = headerFromBase(base);

    if (header.mBlockType == BlockType::cString)
    {
        // Strings contain no heap references.
        return;
    }

    // Get the payload.
    VMWord* payload = payloadFromBase(base);

    if (header.mBlockType == BlockType::cObject)
    {
        // Handle struct/class stuff.
        if (mTypeLayoutTable == nullptr || mTypeLayoutTable->hasLayout(header.mTypeID) == false)
        {
            return;
        }

        const TypeLayout& ti = mTypeLayoutTable->getLayout(header.mTypeID);
        TypeLayoutRefCount refCount = ti.getRefCount();
        if (refCount == 0)
        {
            // If the type has no references to follow, we can bail early.
            return;
        }

        // Otherwise, we need to look them up and follow them.
        TypeLayoutRefOffsetIndex refOffsetStart = ti.getRefOffsetStartIndex();
        if (mTypeLayoutTable->hasRefOffsetRange(refOffsetStart, refCount) == false)
        {
            return;
        }

        for (u32 i = 0; i < refCount; ++i)
        {
            // Get the offset from the array of offsets.
            TypeLayoutRefOffset offset = mTypeLayoutTable->getRefOffset(refOffsetStart + i);
            // Mark it accordingly in the block.
            markValue(payload[offset]);
        }

        return;
    }

    if (header.mBlockType == BlockType::cList)
    {
        ListBlock list{&header, payload};
        const u32 len = header.mLength;
        if (len == 0)
        {
            return;
        }

        if (mTypeLayoutTable == nullptr || mTypeLayoutTable->hasLayout(header.mTypeID) == false)
        {
            return;
        }

        const TypeLayout& ti = mTypeLayoutTable->getLayout(header.mTypeID);
        if (ti.getKind() == TypeLayout::Kind::cReference)
        {
            // If we have a list of references (classes), follow these (and we're done).
            for (u32 i = 0; i < len; ++i)
            {
                markValue(*list.getElement(i));
            }
            return;
        }

        // If we're here, this means we have struct elements.
        TypeLayoutRefCount refCount = ti.getRefCount();
        if (refCount == 0)
        {
            // If the type has no references to follow, we can bail early.
            return;
        }

        TypeLayoutRefOffsetIndex refOffsetStart = ti.getRefOffsetStartIndex();
        if (mTypeLayoutTable->hasRefOffsetRange(refOffsetStart, refCount) == false)
        {
            return;
        }

        for (u32 i = 0; i < len; ++i)
        {
            // The base address of the element is the i-th element in the block.
            VMWord* elementBase = list.getElement(i);
            for (u32 j = 0; j < refCount; ++j)
            {
                // We offset that base by the j-th reference the struct holds.
                TypeLayoutRefOffset offset = mTypeLayoutTable->getRefOffset(refOffsetStart + j);
                // Mark it accordingly in the block.
                markValue(elementBase[offset]);
            }
        }

        return;
    }

    if (header.mBlockType == BlockType::cMap)
    {
        MapBlock map{&header, payload};
        const u32 len = header.mLength;
        if (len == 0)
        {
            return;
        }

        // If we have strings as keys, we need to mark them.
        bool markStringKeys = (header.mTypeID == cStringTypeID);
        bool markReferenceValue = false;
        bool markStructValueRefs = false;
        TypeLayoutRefCount refCount = 0;
        TypeLayoutRefOffsetIndex refOffsetStart = 0;

        TypeID valueTypeID = header.getMapValueTypeID();

        const TypeLayout& ti = mTypeLayoutTable->getLayout(valueTypeID);
        if (ti.getKind() == TypeLayout::Kind::cReference)
        {
            // We have a reference type as the value type.
            markReferenceValue = true;
        }
        else
        {
            // We have a struct type as the value type, so we need to check if it has any references to follow.
            refCount = ti.getRefCount();
            refOffsetStart = ti.getRefOffsetStartIndex();
            markStructValueRefs = refCount != 0 && mTypeLayoutTable->hasRefOffsetRange(refOffsetStart, refCount);
        }

        // If we have nothing to mark at all, bail.
        if (markStringKeys == false && markReferenceValue == false && markStructValueRefs == false)
        {
            return;
        }

        // Otherwise, go over all buckets that are marked.
        map.forEachOccupiedBucket(
            [&](u32 index, const VMWord* bucket)
            {
                // If we have strings, mark the key (index 0 is the control, index 1 the key).
                if (markStringKeys)
                {
                    markValue(bucket[1]);
                }

                // If we have a reference value, mark it.
                if (markReferenceValue)
                {
                    markValue(*map.getValue(index));
                    return;
                }

                // If we have a struct value, go over all refs if holds and mark them.
                if (markStructValueRefs)
                {
                    const VMWord* valueBase = map.getValue(index);
                    for (u32 i = 0; i < refCount; ++i)
                    {
                        // Like for lists, we look up the offset in the layout table.
                        TypeLayoutRefOffset offset = mTypeLayoutTable->getRefOffset(refOffsetStart + i);
                        markValue(valueBase[offset]);
                    }
                }
            });

        return;
    }
}

u32 Heap::sweepAndMerge()
{
    u32 liveWords = 0;

    for (HeapIndex base = 0; base < static_cast<HeapIndex>(mData.size());)
    {
        // Get the block we're currently processing.
        BlockHeader& header = headerFromBase(base);

        // If the block is free, we can skip it.
        if (header.mBlockType == BlockType::cFree)
        {
            base += header.mSize;
            continue;
        }

        // If the block is used and not marked, we can free it.
        if (header.mMarked == false)
        {
            header.mBlockType = BlockType::cFree;
            header.mTypeID = 0;
            header.mLength = 0;
            header.clearElementInfo();

            // Also reset the mark for good measure.
            header.mMarked = false;
        }
        else
        {
            // If the block is marked, clear the mark and count the words that are live.
            header.mMarked = false;
            liveWords += header.mSize;
        }

        base += header.mSize;
    }

    // Merge adjacent free ones.
    for (HeapIndex base = 0; base < static_cast<HeapIndex>(mData.size());)
    {
        BlockHeader& a = headerFromBase(base);

        HeapIndex next = base + a.mSize;
        if (a.mBlockType == BlockType::cFree && next < mData.size())
        {
            BlockHeader& b = headerFromBase(next);
            if (b.mBlockType == BlockType::cFree)
            {
                // If the next block is a free block, add its size to the current one.
                a.mSize += b.mSize;
                // Move on without incrementing the base.
                continue;
            }
        }

        base += a.mSize;
    }

    return liveWords;
}

void Heap::sweepHandles()
{
    // Go over all handles.
    for (HeapIndex i = 0; i < static_cast<HeapIndex>(mHandles.size()); ++i)
    {
        // If the handle is invalid, skip it.
        HeapIndex base = mHandles[i];
        if (base == cInvalidHeapIndex)
        {
            continue;
        }

        // Otherwise, check whether the handle points to a now-free block.
        if (headerFromBase(base).mBlockType == BlockType::cFree)
        {
            mHandles[i] = cInvalidHeapIndex;
            mFreeHandles.push_back(i);
        }
    }
}

void Heap::rebuildFreeList()
{
    mFreeListHead = cInvalidHeapIndex;
    HeapIndex prevFree = cInvalidHeapIndex;

    for (HeapIndex base = 0; base < static_cast<HeapIndex>(mData.size());)
    {
        // Get the header of the current block.
        BlockHeader& header = headerFromBase(base);

        if (header.mBlockType == BlockType::cFree)
        {
            if (prevFree == cInvalidHeapIndex)
            {
                // Set it as the head of the free list.
                mFreeListHead = base;
            }
            else
            {
                // Link the previous free block to this one.
                setNextFreeBlock(prevFree, base);
            }

            // Clear the next pointer of the current free block.
            setNextFreeBlock(base, cInvalidHeapIndex);

            prevFree = base;
        }

        // Advance the base.
        base += header.mSize;
    }

    if (prevFree != cInvalidHeapIndex)
    {
        setNextFreeBlock(prevFree, cInvalidHeapIndex);
    }
}

} // namespace simlang
