#pragma once

#include <cstring>
#include <type_traits>

#include "util/types.h"

namespace simlang::bits
{

template <typename To, typename From>
To bitCast(const From& from)
{
    static_assert(sizeof(To) == sizeof(From), "bitCast requires source and destination types to have the same size.");
    static_assert(std::is_trivially_copyable_v<To>, "bitCast destination type must be trivially copyable.");
    static_assert(std::is_trivially_copyable_v<From>, "bitCast source type must be trivially copyable.");

    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

template <typename T>
T readUnaligned(const u8* bytes)
{
    static_assert(std::is_trivially_copyable_v<T>, "readUnaligned type must be trivially copyable.");

    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

template <typename T>
void writeUnaligned(u8* bytes, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>, "writeUnaligned type must be trivially copyable.");

    std::memcpy(bytes, &value, sizeof(T));
}

} // namespace simlang::bits
