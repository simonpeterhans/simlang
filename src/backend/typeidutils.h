#pragma once

#include "runtime/stringdata.h"
#include "runtime/typeids.h"

namespace simlang
{

enum class PrimitiveTypeKind : u8;
struct Type;

TypeID getRuntimePrimitiveTypeID(PrimitiveTypeKind kind);
bool getRuntimeTypeID(Type* type, TypeID& out);
const char* getRuntimeBuiltinTypeIDName(TypeID id);
StringFormatArgKind getStringFormatArgKind(PrimitiveTypeKind kind);

} // namespace simlang
