#pragma once

#include "util/types.h"

namespace simlang
{

struct DiagnosticFormat
{
    constexpr explicit DiagnosticFormat(const char* text)
        : mText(text)
        , mPlaceholderCount(countPlaceholders(text))
    {
    }

    const char* mText = nullptr;
    const usize mPlaceholderCount = 0;

private:
    static constexpr usize countPlaceholders(const char* format)
    {
        // Assumes a null-terminated string.
        usize placeholderCount = 0;
        for (usize i = 0; format[i] != '\0'; ++i)
        {
            if (format[i] == '{' && format[i + 1] == '}')
            {
                // Since the string is null-terminated, i + 1 is not out of bounds if i is not \0.
                ++placeholderCount;
                ++i;
            }
        }

        return placeholderCount;
    }
};

} // namespace simlang
