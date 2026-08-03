#pragma once

#include <vector>

#include "util/types.h"

namespace simlang
{

class ArenaAllocator;
struct Symbol;
enum class SymbolType : u8;

class SymbolRegistry
{
public:
    explicit SymbolRegistry(ArenaAllocator& allocator);

    Symbol* createSymbol(SymbolType type);
    const std::vector<Symbol*>& getSymbols() const { return mSymbols; }

private:
    ArenaAllocator& mAllocator;
    std::vector<Symbol*> mSymbols;
};

} // namespace simlang
