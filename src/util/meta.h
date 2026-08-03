#pragma once

#include <type_traits>

namespace simlang
{

#if defined(_MSC_VER)
    #define SIMLANG_NOINLINE __declspec(noinline)
    #define SIMLANG_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define SIMLANG_NOINLINE __attribute__((noinline))
    #define SIMLANG_ALWAYS_INLINE inline __attribute__((always_inline))
#else
    #define SIMLANG_NOINLINE
    #define SIMLANG_ALWAYS_INLINE inline
#endif

#define SIMLANG_CONCAT_INNER(a, b) a##b
#define SIMLANG_CONCAT(a, b) SIMLANG_CONCAT_INNER(a, b)

template <typename T>
inline constexpr bool always_false_v = false;

template <typename T>
using RemoveCVRefT = std::remove_cv_t<std::remove_reference_t<T>>;

} // namespace simlang
