#pragma once

#include <type_traits>

#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

// C++17 doesn't have spans, so we use our own array view instead.
template <typename T>
class ArrayView
{
public:
    ArrayView() = default;

    ArrayView(T* data, usize size)
        : mData(data)
        , mSize(size)
    {
        // If we have any size > 0, the data cannot be null.
        SIMLANG_ASSERTM(size == 0 || data != nullptr, "Non-empty ArrayView must have storage.");
    }

    // Allows ArrayView<const T> = ArrayView<T>.
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    explicit ArrayView(ArrayView<U> other)
        : mData(other.data())
        , mSize(other.size())
    {
    }

    T& operator[](usize index)
    {
        SIMLANG_ASSERT(index < mSize);
        return mData[index];
    }
    const T& operator[](usize index) const
    {
        SIMLANG_ASSERT(index < mSize);
        return mData[index];
    }

    T* begin() { return mData; }
    const T* begin() const { return mData; }

    T* end() { return (mSize == 0) ? mData : mData + mSize; }
    const T* end() const { return (mSize == 0) ? mData : mData + mSize; }

    T* data() { return mData; }
    const T* data() const { return mData; }

    usize size() const { return mSize; }
    bool empty() const { return (mSize == 0); }

    T& front()
    {
        SIMLANG_ASSERT(mSize > 0);
        return mData[0];
    }
    const T& front() const
    {
        SIMLANG_ASSERT(mSize > 0);
        return mData[0];
    }

    T& back()
    {
        SIMLANG_ASSERT(mSize > 0);
        return mData[mSize - 1];
    }
    const T& back() const
    {
        SIMLANG_ASSERT(mSize > 0);
        return mData[mSize - 1];
    }

private:
    T* mData = nullptr;
    usize mSize = 0;
};

} // namespace simlang
