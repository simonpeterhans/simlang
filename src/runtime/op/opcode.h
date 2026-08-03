#pragma once

#include "util/types.h"

namespace simlang
{

enum class OpCode : u8
{
#define X(NAME, COUNT, T1, N1, T2, N2) c##NAME,

#include "runtime/op/opcodes.def"

#undef X
};

} // namespace simlang
