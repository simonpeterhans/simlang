#pragma once

#include <type_traits>

namespace simlang
{

template <class T>
class FlagSet
{
    static_assert(std::is_integral_v<T>, "FlagSet requires integral storage!");

public:
    constexpr FlagSet() = default;

    constexpr explicit FlagSet(T bits)
        : mBits(bits)
    {
    }

    constexpr FlagSet& setBits(T bits)
    {
        mBits = bits;
        return *this;
    }

    constexpr FlagSet& addBits(T bits)
    {
        mBits = static_cast<T>(mBits | bits);
        return *this;
    }

    constexpr FlagSet& add(FlagSet other) { return addBits(other.mBits); }

    constexpr T bits() const { return mBits; }

    template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    constexpr bool test(E e) const
    {
        const T bit = static_cast<T>(e);
        return (mBits & bit) != 0;
    }

    template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    constexpr FlagSet& set(E e, bool on = true)
    {
        const T bit = static_cast<T>(e);
        if (on)
        {
            mBits = static_cast<T>(mBits | bit);
        }
        else
        {
            mBits = static_cast<T>(mBits & static_cast<T>(~bit));
        }
        return *this;
    }

private:
    T mBits = 0;
};

template <class E>
class TypedFlagSet
{
    static_assert(std::is_enum_v<E>, "TypedFlagSet requires enum type!");

public:
    using ET = std::underlying_type_t<E>;

    constexpr TypedFlagSet() = default;

    constexpr explicit TypedFlagSet(E e)
        : mBits(static_cast<ET>(e))
    {
    }

    constexpr explicit TypedFlagSet(ET bits)
        : mBits(bits)
    {
    }

    constexpr TypedFlagSet& setBits(ET bits)
    {
        mBits = bits;
        return *this;
    }

    constexpr TypedFlagSet& addBits(ET bits)
    {
        mBits = static_cast<ET>(mBits | bits);
        return *this;
    }

    constexpr TypedFlagSet& add(TypedFlagSet other) { return addBits(other.mBits); }

    constexpr ET bits() const { return mBits; }

    constexpr bool test(E e) const
    {
        const ET bit = static_cast<ET>(e);
        return (mBits & bit) != 0;
    }

    constexpr TypedFlagSet& set(E e, bool on = true)
    {
        const ET bit = static_cast<ET>(e);
        if (on)
        {
            mBits = static_cast<ET>(mBits | bit);
        }
        else
        {
            mBits = static_cast<ET>(mBits & static_cast<ET>(~bit));
        }
        return *this;
    }

private:
    ET mBits = 0;
};

} // namespace simlang
