#pragma once

#include <functional>
#include <type_traits>
#include <utility>

namespace simlang
{

class OnScopeEnd
{
public:
    // Force F to be callable.
    template <class F,
              typename = std::enable_if_t<std::is_same_v<std::decay_t<F>, OnScopeEnd> == false &&
                                          std::is_invocable_r_v<void, F&>>>
    explicit OnScopeEnd(F&& f)
        : mFunc(std::forward<F>(f))
    {
    }

    OnScopeEnd(const OnScopeEnd&) = delete;
    OnScopeEnd& operator=(const OnScopeEnd&) = delete;

    OnScopeEnd(OnScopeEnd&& other) noexcept
        : mFunc(std::move(other.mFunc))
    {
        other.mFunc = nullptr;
    }

    ~OnScopeEnd()
    {
        if (mFunc != nullptr)
        {
            mFunc();
        }
    }

private:
    std::function<void()> mFunc;
};

template <typename T>
class ScopedValueBinder
{
public:
    explicit ScopedValueBinder(T& storage, T val)
        : mStorage(storage)
        , mOldValue(storage)
    {
        mStorage = val;
    }

    // No copying.
    ScopedValueBinder(const ScopedValueBinder&) = delete;
    ScopedValueBinder& operator=(const ScopedValueBinder&) = delete;

    ScopedValueBinder(ScopedValueBinder&& other) noexcept(std::is_nothrow_copy_constructible_v<T>)
        : mStorage(other.mStorage)
        , mOldValue(other.mOldValue)
        , mActive(other.mActive)
    {
        // If this gets move-constructed, disable the old one (!).
        // That way, the old one does not trigger the reset on the destructor.
        // This is practical and required when returning an SVB.
        other.mActive = false;
    }
    ScopedValueBinder& operator=(ScopedValueBinder&&) = delete;

    ~ScopedValueBinder()
    {
        if (mActive)
        {
            mStorage = mOldValue;
        }
    }

private:
    T& mStorage;
    T mOldValue;
    bool mActive = true;
};

} // namespace simlang
