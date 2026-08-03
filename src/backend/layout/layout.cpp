#include "backend/layout/layout.h"

#include "type/typekind.h"
#include "type/types.h"
#include "util/asserts.h"

namespace simlang::layout
{

u32 getWordSizeForType(Type* type)
{
    if (type == nullptr)
    {
        SIMLANG_ASSERTM(type != nullptr, "Cannot get layout size for null type.");
        return 0;
    }

    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        {
            auto* pt = static_cast<PrimitiveType*>(type);
            switch (pt->mPrimitiveKind)
            {
                case PrimitiveTypeKind::cVoid:
                {
                    return 0;
                }
                case PrimitiveTypeKind::cInt:
                case PrimitiveTypeKind::cFloat:
                case PrimitiveTypeKind::cBool:
                case PrimitiveTypeKind::cString:
                {
                    return 1;
                }
                default:
                {
                    SIMLANG_BREAK("Invalid primitive type in layout.");
                    return 0;
                }
            }
        }
        case TypeKind::cStruct:
        {
            auto* structType = static_cast<AggregateType*>(type);
            if (structType->mLayout == nullptr)
            {
                SIMLANG_BREAK("Struct layout requested without it being laid out yet.");
                return 0;
            }
            return structType->mLayout->mSize;
        }
        case TypeKind::cFunction:
        {
            return 1;
        }
        case TypeKind::cList:
        case TypeKind::cMap:
        case TypeKind::cClass:
        {
            // Lists, maps, and classes are just the handle.
            return 1;
        }
        case TypeKind::cInterface:
        {
            // Interfaces are the object handle and the interface table index.
            return 2;
        }
        case TypeKind::cNull:
        {
            return 1;
        }
        default:
        {
            SIMLANG_BREAK("Invalid type kind in layout.");
            return 0;
        }
    }
}

u32 getStorageWordSizeForType(Type* type)
{
    u32 words = getWordSizeForType(type);
    // Storage can never be 0, so return at least 1.
    return (words == 0) ? 1 : words;
}

} // namespace simlang::layout
