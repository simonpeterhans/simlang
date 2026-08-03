#pragma once

#include <vector>

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct StringLiteral
{
    u32 mOffset = 0;
    u16 mLength = 0;
};

struct StringPool
{
    std::vector<StringLiteral> mLiterals;
    std::vector<u8> mBytes;
};

enum class StringFormatArgKind : u8
{
    cInvalid,

    cInt,
    cFloat,
    cBool,
    cString,
};

constexpr const char* stringFormatArgKindToString(StringFormatArgKind kind)
{
    switch (kind)
    {
        case StringFormatArgKind::cInt: return "int";
        case StringFormatArgKind::cFloat: return "float";
        case StringFormatArgKind::cBool: return "bool";
        case StringFormatArgKind::cString: return "string";
        default: return "???";
    }
}

struct StringFormatTemplate
{
    std::vector<StringLiteralIdx> mLiteralIndices;
    // This could be extended to also contain format and flags for printing.
    std::vector<StringFormatArgKind> mArgKinds;
    u32 mLiteralByteCount = 0;
};

} // namespace simlang
