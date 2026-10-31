#pragma once

#include <functional>
#include <string_view>

#include "util/bitutils.h"
#include "util/types.h"

namespace simlang
{

inline constexpr u32 cHashSeed32 = 2166136261U;

inline u32 mixHash32(u32 value)
{
    value ^= value >> 16U;
    value *= 0x85EBCA6BU;
    value ^= value >> 13U;
    value *= 0xC2B2AE35U;
    value ^= value >> 16U;

    return value;
}

inline void combineHash32(u32& hash, u32 component)
{
    hash ^= component;
    hash *= 16777619U;
}

inline u32 hashFloat32(f32 value)
{
    return mixHash32(value == 0.0f ? 0U : bits::bitCast<u32>(value));
}

inline u32 hashBytes32(std::string_view bytes)
{
    u32 hash = cHashSeed32;
    for (char c : bytes)
    {
        combineHash32(hash, static_cast<u8>(c));
    }

    return mixHash32(hash);
}

template <typename T>
void hashCombine(std::size_t& seed, const T& value)
{
    constexpr std::size_t cGoldenRatio = 0x9E3779B97F4A7C15ULL;
    std::size_t hash = std::hash<T>{}(value);
    seed ^= hash + cGoldenRatio + (seed << 6) + (seed >> 2);
}

} // namespace simlang
