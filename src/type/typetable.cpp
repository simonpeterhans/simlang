#include "type/typetable.h"

#include <utility>

#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "util/arena.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/asserts.h"

namespace simlang
{

static constexpr usize cInitialFunctionTypeCapacity = 512;
static constexpr usize cInitialListTypeCapacity = 64;
static constexpr usize cInitialMapTypeCapacity = 64;
static constexpr usize cInitialAggregateTypeCapacity = 128;
static constexpr usize cInitialInterfaceTypeCapacity = 64;

TypeTable::TypeTable(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
    mFunctionTypes.reserve(cInitialFunctionTypeCapacity);
    mListTypes.reserve(cInitialListTypeCapacity);
    mMapTypes.reserve(cInitialMapTypeCapacity);
    mAggregateTypes.reserve(cInitialAggregateTypeCapacity);
    mInterfaceTypes.reserve(cInitialInterfaceTypeCapacity);

    // Create the error and null type right away.
    mErrorType = mAllocator.create<ErrorType>();
    mNullType = mAllocator.create<NullType>();

    // The same goes for the primitives.
    for (PrimitiveTypeKind ptk : cPrimitiveTypeKinds)
    {
        auto* primitiveType = mAllocator.create<PrimitiveType>(ptk);
        mPrimitiveTypes[static_cast<usize>(ptk)] = primitiveType;
    }
}

FunctionType* TypeTable::getOrAddFunction(Type* returnType, const std::vector<FunctionParam>& paramTypes)
{
    FunctionSignatureKey key{returnType, paramTypes};
    auto it = mFunctionTypes.find(key);
    if (it != mFunctionTypes.end())
    {
        return it->second;
    }

    // Convert the params from vector to ArrayView.
    ArrayView<FunctionParam> paramTypesView = makeArrayView(mAllocator, paramTypes);

    auto* funcType = mAllocator.create<FunctionType>(returnType, paramTypesView);
    mFunctionTypes[key] = funcType;

    return funcType;
}

ListType* TypeTable::getOrAddList(Type* elementType)
{
    auto it = mListTypes.find(elementType);
    if (it != mListTypes.end())
    {
        return it->second;
    }

    auto* listType = mAllocator.create<ListType>(elementType);
    mListTypes.emplace(elementType, listType);

    return listType;
}

MapType* TypeTable::getOrAddMap(Type* keyType, Type* valueType)
{
    MapTypeKey key{keyType, valueType};
    auto it = mMapTypes.find(key);
    if (it != mMapTypes.end())
    {
        return it->second;
    }

    auto* mapType = mAllocator.create<MapType>(keyType, valueType);
    mMapTypes.emplace(key, mapType);

    return mapType;
}

PrimitiveType* TypeTable::getPrimitiveType(PrimitiveTypeKind kind) const
{
    // Looks up the primitive type for the provided kind.
    // Note that the first slot for cInvalid remains null.
    // (cInvalid is not a valid primitive type kind, so this shouldn't be called with that.)

    SIMLANG_ASSERTM(kind != PrimitiveTypeKind::cInvalid && kind < PrimitiveTypeKind::cCount,
                    "Invalid primitive type kind!");

    return mPrimitiveTypes[static_cast<usize>(kind)];
}

AggregateType* TypeTable::getOrAddAggregateType(Symbol* typeSymbol)
{
    auto it = mAggregateTypes.find(typeSymbol);
    if (it != mAggregateTypes.end())
    {
        return it->second;
    }

    TypeKind kind;
    switch (typeSymbol->mSymbolType)
    {
        case SymbolType::cStruct: kind = TypeKind::cStruct; break;
        case SymbolType::cClass: kind = TypeKind::cClass; break;
        default: return nullptr;
    }

    auto* declaredType = mAllocator.create<AggregateType>(kind, typeSymbol);
    mAggregateTypes[typeSymbol] = declaredType;

    // Also set info on the type we just defined in the defining symbol.
    typeSymbol->mType = declaredType;

    return declaredType;
}

InterfaceType* TypeTable::getOrAddInterfaceType(Symbol* typeSymbol)
{
    // This goes pretty much like aggregate types.
    auto it = mInterfaceTypes.find(typeSymbol);
    if (it != mInterfaceTypes.end())
    {
        return it->second;
    }

    if (typeSymbol->mSymbolType != SymbolType::cInterface)
    {
        return nullptr;
    }

    auto* declaredType = mAllocator.create<InterfaceType>(typeSymbol);
    mInterfaceTypes[typeSymbol] = declaredType;

    typeSymbol->mType = declaredType;

    return declaredType;
}

} // namespace simlang
