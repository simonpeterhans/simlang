#pragma once

#include "runtime/vmdefines.h"

namespace simlang
{

struct MapAccessInfo
{
    OpWordCount mKeyWords = 0;
    FieldOffset mValueFieldOffset = 0;
    OpWordCount mValueWords = 0;
};

} // namespace simlang
