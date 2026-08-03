#pragma once

#include <string_view>
#include <type_traits>
#include <utility>

#include "ast/nodes/astnode.h"
#include "diag/diagnosticmanager.h"
#include "symbol/interning.h"
#include "util/arena.h"

namespace simlang
{

class IdentifierTable;

struct ParserContext
{
    template <typename T, typename... Args>
    T* create(Args&&... args)
    {
        T* o = mAllocator.create<T>(std::forward<Args>(args)...);
        if constexpr (std::is_base_of_v<ASTNode, T>)
        {
            ++mCreatedASTNodeCount;
        }
        return o;
    }

    template <DiagnosticType DT, typename... Args>
    DiagnosticBuilder report(SourceRange range, Args&&... args)
    {
        return mDiag.report<DT>(range, std::forward<Args>(args)...);
    }

    Identifier* internIdentifier(std::string_view name, SourceRange range)
    {
        return intern::addIdentifier(mDiag, mIdentifiers, name, range);
    }

    const InternedString* internString(std::string_view value, SourceRange range)
    {
        return intern::addString(mDiag, mStrings, value, range);
    }

    ArenaAllocator& mAllocator;
    DiagnosticManager& mDiag;
    IdentifierTable& mIdentifiers;
    StringTable& mStrings;
    usize mCreatedASTNodeCount = 0;
};

} // namespace simlang
