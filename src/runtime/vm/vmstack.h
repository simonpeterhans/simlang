#pragma once

#include <memory>

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

// Minimalistic VM stack.
// Note that this purely exists for performance and does no bounds checking whatsoever.
class VMStack
{
public:
    explicit VMStack(u32 initialCapacity);

    // Assumes space was reserved previously via reserve().
    void pushWord(VMWord word)
    {
        // Push one word and directly assign to it.
        *mTop++ = word;
    }

    // Assumes the stack is not empty.
    VMWord popWord()
    {
        // Decrement the top pointer and return the previous ToS.
        return *--mTop;
    }

    // Assumes space was reserved previously (if needed) via reserve().
    void setSize(u32 wordCount)
    {
        // Directly set the size.
        // Typically used for popping N words if the stack size is known.
        mTop = mBase + wordCount;
    }

    u32 getSize() const
    {
        // ToS points to the word after the last pushed word.
        // This means we can just subtract the base from it to get the size.
        return static_cast<u32>(mTop - mBase);
    }

    u32 getCapacity() const { return mCapacityWords; }

    bool reserve(u64 requiredWords) noexcept;

    VMWord* getData() { return mBase; }
    const VMWord* getData() const { return mBase; }

private:
    std::unique_ptr<VMWord[]> mStorage;
    VMWord* mBase = nullptr;
    VMWord* mTop = nullptr;
    u32 mCapacityWords = 0;
};

} // namespace simlang
