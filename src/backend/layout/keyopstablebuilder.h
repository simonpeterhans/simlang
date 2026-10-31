#pragma once

#include <vector>

#include "runtime/memory/keyopstable.h"

namespace simlang
{

struct Type;

class KeyOpsTableBuilder
{
public:
    void addType(Type* type);

    KeyOpsTable build() &&;

private:
    static void collectOps(Type* type, u32 offset, std::vector<KeyOp>& ops);
    static void appendOp(std::vector<KeyOp>& ops, u32 offset, u32 words, KeyOpKind kind);

    std::vector<KeyOps> mTypes;
    std::vector<KeyOp> mOps;
};

} // namespace simlang
