#pragma once

#include "ast/nodes/astnode.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"

namespace simlang
{

inline SourceRange getSymbolSourceRange(const Symbol* symbol)
{
    if (symbol != nullptr && symbol->mDeclNode != nullptr)
    {
        return symbol->mDeclNode->mSourceRange;
    }

    return cInvalidSourceRange;
}

} // namespace simlang
