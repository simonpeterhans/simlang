#include "util/asserts.h"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <string_view>

#include "util/textsinks.h"
#include "util/textwriter.h"

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace simlang
{

void breakHere()
{
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__clang__) || defined(__GNUC__)
    #if defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
    __builtin_debugtrap();
    #elif defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("int $3");
    #elif defined(SIGTRAP)
    std::raise(SIGTRAP);
    #else
    std::raise(SIGABRT);
    #endif
#else
    #if defined(SIGTRAP)
    std::raise(SIGTRAP);
    #else
    std::raise(SIGABRT);
    #endif
#endif
}

void reportAssert(const char* expr, const char* file, int line, const char* func, std::string_view msg)
{
    FileTextSink sink{stderr};
    TextWriter out{sink};

    std::string_view kind{expr};
    if (kind == "BREAK" || kind == "BREAK_ONCE")
    {
        out.write(kind).writeLine(" HIT");
    }
    else
    {
        out << "ASSERT FAILED: " << expr << '\n';
    }

    out << "  at " << file << ":" << line << " in " << func << '\n';
    if (msg.empty() == false)
    {
        out.write("  note: ").writeLine(msg);
    }
    out.flush();
}

void reportAssertFormat(const char* expr, const char* file, int line, const char* func, const char* fmt, ...)
{
    char buffer[1024];

    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (n < 0)
    {
        buffer[0] = '\0';
    }

    reportAssert(expr, file, line, func, std::string_view{buffer});
}

} // namespace simlang
