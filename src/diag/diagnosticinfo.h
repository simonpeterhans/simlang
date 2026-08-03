#pragma once

#include <type_traits>

#include "diag/diagnosticformat.h"
#include "diag/diagnosticlevel.h"
#include "diag/diagnostictype.h"

namespace simlang
{

struct DiagnosticInfo
{
    constexpr DiagnosticInfo(DiagnosticLevel level, const char* description, DiagnosticFormat format)
        : mFormat(format)
        , mDescription(description)
        , mLevel(level)
    {
    }

    DiagnosticFormat mFormat;
    const char* mDescription;
    DiagnosticLevel mLevel;
};

inline constexpr DiagnosticInfo cDiagnosticTypeInfos[] = {
#define X(name, level, description, format) {DiagnosticLevel::level, description, DiagnosticFormat(format)},

#include "diag/diagnostics.def"

#undef X
};

constexpr const DiagnosticFormat& getFormatForType(DiagnosticType type)
{
    return cDiagnosticTypeInfos[static_cast<std::underlying_type_t<DiagnosticType>>(type)].mFormat;
}

} // namespace simlang
