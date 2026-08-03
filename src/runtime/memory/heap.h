#pragma once

#include <vector>

#include "runtime/memory/heapblock.h"
#include "runtime/vmdefines.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

class TypeLayoutTable;

static_assert(sizeof(VMWord) == 4 && sizeof(HeapIndex) == 4, "Heap only supports 32 bit words and indices!");

inline constexpr HeapIndex cInvalidHeapIndex = cMaxHeapWords;

struct HeapOptions
{
    u32 mMaxHeapWords = 256 * 1024 * 1024;
    u32 mInitialHeapWords = 1 * 1024 * 1024;
    u32 mMinHeapGrowthWords = 1 * 1024 * 1024;
    f32 mHeapGrowthFactor = 2.0f;
    u32 mMinGCThresholdWords = 1 * 1024 * 1024;
    f32 mGCThresholdFactor = 2.0f;
};

class Heap
{
public:
    // Roots for mark-and-sweep GC to start from.
    struct Roots
    {
        ArrayView<const VMWord> mGlobals;
        ArrayView<const VMWord> mStack;
        ArrayView<const VMWord> mTemp;
    };

    class RootProvider
    {
    public:
        virtual Roots makeRoots() const = 0;

    protected:
        ~RootProvider() = default;
    };

    struct StringAlloc
    {
        VMWord mRef = cNullRef;
        char* mBytes = nullptr;
    };

    struct ListAlloc
    {
        VMWord mRef = cNullRef;
        ListBlock mBlock;
    };

    struct MapAlloc
    {
        VMWord mRef = cNullRef;
        MapBlock mBlock;
    };

    explicit Heap(HeapOptions options = {});

    VMWord allocateObject(TypeID typeID, u32 dataWords, const RootProvider& roots);
    StringAlloc allocateString(u32 byteLen, const RootProvider& roots);
    ListAlloc allocateList(TypeID elemTypeID, u32 elemWords, u32 capacity, const RootProvider& roots);
    bool resizeList(VMWord handle, u32 capacity, const RootProvider& roots);
    MapAlloc allocateMap(TypeID keyTypeID, TypeID valueTypeID, u32 valueWords, u32 capacity, const RootProvider& roots);
    bool resizeMap(VMWord handle, u32 capacity, const RootProvider& roots);

    bool resolvePayloadAddress(VMWord address, Block& out);
    bool resolveHandle(VMWord handle, Block& out);
    bool resolveListHandle(VMWord handle, ListBlock& out);
    bool resolveMapHandle(VMWord handle, MapBlock& out);

    VMWord& wordAt(HeapIndex idx) { return mData[idx]; }
    const VMWord& wordAt(HeapIndex idx) const { return mData[idx]; }

    void collect(const Roots& roots);

    void setTypeLayoutTable(const TypeLayoutTable* table) { mTypeLayoutTable = table; }

private:
    BlockHeader& headerFromBase(HeapIndex base) { return *reinterpret_cast<BlockHeader*>(&mData[base]); }
    const BlockHeader& headerFromBase(HeapIndex base) const
    {
        return *reinterpret_cast<const BlockHeader*>(&mData[base]);
    }

    static HeapIndex payloadIndexFromBase(HeapIndex base) { return base + cBlockHeaderWords; }
    VMWord* payloadFromBase(HeapIndex base) { return &mData[payloadIndexFromBase(base)]; }
    const VMWord* payloadFromBase(HeapIndex base) const { return &mData[payloadIndexFromBase(base)]; }

    HeapIndex getNextFreeBlock(HeapIndex freeBlockBase) const { return *payloadFromBase(freeBlockBase); }
    void setNextFreeBlock(HeapIndex freeBlockBase, HeapIndex nextFreeBlockBase)
    {
        *payloadFromBase(freeBlockBase) = nextFreeBlockBase;
    }

    bool resolveHandleToBase(VMWord handle, HeapIndex& outBase) const;
    bool resolveListHandle(VMWord handle, HeapIndex& outBase, ListBlock& out);
    bool resolveMapHandle(VMWord handle, HeapIndex& outBase, MapBlock& out);

    HeapIndex alloc(BlockType t,
                    TypeID typeID,
                    u64 requestedPayloadWords,
                    u32 length,
                    u32 elementWords,
                    const RootProvider& roots,
                    bool zero,
                    TypeID mapValueTypeID = 0);
    HeapIndex tryAllocFromFreeList(u32 requestedWords, u32& allocatedWords);
    bool growHeap(u32 minWords);
    bool appendFreeBlock(u32 words);
    void writeHeader(HeapIndex base,
                     BlockType t,
                     TypeID typeID,
                     u32 totalWords,
                     u32 length,
                     u32 elementWords,
                     TypeID mapValueTypeID = 0);
    void setOwnerRange(HeapIndex base, u32 totalWords);
    VMWord makeHandle(HeapIndex base);

    void markFromRoots(const Roots& roots);
    void markValue(VMWord maybeHeapAddr);
    void scanBlockPrecise(HeapIndex base);
    u32 sweepAndMerge();
    void sweepHandles();
    void rebuildFreeList();

    std::vector<VMWord> mData;
    std::vector<HeapIndex> mHandles;
    std::vector<HeapIndex> mFreeHandles;
    std::vector<HeapIndex> mOwner;
    std::vector<HeapIndex> mMarkStack;
    const TypeLayoutTable* mTypeLayoutTable = nullptr;
    HeapOptions mOptions;
    HeapIndex mFreeListHead = cInvalidHeapIndex;
    u32 mAllocWordsSinceGC = 0;
    u32 mNextGCThresholdWords = 0;
};

} // namespace simlang
