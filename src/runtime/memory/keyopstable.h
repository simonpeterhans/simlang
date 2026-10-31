#pragma once

#include <utility>
#include <vector>

#include "runtime/vmdefines.h"

namespace simlang
{

enum class KeyOpKind : u8
{
    cWordSpan,
    cFloat,
    cString
};

struct KeyOp
{
    FieldOffset mOffset = 0;
    OpWordCount mWordCount = 0;
    KeyOpKind mKind = KeyOpKind::cWordSpan;
};

struct KeyOps
{
    u32 mFirstOp = 0;
    OpWordCount mOpCount = 0;
};

class KeyOpsTable
{
public:
    KeyOpsTable() = default;
    KeyOpsTable(std::vector<KeyOps> types, std::vector<KeyOp> ops)
        : mTypes(std::move(types))
        , mOps(std::move(ops))
    {
    }

    const KeyOps& getOps(TypeID typeID) const { return mTypes[typeID]; }
    const KeyOp& getOp(u32 index) const { return mOps[index]; }

private:
    std::vector<KeyOps> mTypes;
    std::vector<KeyOp> mOps;
};

} // namespace simlang
