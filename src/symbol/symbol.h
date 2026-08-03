#pragma once

#include "symbol/constvalue.h"
#include "symbol/symboltype.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

class Scope;
struct ASTNode;
struct Identifier;
struct Type;

// clang-format off
enum class SymbolFlags : u8
{
    cNone      = 0,

    cExport    = 1 << 0,
    cPrivate   = 1 << 1,
    cImpl      = 1 << 2,
    cMutable   = 1 << 3,
    cInOut     = 1 << 4,
    cConstExpr = 1 << 5,
};
// clang-format on

struct Symbol
{
    Identifier* mIdentifier = nullptr;
    ASTNode* mDeclNode = nullptr;
    Scope* mScope = nullptr;

    Type* mType = nullptr;
    ArrayView<Symbol*> mMembers;
    ConstValue mConstValue;

    // Symbol ID, assigned when registered to the table.
    i32 mID = -1;
    // Internal index, used differently depending on the symbol type.
    i32 mIndex = -1;

    SymbolType mSymbolType = SymbolType::cInvalid;
    TypedFlagSet<SymbolFlags> mFlags;

    ConstEvalState mConstEvalState = ConstEvalState::cUnprocessed;
};

} // namespace simlang
