#pragma once

#include "util/types.h"

namespace simlang
{

struct Type;

namespace layout
{

u32 getWordSizeForType(Type* type);
u32 getStorageWordSizeForType(Type* type);

} // namespace layout

} // namespace simlang
