#include "symbol/symbolregistry.h"

#include "symbol/symbol.h"
#include "util/arena.h"

namespace simlang
{

static constexpr usize cInitialSymbolCapacity = 1024;

SymbolRegistry::SymbolRegistry(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
    mSymbols.reserve(cInitialSymbolCapacity);
}

Symbol* SymbolRegistry::createSymbol(SymbolType type)
{
    auto* symbol = mAllocator.create<Symbol>();
    symbol->mID = static_cast<i32>(mSymbols.size());
    symbol->mSymbolType = type;

    mSymbols.push_back(symbol);

    return mSymbols.back();
}

} // namespace simlang
