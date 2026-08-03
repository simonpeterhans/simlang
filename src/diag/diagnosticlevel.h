#pragma once

#include "util/types.h"

namespace simlang
{

enum class DiagnosticLevel : u8
{
    cInvalid,

    cError,
    cWarning,
    cNote,
    cHint,
};

constexpr const char* diagnosticLevelToString(DiagnosticLevel level)
{
    // clang-format off
    switch (level)
    {
        case DiagnosticLevel::cError:       return "error";
        case DiagnosticLevel::cWarning:     return "warning";
        case DiagnosticLevel::cNote:        return "note";
        case DiagnosticLevel::cHint:        return "hint";
            
        default:                            return "???";
    }
    // clang-format on
}

} // namespace simlang
