#include "util/textwriter.h"

#include <utility>

#include "util/textsink.h"

namespace simlang
{

namespace
{
class NullTextSink final : public TextSink
{
public:
    void write(std::string_view) override {}
};

TextSink& nullTextSink()
{
    static NullTextSink sink;
    return sink;
}
} // namespace

TextWriter::TextWriter(TextSink& sink)
    : mSink(sink)
{
}

TextWriter& TextWriter::operator<<(std::string_view text)
{
    return write(text);
}

TextWriter& TextWriter::operator<<(const std::string& text)
{
    return write(text);
}

TextWriter& TextWriter::operator<<(const char* text)
{
    return write(text);
}

TextWriter& TextWriter::operator<<(char c)
{
    return write(std::string_view{&c, 1});
}

TextWriter& TextWriter::write(std::string_view text)
{
    // The actual call to the sink.
    // All the other writes call this with a string_view.
    mSink.write(text);
    // Return ourselves for chaining.
    return *this;
}

TextWriter& TextWriter::write(const std::string& text)
{
    return write(std::string_view{text.data(), text.size()});
}

TextWriter& TextWriter::write(const char* text)
{
    return write(text != nullptr ? std::string_view{text} : std::string_view{});
}

TextWriter& TextWriter::newline()
{
    return write("\n");
}

TextWriter& TextWriter::writeLine()
{
    return newline();
}

TextWriter& TextWriter::writeLine(std::string_view text)
{
    write(text);
    return newline();
}

TextWriter& TextWriter::writeLine(const std::string& text)
{
    write(text);
    return newline();
}

TextWriter& TextWriter::writeLine(const char* text)
{
    write(text);
    return newline();
}

void TextWriter::flush()
{
    mSink.flush();
}

TextWriter& TextWriter::writePadded(std::string_view text, usize width, TextAlignment alignment)
{
    usize textWidth = text.size();
    usize padding = (textWidth < width) ? (width - textWidth) : 0;

    // Do the padding either before or after the text.
    if (alignment == TextAlignment::cRight)
    {
        writeRepeated(' ', padding);
    }

    write(text);

    if (alignment == TextAlignment::cLeft)
    {
        writeRepeated(' ', padding);
    }

    return *this;
}

void TextWriter::writeRepeated(char c, usize count)
{
    // This is a fun one.
    // Use a small buffer, if we ever need more just iterate until we're done.
    static constexpr usize cBufferSize = 64;

    std::array<char, cBufferSize> buffer;
    buffer.fill(c);

    while (count > 0)
    {
        // Don't overshoot if we asked for less.
        usize n = (count < cBufferSize) ? count : cBufferSize;
        mSink.write(std::string_view{buffer.data(), n});
        count -= n;
    }
}

ScopedTextWriter::ScopedTextWriter(std::unique_ptr<TextSink> sink)
    : mSink(std::move(sink))
    , mWriter(mSink != nullptr ? *mSink : nullTextSink())
{
}

ScopedTextWriter::~ScopedTextWriter()
{
    if (mSink != nullptr)
    {
        mWriter.flush();
    }
}

bool ScopedTextWriter::isOpen() const
{
    return mSink != nullptr;
}

TextWriter& ScopedTextWriter::getWriter()
{
    return mWriter;
}

} // namespace simlang
