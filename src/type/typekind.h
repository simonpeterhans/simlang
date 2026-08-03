#pragma once

#include "util/types.h"

namespace simlang
{

enum class TypeKind : u8
{
    cError,
    cNull,
    cPrimitive,
    cFunction,
    cList,
    cMap,
    cInterface,
    cStruct,
    cClass,
};

enum class PrimitiveTypeKind : u8
{
    cInvalid,

    cVoid,
    cInt,
    cFloat,
    cBool,
    cString,

    cCount
};

// Useful for iterating over the valid ptks.
inline constexpr PrimitiveTypeKind cPrimitiveTypeKinds[] = {
    PrimitiveTypeKind::cVoid,
    PrimitiveTypeKind::cInt,
    PrimitiveTypeKind::cFloat,
    PrimitiveTypeKind::cBool,
    PrimitiveTypeKind::cString,
};

constexpr const char* primitiveTypeKindToString(PrimitiveTypeKind kind)
{
    switch (kind)
    {
        case PrimitiveTypeKind::cVoid: return "void";
        case PrimitiveTypeKind::cInt: return "int";
        case PrimitiveTypeKind::cFloat: return "float";
        case PrimitiveTypeKind::cBool: return "bool";
        case PrimitiveTypeKind::cString: return "string";
        default: return "???";
    }
}

} // namespace simlang
