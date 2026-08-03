#pragma once

#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "type/typekind.h"
#include "type/types.h"
#include "util/hash.h"
#include "util/types.h"

namespace simlang
{

class ArenaAllocator;
struct Symbol;

struct FunctionSignatureKey
{
    bool operator==(const FunctionSignatureKey& other) const
    {
        return mReturnType == other.mReturnType && mParamTypes == other.mParamTypes;
    }

    Type* mReturnType;
    std::vector<FunctionParam> mParamTypes;
};

struct FunctionSignatureKeyHash
{
    std::size_t operator()(const FunctionSignatureKey& key) const
    {
        std::size_t h = 0;
        hashCombine(h, key.mReturnType);

        for (const FunctionParam& t : key.mParamTypes)
        {
            hashCombine(h, t.mType);
            hashCombine(h, t.mIsInOut);
        }
        return h;
    }
};

struct MapTypeKey
{
    bool operator==(const MapTypeKey& other) const { return mKey == other.mKey && mValue == other.mValue; }

    Type* mKey;
    Type* mValue;
};

struct MapTypeKeyHash
{
    std::size_t operator()(const MapTypeKey& key) const
    {
        std::size_t h = 0;
        hashCombine(h, key.mKey);
        hashCombine(h, key.mValue);
        return h;
    }
};

class TypeTable
{
public:
    explicit TypeTable(ArenaAllocator& allocator);

    FunctionType* getOrAddFunction(Type* returnType, const std::vector<FunctionParam>& paramTypes);
    ListType* getOrAddList(Type* elementType);
    MapType* getOrAddMap(Type* keyType, Type* valueType);
    AggregateType* getOrAddAggregateType(Symbol* typeSymbol);
    InterfaceType* getOrAddInterfaceType(Symbol* typeSymbol);

    PrimitiveType* getPrimitiveType(PrimitiveTypeKind kind) const;
    NullType* getNullType() const { return mNullType; }
    ErrorType* getErrorType() const { return mErrorType; }

private:
    ArenaAllocator& mAllocator;

    // Note that the first slot for cInvalid remains null.
    std::array<PrimitiveType*, static_cast<usize>(PrimitiveTypeKind::cCount)> mPrimitiveTypes{};
    NullType* mNullType = nullptr;
    ErrorType* mErrorType = nullptr;

    // Create or look up function type by signature.
    std::unordered_map<FunctionSignatureKey, FunctionType*, FunctionSignatureKeyHash> mFunctionTypes;
    // Create or look up list type by type of the element.
    std::unordered_map<Type*, ListType*> mListTypes;
    // Create or look up map type by key/value type pair.
    std::unordered_map<MapTypeKey, MapType*, MapTypeKeyHash> mMapTypes;
    // Create or look up struct/class type by symbol.
    std::unordered_map<Symbol*, AggregateType*> mAggregateTypes;
    std::unordered_map<Symbol*, InterfaceType*> mInterfaceTypes;
};

} // namespace simlang
