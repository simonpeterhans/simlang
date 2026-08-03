#include "util/escaping.h"

namespace simlang::escape
{

static constexpr unsigned char cFirstPrintableAscii = ' ';
static constexpr unsigned char cAsciiDelete = 0x7F;
static constexpr unsigned char cHexBase = 16;
static constexpr usize cHexEscapeCharCount = 3;

static bool shouldHexEscape(unsigned char value)
{
    return value < cFirstPrintableAscii || value == cAsciiDelete;
}

static char hexDigit(unsigned char value)
{
    static constexpr char cHexDigits[] = "0123456789ABCDEF";

    return cHexDigits[value];
}

static bool decodeHexDigit(char c, unsigned char& out)
{
    if (c >= '0' && c <= '9')
    {
        out = static_cast<unsigned char>(c - '0');
        return true;
    }

    if (c >= 'A' && c <= 'F')
    {
        out = static_cast<unsigned char>(10 + c - 'A');
        return true;
    }

    if (c >= 'a' && c <= 'f')
    {
        out = static_cast<unsigned char>(10 + c - 'a');
        return true;
    }

    return false;
}

static void appendEscapedByte(std::string& out, unsigned char value, bool escapeSingleQuote)
{
    switch (value)
    {
        case '\n':
        {
            out += "\\n";
            return;
        }
        case '\r':
        {
            out += "\\r";
            return;
        }
        case '\t':
        {
            out += "\\t";
            return;
        }
        case '\\':
        {
            out += "\\\\";
            return;
        }
        case '\'':
        {
            if (escapeSingleQuote)
            {
                out += "\\'";
            }
            else
            {
                out.push_back('\'');
            }
            return;
        }
        case '"':
        {
            out += "\\\"";
            return;
        }
        default:
        {
            if (shouldHexEscape(value))
            {
                // If this is a hex character that needs escaping (\xNN), do that.
                // Add the \x.
                out += "\\x";
                // Convert the u8 to two hex digits.
                out.push_back(hexDigit(value / cHexBase));
                out.push_back(hexDigit(value % cHexBase));
            }
            else
            {
                out.push_back(static_cast<char>(value));
            }
        }
    }
}

bool decodeEscapeSequence(std::string_view text, usize& charsConsumed, char& out)
{
    charsConsumed = 0;

    if (text.empty())
    {
        return false;
    }

    switch (text[0])
    {
        case '"':
        case '\'':
        case '{':
        case '}':
        case '\\':
        {
            out = text[0];
            charsConsumed = 1;
            return true;
        }
        case 'n':
        {
            out = '\n';
            charsConsumed = 1;
            return true;
        }
        case 'r':
        {
            out = '\r';
            charsConsumed = 1;
            return true;
        }
        case 't':
        {
            out = '\t';
            charsConsumed = 1;
            return true;
        }
        case 'x':
        {
            // The expected format is \xNN, so we need at least 3 characters after the \.
            if (text.size() < cHexEscapeCharCount)
            {
                return false;
            }

            // We're only interested in NN after the \x.
            unsigned char high = 0;
            unsigned char low = 0;
            // Convert two hex characters to a numerical value.
            if (decodeHexDigit(text[1], high) == false || decodeHexDigit(text[2], low) == false)
            {
                return false;
            }

            // Compose the two hex values to the final character.
            out = static_cast<char>((high * cHexBase) + low);
            // We consumed xNN.
            charsConsumed = cHexEscapeCharCount;
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool validateEscapeSequence(std::string_view text, usize& charsConsumed)
{
    char ignored = '\0';
    return decodeEscapeSequence(text, charsConsumed, ignored);
}

std::string formatEscapedByte(unsigned char value)
{
    std::string out;
    appendEscapedByte(out, value, true);
    return out;
}

std::string formatEscapedText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    for (char rawChar : text)
    {
        appendEscapedByte(out, static_cast<unsigned char>(rawChar), false);
    }

    return out;
}

} // namespace simlang::escape
