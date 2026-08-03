#include "util/arena.h"

#include <algorithm>

#include "util/asserts.h"

namespace simlang
{

static usize getAlignmentPadding(const u8* block, usize offset, usize alignment)
{
    // Get the address we're currently pointing at.
    usize address = reinterpret_cast<usize>(block) + offset;
    // Get the next aligned address (move into aligned address space and set the alignment bits to 0).
    usize alignedAddress = (address + alignment - 1) & ~(alignment - 1);
    // Padding between the current address and the start of the next (aligned) address.
    return alignedAddress - address;
}

ArenaAllocator::ArenaAllocator(usize initialBlockSize, usize maxBlockSize)
{
    if (initialBlockSize == 0)
    {
        initialBlockSize = cDefaultInitialBlockSizeBytes;
    }

    if (maxBlockSize < initialBlockSize)
    {
        maxBlockSize = initialBlockSize;
    }

    mMaxBlockSize = maxBlockSize;
    mNextBlockSize = initialBlockSize;
}

ArenaAllocator::~ArenaAllocator()
{
    for (u8* block : mBlocks)
    {
        operator delete(block);
    }

    mBlocks.clear();
}

void* ArenaAllocator::allocateBytes(usize size, usize alignment)
{
    SIMLANG_ASSERTM(alignment != 0 && (alignment & (alignment - 1)) == 0,
                    "Arena allocation alignment must be a power of two.");

    // Get the padding required for alignment.
    usize padding = getAlignmentPadding(mCurrentBlock, mCurrentOffset, alignment);

    // Find out if we have enough space.
    if (mCurrentOffset + padding + size > mCurrentBlockSize)
    {
        // We need a new block.
        usize requiredBlockSize = size + alignment;
        // Make sure we allocate enough in case of a large allocation.
        usize newBlockSize = std::max(mNextBlockSize, requiredBlockSize);

        // Allocate the block and grow the normal block size if this was not a large one-off allocation.
        allocateBlock(newBlockSize);
        if (requiredBlockSize <= mMaxBlockSize)
        {
            growNextBlockSize();
        }

        // Update the padding for the new block.
        padding = getAlignmentPadding(mCurrentBlock, mCurrentOffset, alignment);
    }

    // Add the required padding.
    mCurrentOffset += padding;

    // Return the current position and consume the requested size.
    void* ptr = mCurrentBlock + mCurrentOffset;
    mCurrentOffset += size;
    mAllocatedBytes += padding + size;

    return ptr;
}

void ArenaAllocator::allocateBlock(usize size)
{
    mCurrentBlock = static_cast<u8*>(operator new(size));
    mCurrentBlockSize = size;
    mCurrentOffset = 0;
    mReservedBytes += size;
    mBlocks.push_back(mCurrentBlock);
}

void ArenaAllocator::growNextBlockSize()
{
    // If we already reached the max, bail.
    if (mNextBlockSize >= mMaxBlockSize)
    {
        return;
    }

    if (mNextBlockSize > mMaxBlockSize / 2)
    {
        // If doubling would exceed the max, just set to max.
        mNextBlockSize = mMaxBlockSize;
    }
    else
    {
        // Otherwise, double normally.
        mNextBlockSize *= 2;
    }
}

} // namespace simlang
