#include "diag/diagnosticmanager.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

#include "diag/diagnosticlevel.h"
#include "diag/diagnostictype.h"
#include "parser/tokentype.h"
#include "source/linecolumninfo.h"
#include "source/source.h"
#include "source/sourcemanager.h"
#include "util/meta.h"
#include "util/textwriter.h"

namespace simlang
{

static constexpr u8 cTabWidth = 4;

DiagnosticBuilder::DiagnosticBuilder(DiagnosticManager& manager)
    : mManager(manager)
{
}

DiagnosticBuilder& DiagnosticBuilder::note(SourceRange range, std::string_view message)
{
    return note<cNoteMessage>(range, message);
}

DiagnosticBuilder& DiagnosticBuilder::hint(SourceRange range, std::string_view message)
{
    return hint<cHintMessage>(range, message);
}

DiagnosticManager::DiagnosticManager(const SourceManager& sm, TextSink& output)
    : mSourceManager(sm)
    , mOutput(output)
{
}

void DiagnosticManager::emit() const
{
    TextWriter out{mOutput};
    bool emittedDiagnostic = false;
    for (const Diagnostic& diag : mDiagnostics)
    {
        if (emittedDiagnostic && diag.mAttached == false)
        {
            out.newline();
        }

        emit(out, diag, diag.mAttached);
        emittedDiagnostic = true;
    }
    out.flush();
}

void DiagnosticManager::emitAndClear()
{
    emit();
    mDiagnostics.clear();
    mErrorCount = 0;
}

template <typename T>
static void appendInt(std::string& out, T value)
{
    std::array<char, 20> buffer;
    auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc())
    {
        return;
    }

    out.append(buffer.data(), static_cast<usize>(result.ptr - buffer.data()));
}

static void appendFloat(std::string& out, double value)
{
    std::array<char, 20> buffer;
    int count = std::snprintf(buffer.data(), buffer.size(), "%.6g", value);
    if (count <= 0)
    {
        return;
    }

    usize size = static_cast<usize>(count);
    if (size >= buffer.size())
    {
        size = buffer.size() - 1;
    }

    out.append(buffer.data(), size);
}

static void appendTokenList(std::string& out, const std::vector<TokenType>& tokens)
{
    usize n = tokens.size();
    for (usize i = 0; i < n; ++i)
    {
        // Wrap in 'token'.
        out.push_back('\'');
        out.append(tokenTypeToString(tokens[i]));
        out.push_back('\'');

        if (i + 2 == n)
        {
            // If we're at the penultimate token, do special stuff.
            if (n == 2)
            {
                // Only 2 things, 'x or y'.
                out.append(" or ");
            }
            else
            {
                // More than 2 things, 'x, y, or z'.
                out.append(", or ");
            }
        }
        else if (i + 1 < n)
        {
            out.append(", ");
        }
    }
}

static void appendDiagnosticParam(std::string& out, const DiagnosticParam& param)
{
    // Handle the diagnostic parameter based on what we have.
    std::visit(
        [&](auto&& value)
        {
            using ValueType = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<ValueType, bool>)
            {
                out.append(value ? "true" : "false");
            }
            else if constexpr (std::is_integral_v<ValueType>)
            {
                appendInt(out, value);
            }
            else if constexpr (std::is_floating_point_v<ValueType>)
            {
                appendFloat(out, value);
            }
            else if constexpr (std::is_same_v<ValueType, std::string>)
            {
                out.append(value);
            }
            else if constexpr (std::is_same_v<ValueType, std::vector<TokenType>>)
            {
                appendTokenList(out, value);
            }
            else
            {
                static_assert(always_false_v<ValueType>, "Unhandled type.");
            }
        },
        param);
}

static std::string formatDiagnosticMessage(std::string_view format, const std::vector<DiagnosticParam>& params)
{
    std::string out;
    out.reserve(format.size());

    usize pos = 0;
    usize paramIndex = 0;

    while (pos < format.size())
    {
        if (format[pos] == '{' && pos + 1 < format.size() && format[pos + 1] == '}')
        {
            // If we encounter {} we need to replace it with the next parameter.
            if (paramIndex >= params.size())
            {
                out.append("<missing param>");
            }
            else
            {
                appendDiagnosticParam(out, params[paramIndex]);
            }

            // Next param, skip over the {}.
            ++paramIndex;
            pos += 2;
        }
        else
        {
            // Copy the character and advance.
            out.push_back(format[pos++]);
        }
    }

    return out;
}

static std::string getDiagnosticMessage(const Diagnostic& diag)
{
    if (diag.mFormat.empty())
    {
        return std::string{diag.mDescription};
    }

    return formatDiagnosticMessage(diag.mFormat, diag.mParams);
}

static std::string_view getDisplayFilename(std::string_view filename)
{
    // Strip any '/' or '\' in the path.
    usize pos = filename.find_last_of("/\\");
    if (pos == std::string_view::npos)
    {
        return filename;
    }

    return filename.substr(pos + 1);
}

static u8 getDigitCount(u32 number)
{
    u8 digits = 1;

    while (number >= 10)
    {
        number /= 10;
        ++digits;
    }

    return digits;
}

static usize getRenderedColumn(const std::string_view& line, u32 column)
{
    // Here we account for tabs for the column we actually want to display to the user.
    usize actualColumn = 0;
    usize maxColumn = std::min(static_cast<usize>(column), line.size());

    for (usize i = 0; i < maxColumn; ++i)
    {
        if (line[i] == '\t')
        {
            actualColumn += cTabWidth;
        }
        else
        {
            actualColumn += 1;
        }
    }

    return actualColumn;
}

static std::string formatLine(const std::string_view& sv)
{
    std::string out;
    u32 col = 0;

    for (char c : sv)
    {
        if (c == '\t')
        {
            u32 n = cTabWidth - (col % cTabWidth);
            out.append(n, ' ');
            col += n;
        }
        else
        {
            out.push_back(c);
            ++col;
        }
    }

    return out;
}

static void emitDiagnosticWithoutSource(TextWriter& out, const Diagnostic& diag, bool attached)
{
    const char* levelStr = diagnosticLevelToString(diag.mLevel);

    if (attached)
    {
        // level: msg
        out << levelStr << ": " << getDiagnosticMessage(diag) << '\n';
        return;
    }

    // level: descr
    //   fmtMsg
    out << levelStr << ": " << diag.mDescription << '\n';

    std::string formattedMessage = formatDiagnosticMessage(diag.mFormat, diag.mParams);
    if (formattedMessage.empty() == false)
    {
        out << "  " << formattedMessage << '\n';
    }
}

static void emitAttachedDiagnostic(TextWriter& out,
                                   const Diagnostic& diag,
                                   std::string_view filename,
                                   u32 lineIdx,
                                   u32 columnIdx)
{
    const char* levelStr = diagnosticLevelToString(diag.mLevel);
    std::string formattedMessage = getDiagnosticMessage(diag);

    // level: msg [file:line:column]
    out << levelStr << ": " << formattedMessage << " [" << getDisplayFilename(filename) << ":" << lineIdx << ":"
        << columnIdx << "]\n";
}

void DiagnosticManager::emit(TextWriter& out, const Diagnostic& diag, bool attached) const
{
    SourceRange range = diag.mSourceRange;
    if (range.isValid() == false)
    {
        emitDiagnosticWithoutSource(out, diag, attached);
        return;
    }

    ResolvedSourceLocation startResolved = mSourceManager.resolveLocation(range.getStartLoc());
    if (startResolved.isValid() == false)
    {
        emitDiagnosticWithoutSource(out, diag, attached);
        return;
    }

    std::string_view filename = startResolved.mSource->getFilename();
    if (filename.empty())
    {
        emitDiagnosticWithoutSource(out, diag, attached);
        return;
    }

    LineColumnInfo startInfo = startResolved.mSource->getLineAndColumnFromOffset(startResolved.mLocalOffset);

    // For the user, line and column numbers start at 1 and not at 0.
    u32 lineIdx = startInfo.mLineIndex + 1;
    u32 columnIdx = startInfo.mColumnIndex + 1;

    if (attached)
    {
        emitAttachedDiagnostic(out, diag, filename, lineIdx, columnIdx);
        return;
    }

    const char* levelStr = diagnosticLevelToString(diag.mLevel);

    // Print the diagnostic header.
    // [file:line:column]: level description
    out << "[" << filename << ":" << lineIdx << ":" << columnIdx << "]: " << levelStr << ": " << diag.mDescription
        << '\n';

    // Indent size is the number of digits in the line number.
    std::string indent(getDigitCount(lineIdx), ' ');
    usize caretOffset = getRenderedColumn(startInfo.mLine, startInfo.mColumnIndex);
    usize markerWidth = 1;

    if (range.isEmpty() == false)
    {
        // If we have a range, compute the marker width.
        ResolvedSourceLocation endResolved = mSourceManager.resolveLocation(range.getEndLoc());
        LineColumnInfo endInfo = endResolved.mSource->getLineAndColumnFromOffset(endResolved.mLocalOffset);
        if (endInfo.mLineIndex == startInfo.mLineIndex)
        {
            // Same-line ranges get an exact underline from start to end.
            usize endOffset = getRenderedColumn(startInfo.mLine, endInfo.mColumnIndex);
            markerWidth = endOffset - caretOffset;
        }
        else
        {
            // Multi-line ranges underline from the caret to the end of the first rendered line.
            std::string formattedLine = formatLine(startInfo.mLine);
            markerWidth = formattedLine.size() - caretOffset;
            if (markerWidth == 0)
            {
                markerWidth = 1;
            }
        }
    }

    std::string caretString(caretOffset, ' ');
    std::string markerString(markerWidth, '^');

    std::string formattedLine = formatLine(startInfo.mLine);
    std::string formattedMessage = formatDiagnosticMessage(diag.mFormat, diag.mParams);

    // Print a visual separator (|).
    out.write(indent).writeLine(" | ");

    // Print the line number and the line itself.
    out << lineIdx << " | " << formattedLine << '\n';

    // Print the underline and the formatted message.
    out.write(indent).write(" | ").write(caretString).write(markerString).write(" ").writeLine(formattedMessage);
}

} // namespace simlang
