#include "backend/typeidutils.h"

#include "symbol/symbol.h"
#include "type/typekind.h"
#include "type/types.h"

namespace simlang
{

TypeID getRuntimePrimitiveTypeID(PrimitiveTypeKind kind)
{
    switch (kind)
    {
        case PrimitiveTypeKind::cVoid: return cVoidTypeID;
        case PrimitiveTypeKind::cInt: return cIntTypeID;
        case PrimitiveTypeKind::cFloat: return cFloatTypeID;
        case PrimitiveTypeKind::cBool: return cBoolTypeID;
        case PrimitiveTypeKind::cString: return cStringTypeID;
        default: return cInvalidTypeID;
    }
}

bool getRuntimeTypeID(Type* type, TypeID& out)
{
    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        {
            auto* primitiveType = static_cast<PrimitiveType*>(type);
            TypeID id = getRuntimePrimitiveTypeID(primitiveType->mPrimitiveKind);
            if (id == cInvalidTypeID)
            {
                return false;
            }

            out = id;
            return true;
        }
        case TypeKind::cList:
        {
            out = cListTypeID;
            return true;
        }
        case TypeKind::cMap:
        {
            out = cMapTypeID;
            return true;
        }
        case TypeKind::cStruct:
        case TypeKind::cClass:
        {
            auto* aggregateType = static_cast<AggregateType*>(type);
            out = static_cast<TypeID>(aggregateType->mSymbol->mIndex);
            return true;
        }
        case TypeKind::cInterface:
        {
            auto* interfaceType = static_cast<InterfaceType*>(type);
            out = static_cast<TypeID>(interfaceType->mSymbol->mIndex);
            return true;
        }
        default:
        {
            return false;
        }
    }
}

const char* getRuntimeBuiltinTypeIDName(TypeID id)
{
    switch (id)
    {
        case cVoidTypeID: return "void";
        case cIntTypeID: return "int";
        case cFloatTypeID: return "float";
        case cBoolTypeID: return "bool";
        case cStringTypeID: return "string";
        case cListTypeID: return "list";
        case cMapTypeID: return "map";
        default: return nullptr;
    }
}

StringFormatArgKind getStringFormatArgKind(PrimitiveTypeKind kind)
{
    switch (kind)
    {
        case PrimitiveTypeKind::cInt: return StringFormatArgKind::cInt;
        case PrimitiveTypeKind::cFloat: return StringFormatArgKind::cFloat;
        case PrimitiveTypeKind::cBool: return StringFormatArgKind::cBool;
        case PrimitiveTypeKind::cString: return StringFormatArgKind::cString;
        default: return StringFormatArgKind::cInvalid;
    }
}

} // namespace simlang
