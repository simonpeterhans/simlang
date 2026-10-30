#pragma once

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

enum BuiltinTypeID : TypeID
{
    cInvalidTypeID,

    cIntTypeID,
    cFloatTypeID,
    cBoolTypeID,
    cStringTypeID,
    cListTypeID,
    cMapTypeID,
    cFunctionTypeID,

    cBuiltinTypeCount
};

inline constexpr u32 cFirstUserTypeID = cBuiltinTypeCount;

} // namespace simlang
