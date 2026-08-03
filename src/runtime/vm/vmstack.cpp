#include "runtime/vm/vmstack.h"

#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace simlang
{

VMStack::VMStack(u32 initialCapacity)
{
    // Always start with at least one word since that makes growing (and everything else) easier.
    mCapacityWords = (initialCapacity == 0) ? 1U : initialCapacity;
    if (mCapacityWords > cMaxVMStackWords)
    {
        mCapacityWords = cMaxVMStackWords;
    }

    mStorage.reset(new VMWord[mCapacityWords]);
    mBase = mStorage.get();
    mTop = mBase;
}

bool VMStack::reserve(u64 requiredWords) noexcept
{
    // If we have more capacity than we need, do nothing.
    if (requiredWords <= mCapacityWords)
    {
        return true;
    }

    // If we need more words than our stack size, this fails.
    if (requiredWords > cMaxVMStackWords)
    {
        return false;
    }

    // Otherwise, find out how many words are currently used.
    u32 liveWords = getSize();

    // Grow geometrically while that stays within the VM stack limit.
    u32 newSize = static_cast<u32>(requiredWords);
    if (mCapacityWords <= cMaxVMStackWords / 2)
    {
        u32 doubledSize = mCapacityWords * 2;
        if (doubledSize > newSize)
        {
            newSize = doubledSize;
        }
    }

    std::unique_ptr<VMWord[]> newStack{new (std::nothrow) VMWord[newSize]};
    if (newStack == nullptr)
    {
        return false;
    }

    // If we have any live words, copy them to the new stack.
    if (liveWords > 0)
    {
        std::memcpy(newStack.get(), mBase, liveWords * sizeof(VMWord));
    }

    // Move the memory and delete the old stack.
    mStorage = std::move(newStack);

    // Update the pointers and capacity.
    mBase = mStorage.get();
    mTop = mBase + liveWords;
    mCapacityWords = newSize;

    return true;
}

} // namespace simlang
