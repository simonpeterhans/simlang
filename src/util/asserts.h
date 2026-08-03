#pragma once

#include <string_view>

#include "util/meta.h"

#ifndef SIMLANG_ENABLE_ASSERTS
    #ifdef NDEBUG
        #define SIMLANG_ENABLE_ASSERTS 0
    #else
        #define SIMLANG_ENABLE_ASSERTS 1
    #endif
#endif

namespace simlang
{

void breakHere();
void reportAssert(const char* expr, const char* file, int line, const char* func, std::string_view msg = {});
void reportAssertFormat(const char* expr, const char* file, int line, const char* func, const char* fmt, ...);

#if SIMLANG_ENABLE_ASSERTS
    #define SIMLANG_ASSERT_IMPL(cond, msgStr) \
        do \
        { \
            if (!(cond)) \
            { \
                ::simlang::reportAssert(#cond, __FILE__, __LINE__, __func__, std::string_view{msgStr}); \
                ::simlang::breakHere(); \
            } \
        } while (0)

    #define SIMLANG_ASSERTF_IMPL(cond, fmt, ...) \
        do \
        { \
            if (!(cond)) \
            { \
                ::simlang::reportAssertFormat(#cond, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
                ::simlang::breakHere(); \
            } \
        } while (0)

    #define SIMLANG_ASSERT_ONCE_IMPL(cond, msgStr, line) \
        do \
        { \
            static bool SIMLANG_CONCAT(_assert_once_hit_, line) = false; \
            if (!(cond) && !SIMLANG_CONCAT(_assert_once_hit_, line)) \
            { \
                SIMLANG_CONCAT(_assert_once_hit_, line) = true; \
                ::simlang::reportAssert(#cond, __FILE__, __LINE__, __func__, std::string_view{msgStr}); \
                ::simlang::breakHere(); \
            } \
        } while (0)

    #define SIMLANG_ASSERTF_ONCE_IMPL(cond, fmt, line, ...) \
        do \
        { \
            static bool SIMLANG_CONCAT(_assert_once_hit_, line) = false; \
            if (!(cond) && !SIMLANG_CONCAT(_assert_once_hit_, line)) \
            { \
                SIMLANG_CONCAT(_assert_once_hit_, line) = true; \
                ::simlang::reportAssertFormat(#cond, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
                ::simlang::breakHere(); \
            } \
        } while (0)

    #define SIMLANG_BREAK_IMPL(msgStr) \
        do \
        { \
            ::simlang::reportAssert("BREAK", __FILE__, __LINE__, __func__, std::string_view{msgStr}); \
            ::simlang::breakHere(); \
        } while (0)

    #define SIMLANG_BREAKF_IMPL(fmt, ...) \
        do \
        { \
            ::simlang::reportAssertFormat("BREAK", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
            ::simlang::breakHere(); \
        } while (0)

    #define SIMLANG_BREAK_ONCE_IMPL(msgStr, line) \
        do \
        { \
            static bool SIMLANG_CONCAT(_break_once_hit_, line) = false; \
            if (!SIMLANG_CONCAT(_break_once_hit_, line)) \
            { \
                SIMLANG_CONCAT(_break_once_hit_, line) = true; \
                ::simlang::reportAssert("BREAK_ONCE", __FILE__, __LINE__, __func__, std::string_view{msgStr}); \
                ::simlang::breakHere(); \
            } \
        } while (0)

    #define SIMLANG_BREAKF_ONCE_IMPL(fmt, line, ...) \
        do \
        { \
            static bool SIMLANG_CONCAT(_break_once_hit_, line) = false; \
            if (!SIMLANG_CONCAT(_break_once_hit_, line)) \
            { \
                SIMLANG_CONCAT(_break_once_hit_, line) = true; \
                ::simlang::reportAssertFormat("BREAK_ONCE", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
                ::simlang::breakHere(); \
            } \
        } while (0)
#else
    #define SIMLANG_ASSERT_IMPL(cond, msgStr) \
        do \
        { \
            (void)sizeof(cond); \
        } while (0)
    #define SIMLANG_ASSERTF_IMPL(cond, fmt, ...) \
        do \
        { \
            (void)sizeof(cond); \
        } while (0)
    #define SIMLANG_ASSERT_ONCE_IMPL(cond, msgStr, line) \
        do \
        { \
            (void)sizeof(cond); \
        } while (0)
    #define SIMLANG_ASSERTF_ONCE_IMPL(cond, fmt, line, ...) \
        do \
        { \
            (void)sizeof(cond); \
        } while (0)

    #define SIMLANG_BREAK_IMPL(msgStr) \
        do \
        { \
        } while (0)
    #define SIMLANG_BREAKF_IMPL(fmt, ...) \
        do \
        { \
        } while (0)
    #define SIMLANG_BREAK_ONCE_IMPL(msgStr, line) \
        do \
        { \
        } while (0)
    #define SIMLANG_BREAKF_ONCE_IMPL(fmt, line, ...) \
        do \
        { \
        } while (0)
#endif

#define SIMLANG_ASSERT(cond) SIMLANG_ASSERT_IMPL((cond), "")
#define SIMLANG_ASSERTM(cond, msg) SIMLANG_ASSERT_IMPL((cond), (msg))
#define SIMLANG_ASSERTF(cond, fmt, ...) SIMLANG_ASSERTF_IMPL((cond), (fmt), ##__VA_ARGS__)

#define SIMLANG_ASSERT_ONCE(cond) SIMLANG_ASSERT_ONCE_IMPL((cond), "", __LINE__)
#define SIMLANG_ASSERTM_ONCE(cond, msg) SIMLANG_ASSERT_ONCE_IMPL((cond), (msg), __LINE__)
#define SIMLANG_ASSERTF_ONCE(cond, fmt, ...) SIMLANG_ASSERTF_ONCE_IMPL((cond), (fmt), __LINE__, ##__VA_ARGS__)

#define SIMLANG_BREAK(msg) SIMLANG_BREAK_IMPL((msg))
#define SIMLANG_BREAKF(fmt, ...) SIMLANG_BREAKF_IMPL((fmt), ##__VA_ARGS__)

#define SIMLANG_BREAK_ONCE(msg) SIMLANG_BREAK_ONCE_IMPL((msg), __LINE__)
#define SIMLANG_BREAKF_ONCE(fmt, ...) SIMLANG_BREAKF_ONCE_IMPL((fmt), __LINE__, ##__VA_ARGS__)

} // namespace simlang
