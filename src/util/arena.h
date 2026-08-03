#pragma once

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

#include "util/types.h"

namespace simlang
{

class ArenaAllocator
{
public:
    static constexpr usize cDefaultInitialBlockSizeBytes = 16 * 1024;
    static constexpr usize cDefaultMaxBlockSizeBytes = 512 * 1024;

    explicit ArenaAllocator(usize initialBlockSize = cDefaultInitialBlockSizeBytes,
                            usize maxBlockSize = cDefaultMaxBlockSizeBytes);

    ~ArenaAllocator();

    void* allocateBytes(usize size, usize alignment = alignof(std::max_align_t));

    usize getReservedBytes() const { return mReservedBytes; }
    usize getAllocatedBytes() const { return mAllocatedBytes; }
    usize getBlockCount() const { return mBlocks.size(); }

    template <typename T, typename... Args>
    T* create(Args&&... args)
    {
        usize size = sizeof(T);
        usize alignment = alignof(T);

        // Allocate the memory, but don't call the constructor.
        void* ptr = allocateBytes(size, alignment);
        // Call the constructor using the previously allocated memory.
        T* obj = new (ptr) T(std::forward<Args>(args)...);

        return obj;
    }

    template <typename T>
    T* createArray(usize count)
    {
        if (count == 0)
        {
            return nullptr;
        }

        usize size = sizeof(T);
        usize alignment = alignof(T);

        // Allocate the memory for all elements.
        void* mem = allocateBytes(size * count, alignment);
        T* arr = static_cast<T*>(mem);
        for (usize i = 0; i < count; ++i)
        {
            // Call the constructor for each element.
            new (&arr[i]) T();
        }

        return arr;
    }

private:
    void allocateBlock(usize size);
    void growNextBlockSize();

    std::vector<u8*> mBlocks;
    usize mMaxBlockSize = cDefaultMaxBlockSizeBytes;
    usize mNextBlockSize = cDefaultInitialBlockSizeBytes;
    usize mCurrentBlockSize = 0;
    usize mCurrentOffset = 0;
    usize mAllocatedBytes = 0;
    usize mReservedBytes = 0;

    u8* mCurrentBlock = nullptr;
};

} // namespace simlang
