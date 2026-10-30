#pragma once

#include "util/types.h"

namespace simlang
{

enum class TypeKind : u8
{
    cError,
    cNull,
    cVoid,
    cPrimitive,
    cFunction,
    cMultiReturn,
    cList,
    cMap,
    cInterface,
    cStruct,
    cClass,
};

enum class PrimitiveTypeKind : u8
{
    cInvalid,

    cInt,
    cFloat,
    cBool,
    cString,

    cCount
};

// Useful for iterating over the valid ptks.
inline constexpr PrimitiveTypeKind cPrimitiveTypeKinds[] = {
    PrimitiveTypeKind::cInt,
    PrimitiveTypeKind::cFloat,
    PrimitiveTypeKind::cBool,
    PrimitiveTypeKind::cString,
};

constexpr const char* primitiveTypeKindToString(PrimitiveTypeKind kind)
{
    switch (kind)
    {
        case PrimitiveTypeKind::cInt: return "int";
        case PrimitiveTypeKind::cFloat: return "float";
        case PrimitiveTypeKind::cBool: return "bool";
        case PrimitiveTypeKind::cString: return "string";
        default: return "???";
    }
}

} // namespace simlang
