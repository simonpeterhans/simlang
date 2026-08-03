#pragma once

#include <array>
#include <charconv>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "meta.h"
#include "util/types.h"

namespace simlang
{

class TextSink;

enum class TextAlignment : u8
{
    cLeft,
    cRight
};

class TextWriter
{
public:
    explicit TextWriter(TextSink& sink);

    TextWriter& operator<<(std::string_view text);
    TextWriter& operator<<(const std::string& text);
    TextWriter& operator<<(const char* text);
    TextWriter& operator<<(char c);

    template <typename T>
    TextWriter& operator<<(T value)
    {
        // Template for non-strings, must be valid in formatValue.
        std::string text = formatValue(value);
        return write(text);
    }

    TextWriter& write(std::string_view text);
    TextWriter& write(const std::string& text);
    TextWriter& write(const char* text);

    TextWriter& newline();

    TextWriter& writeLine();
    TextWriter& writeLine(std::string_view text);
    TextWriter& writeLine(const std::string& text);
    TextWriter& writeLine(const char* text);

    TextWriter& writePadded(std::string_view text, usize width, TextAlignment alignment);

    template <typename T>
    TextWriter& writePaddedValue(const T& value, usize width, TextAlignment alignment)
    {
        std::string text = formatValue(value);
        return writePadded(text, width, alignment);
    }

    void flush();

private:
    void writeRepeated(char c, usize count);

    template <typename T>
    static std::string formatValue(T value)
    {
        using RawT = std::decay_t<T>;

        if constexpr (std::is_same_v<RawT, bool>)
        {
            return value ? "true" : "false";
        }
        else if constexpr (std::is_same_v<RawT, char>)
        {
            return std::string(1, value);
        }
        else if constexpr (std::is_integral_v<RawT>)
        {
            std::array<char, 20> buffer;
            auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (result.ec != std::errc())
            {
                return {};
            }

            return std::string{buffer.data(), static_cast<usize>(result.ptr - buffer.data())};
        }
        else if constexpr (std::is_floating_point_v<RawT>)
        {
            std::array<char, 20> buffer;
            i32 count = std::snprintf(buffer.data(), buffer.size(), "%g", static_cast<double>(value));
            if (count <= 0)
            {
                return {};
            }

            usize size = static_cast<usize>(count);
            if (size >= buffer.size())
            {
                size = buffer.size() - 1;
            }

            return std::string{buffer.data(), size};
        }
        else
        {
            static_assert(always_false_v<T>, "TextWriter only supports arithmetic values for formatting.");
        }
    }

    TextSink& mSink;
};

class ScopedTextWriter
{
public:
    explicit ScopedTextWriter(std::unique_ptr<TextSink> sink);
    ~ScopedTextWriter();

    ScopedTextWriter(const ScopedTextWriter&) = delete;
    ScopedTextWriter& operator=(const ScopedTextWriter&) = delete;
    ScopedTextWriter(ScopedTextWriter&&) = delete;
    ScopedTextWriter& operator=(ScopedTextWriter&&) = delete;

    bool isOpen() const;
    TextWriter& getWriter();

private:
    std::unique_ptr<TextSink> mSink;
    TextWriter mWriter;
};

} // namespace simlang
