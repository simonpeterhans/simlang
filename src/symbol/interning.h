#pragma once

#include <string_view>

#include "diag/diagnosticmanager.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/identifiertable.h"
#include "symbol/internedstring.h"
#include "symbol/stringtable.h"

namespace simlang::intern
{

inline Identifier* addIdentifier(DiagnosticManager& diag,
                                 IdentifierTable& identifiers,
                                 std::string_view name,
                                 SourceRange range)
{
    if (name.empty())
    {
        return nullptr;
    }

    if (name.length() > cMaxIdentifierLen)
    {
        diag.report<cIdentifierTooLong>(range, name, static_cast<u32>(cMaxIdentifierLen));
        return nullptr;
    }

    return identifiers.addIdentifier(name);
}

inline const InternedString* addString(DiagnosticManager& diag,
                                       StringTable& strings,
                                       std::string_view value,
                                       SourceRange range)
{
    if (value.length() > cMaxInternedStringLength)
    {
        diag.report<cStringLiteralTooLong>(range, value.length(), cMaxInternedStringLength);
        return nullptr;
    }

    return strings.addString(value);
}

} // namespace simlang::intern
