#pragma once

#include "util/types.h"

namespace simlang
{

enum DiagnosticType : u8
{
#define X(name, level, description, format) name,

#include "diag/diagnostics.def"

#undef X

    cInvalid
};

} // namespace simlang
