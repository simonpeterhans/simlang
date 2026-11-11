#pragma once

#include "type/typekind.h"
#include "util/types.h"

namespace simlang
{

struct FunctionType;
struct Identifier;
struct Type;
class TypeTable;

enum class CollectionMethod : u8
{
    cNone,

    cListSize,
    cListIsEmpty,
    cListAdd,
    cListAddList,
    cListPop,
    cListBack,
    cListInsertAt,
    cListRemoveAt,
    cListIndexOf,
    cListContains,
    cListRemove,
    cListRemoveAll,
    cListClear,
    cListReserve,
    cListResize,

    cMapGetKeys,
    cMapGetValues,
    cMapSize,
    cMapIsEmpty,
    cMapClear,
    cMapContainsKey,
    cMapTryGet,
    cMapGet,
    cMapSet,
    cMapRemove,
    cMapReserve
};

CollectionMethod findCollectionMethod(TypeKind receiverKind, Identifier* name);
FunctionType* getCollectionMethodType(TypeTable& types, Type* receiver, CollectionMethod method);

} // namespace simlang
