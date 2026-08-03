#pragma once

#include <limits>

#include "runtime/vmdefines.h"
#include "type/typekind.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

struct Symbol;

struct Type
{
    constexpr explicit Type(TypeKind kind)
        : mKind(kind)
    {
    }

    const TypeKind mKind;
};

class ErrorType : public Type
{
public:
    ErrorType()
        : Type(TypeKind::cError)
    {
    }
};

struct NullType : Type
{
    NullType()
        : Type(TypeKind::cNull)
    {
    }
};

struct PrimitiveType : Type
{
    constexpr explicit PrimitiveType(PrimitiveTypeKind kind)
        : Type(TypeKind::cPrimitive)
        , mPrimitiveKind(kind)
    {
    }

    const PrimitiveTypeKind mPrimitiveKind = PrimitiveTypeKind::cInvalid;
};

constexpr PrimitiveTypeKind getPrimitiveKind(Type* type)
{
    if (type == nullptr || type->mKind != TypeKind::cPrimitive)
    {
        return PrimitiveTypeKind::cInvalid;
    }

    return static_cast<PrimitiveType*>(type)->mPrimitiveKind;
}

struct FunctionParam
{
    Type* mType = nullptr;
    bool mIsInOut = false;

    bool operator==(const FunctionParam& other) const { return mType == other.mType && mIsInOut == other.mIsInOut; }
};

struct FunctionType : Type
{
    constexpr explicit FunctionType(Type* returnType, ArrayView<FunctionParam> paramTypes)
        : Type(TypeKind::cFunction)
        , mReturnType(returnType)
        , mParamTypes(paramTypes)
    {
    }

    Type* mReturnType = nullptr;
    ArrayView<FunctionParam> mParamTypes;
};

struct ListType : Type
{
    constexpr explicit ListType(Type* element)
        : Type(TypeKind::cList)
        , mElement(element)
    {
    }

    Type* mElement = nullptr;
};

struct MapType : Type
{
    constexpr explicit MapType(Type* key, Type* value)
        : Type(TypeKind::cMap)
        , mKey(key)
        , mValue(value)
    {
    }

    Type* mKey = nullptr;
    Type* mValue = nullptr;
};

struct InterfaceType : Type
{
    explicit InterfaceType(Symbol* interfaceSymbol)
        : Type(TypeKind::cInterface)
        , mSymbol(interfaceSymbol)
    {
    }

    Symbol* mSymbol;
};

inline constexpr u32 cInvalidInterfaceMethodTableIndex = std::numeric_limits<u32>::max();

struct InterfaceImplementation
{
    InterfaceType* mInterface = nullptr;
    u32 mTableIndex = cInvalidInterfaceMethodTableIndex;
};

struct FieldLayout
{
    Symbol* mSymbol = nullptr;
    u32 mOffset = 0;
};

struct AggregateLayout
{
    ArrayView<FieldLayout> mFields;
    u32 mSize = 0;
};

struct AggregateType : Type
{
    explicit AggregateType(TypeKind kind, Symbol* aggregateSymbol)
        : Type(kind)
        , mSymbol(aggregateSymbol)
    {
    }

    // Track whether the aggregate consists of primitives only (primitive structs are primitives).
    // Also note that we have 1 byte from the Type base padding, so we can fit the bool here.
    bool mIsPrimitive = false;

    Symbol* mSymbol;

    // Class init method. Only used for class types.
    Symbol* mInitMethodSymbol = nullptr;

    AggregateLayout* mLayout = nullptr;
    ArrayView<InterfaceImplementation> mInterfaces;
};

} // namespace simlang
