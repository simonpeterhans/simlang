#include "type/typeformat.h"

#include <string>

#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

std::string typeToString(Type* type)
{
    if (type == nullptr)
    {
        return "<null>";
    }

    switch (type->mKind)
    {
        case TypeKind::cError:
        {
            return "error";
        }

        case TypeKind::cNull:
        {
            return "null";
        }

        case TypeKind::cPrimitive:
        {
            auto* p = static_cast<const PrimitiveType*>(type);
            return primitiveTypeKindToString(p->mPrimitiveKind);
        }

        case TypeKind::cList:
        {
            auto* l = static_cast<const ListType*>(type);
            return "list<" + typeToString(l->mElement) + ">";
        }

        case TypeKind::cMap:
        {
            auto* m = static_cast<const MapType*>(type);
            return "map<" + typeToString(m->mKey) + ", " + typeToString(m->mValue) + ">";
        }

        case TypeKind::cStruct:
        case TypeKind::cClass:
        {
            auto* agg = static_cast<const AggregateType*>(type);
            if (agg->mSymbol != nullptr)
            {
                return std::string{agg->mSymbol->mIdentifier->mName, agg->mSymbol->mIdentifier->mLength};
            }

            return (type->mKind == TypeKind::cStruct) ? "<struct>" : "<class>";
        }

        case TypeKind::cInterface:
        {
            auto* ift = static_cast<const InterfaceType*>(type);
            if (ift->mSymbol != nullptr)
            {
                return std::string{ift->mSymbol->mIdentifier->mName, ift->mSymbol->mIdentifier->mLength};
            }

            return "<interface>";
        }

        case TypeKind::cFunction:
        {
            auto* f = static_cast<const FunctionType*>(type);

            std::string s = "fun(";
            for (usize i = 0; i < f->mParamTypes.size(); ++i)
            {
                if (i != 0)
                {
                    s += ", ";
                }

                const FunctionParam& param = f->mParamTypes[i];
                if (param.mIsInOut)
                {
                    s += "inout ";
                }

                s += typeToString(param.mType);
            }

            s += ") : ";
            s += typeToString(f->mReturnType);
            return s;
        }

        default:
        {
            return "<invalid type>";
        }
    }
}

} // namespace simlang
