#pragma once

#include "util/types.h"

namespace simlang
{

enum class NodeType : u8
{
#define X(name) c##name,

#include "ast/nodes/nodetypes.def"

#undef X
};

} // namespace simlang
