#include "type/collectionmethods.h"

#include <iterator>
#include <string_view>

#include "symbol/identifier.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arrayview.h"
#include "util/asserts.h"

namespace simlang
{

struct NamedCollectionMethod
{
    std::string_view mName;
    CollectionMethod mMethod;
};

static constexpr NamedCollectionMethod cListMethods[] = {
    {"size",      CollectionMethod::cListSize     },
    {"isEmpty",   CollectionMethod::cListIsEmpty  },
    {"add",       CollectionMethod::cListAdd      },
    {"push",      CollectionMethod::cListAdd      },
    {"addList",   CollectionMethod::cListAddList  },
    {"pop",       CollectionMethod::cListPop      },
    {"back",      CollectionMethod::cListBack     },
    {"insertAt",  CollectionMethod::cListInsertAt },
    {"removeAt",  CollectionMethod::cListRemoveAt },
    {"indexOf",   CollectionMethod::cListIndexOf  },
    {"contains",  CollectionMethod::cListContains },
    {"remove",    CollectionMethod::cListRemove   },
    {"removeAll", CollectionMethod::cListRemoveAll},
    {"clear",     CollectionMethod::cListClear    },
    {"reserve",   CollectionMethod::cListReserve  },
    {"resize",    CollectionMethod::cListResize   }
};

static constexpr NamedCollectionMethod cMapMethods[] = {
    {"getKeys",     CollectionMethod::cMapGetKeys    },
    {"getValues",   CollectionMethod::cMapGetValues  },
    {"size",        CollectionMethod::cMapSize       },
    {"isEmpty",     CollectionMethod::cMapIsEmpty    },
    {"clear",       CollectionMethod::cMapClear      },
    {"containsKey", CollectionMethod::cMapContainsKey},
    {"tryGet",      CollectionMethod::cMapTryGet     },
    {"get",         CollectionMethod::cMapGet        },
    {"set",         CollectionMethod::cMapSet        },
    {"remove",      CollectionMethod::cMapRemove     },
    {"reserve",     CollectionMethod::cMapReserve    }
};

static CollectionMethod findMethod(Identifier* name, ArrayView<const NamedCollectionMethod> methods)
{
    for (const NamedCollectionMethod& method : methods)
    {
        if (identifierEquals(name, method.mName))
        {
            return method.mMethod;
        }
    }
    return CollectionMethod::cNone;
}

CollectionMethod findCollectionMethod(TypeKind receiverKind, Identifier* name)
{
    switch (receiverKind)
    {
        case TypeKind::cList: return findMethod(name, {cListMethods, std::size(cListMethods)});
        case TypeKind::cMap: return findMethod(name, {cMapMethods, std::size(cMapMethods)});
        default: return CollectionMethod::cNone;
    }
}

FunctionType* getCollectionMethodType(TypeTable& types, Type* receiver, CollectionMethod method)
{
    Type* voidType = types.getVoidType();
    Type* intType = types.getPrimitiveType(PrimitiveTypeKind::cInt);
    Type* boolType = types.getPrimitiveType(PrimitiveTypeKind::cBool);

    if (receiver->mKind == TypeKind::cList)
    {
        Type* element = static_cast<ListType*>(receiver)->mElement;
        switch (method)
        {
            case CollectionMethod::cListSize: return types.getOrAddFunction(intType, {});
            case CollectionMethod::cListIsEmpty: return types.getOrAddFunction(boolType, {});
            case CollectionMethod::cListAdd:
                return types.getOrAddFunction(voidType,
                                              {
                                                  {element, false}
                });
            case CollectionMethod::cListAddList:
                return types.getOrAddFunction(voidType,
                                              {
                                                  {receiver, false}
                });
            case CollectionMethod::cListPop:
            case CollectionMethod::cListBack: return types.getOrAddFunction(element, {});
            case CollectionMethod::cListInsertAt:
            case CollectionMethod::cListResize:
                return types.getOrAddFunction(voidType,
                                              {
                                                  {intType, false},
                                                  {element, false}
                });
            case CollectionMethod::cListRemoveAt:
                return types.getOrAddFunction(element,
                                              {
                                                  {intType, false}
                });
            case CollectionMethod::cListIndexOf:
            case CollectionMethod::cListRemoveAll:
                return types.getOrAddFunction(intType,
                                              {
                                                  {element, false}
                });
            case CollectionMethod::cListContains:
            case CollectionMethod::cListRemove:
                return types.getOrAddFunction(boolType,
                                              {
                                                  {element, false}
                });
            case CollectionMethod::cListClear: return types.getOrAddFunction(voidType, {});
            case CollectionMethod::cListReserve:
                return types.getOrAddFunction(voidType,
                                              {
                                                  {intType, false}
                });
            default: break;
        }
    }
    else if (receiver->mKind == TypeKind::cMap)
    {
        auto* map = static_cast<MapType*>(receiver);
        Type* key = map->mKey;
        Type* value = map->mValue;
        switch (method)
        {
            case CollectionMethod::cMapGetKeys: return types.getOrAddFunction(types.getOrAddList(key), {});
            case CollectionMethod::cMapGetValues: return types.getOrAddFunction(types.getOrAddList(value), {});
            case CollectionMethod::cMapSize: return types.getOrAddFunction(intType, {});
            case CollectionMethod::cMapIsEmpty: return types.getOrAddFunction(boolType, {});
            case CollectionMethod::cMapClear: return types.getOrAddFunction(voidType, {});
            case CollectionMethod::cMapContainsKey:
            case CollectionMethod::cMapRemove:
                return types.getOrAddFunction(boolType,
                                              {
                                                  {key, false}
                });
            case CollectionMethod::cMapTryGet:
                return types.getOrAddFunction(boolType,
                                              {
                                                  {key,   false},
                                                  {value, true }
                });
            case CollectionMethod::cMapGet:
                return types.getOrAddFunction(value,
                                              {
                                                  {key, false}
                });
            case CollectionMethod::cMapSet:
                return types.getOrAddFunction(voidType,
                                              {
                                                  {key,   false},
                                                  {value, false}
                });
            case CollectionMethod::cMapReserve:
                return types.getOrAddFunction(voidType,
                                              {
                                                  {intType, false}
                });
            default: break;
        }
    }

    SIMLANG_BREAK("Invalid collection method signature.");
    return nullptr;
}

} // namespace simlang
