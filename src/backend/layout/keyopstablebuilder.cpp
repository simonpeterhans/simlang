#include "backend/layout/keyopstablebuilder.h"

#include <utility>

#include "backend/typeidutils.h"
#include "symbol/symbol.h"
#include "type/types.h"

namespace simlang
{

void KeyOpsTableBuilder::appendOp(std::vector<KeyOp>& ops, u32 offset, u32 words, KeyOpKind kind)
{
    // If we're trying to append two consecutive word counts, merge them instead.
    if (kind == KeyOpKind::cWordSpan && ops.empty() == false)
    {
        // Get the previous one.
        KeyOp& previous = ops.back();
        // We also have to compare the offset since we have the case of interfaces ignoring the dispatch table index.
        // Since our keys restrict the interface type involved, the dispatch table index will always be the same.
        // (Otherwise, they might differ if the same object is viewed through different interfaces.)
        if (previous.mKind == KeyOpKind::cWordSpan &&
            static_cast<u32>(previous.mOffset) + previous.mWordCount == offset)
        {
            // If the offset ends where we start, extend the previous range.
            previous.mWordCount = static_cast<OpWordCount>(previous.mWordCount + words);
            return;
        }
    }

    ops.push_back({static_cast<FieldOffset>(offset), static_cast<OpWordCount>(words), kind});
}

void KeyOpsTableBuilder::collectOps(Type* type, u32 offset, std::vector<KeyOp>& ops)
{
    switch (type->mKind)
    {
        case TypeKind::cStruct:
        {
            // For structs, we collect the ops for every single field and track the offset.
            const AggregateLayout* aggregate = static_cast<AggregateType*>(type)->mLayout;
            for (const FieldLayout& field : aggregate->mFields)
            {
                // Recursively collect the ops in case of nested structs.
                collectOps(field.mSymbol->mType, offset + field.mOffset, ops);
            }
            break;
        }
        case TypeKind::cPrimitive:
        {
            // For primitives, we can do simple stuff and directly build the op.
            KeyOpKind kind = KeyOpKind::cWordSpan;
            switch (getPrimitiveKind(type))
            {
                case PrimitiveTypeKind::cFloat: kind = KeyOpKind::cFloat; break;
                case PrimitiveTypeKind::cString: kind = KeyOpKind::cString; break;
                default: break;
            }
            appendOp(ops, offset, 1, kind);
            break;
        }
        case TypeKind::cFunction:
        {
            // Here, we care about the callable token and the captured environment reference.
            appendOp(ops, offset, cFunctionValueWordCount, KeyOpKind::cWordSpan);
            break;
        }
        default:
        {
            // Reference identity uses one word, including the object word of an interface.
            appendOp(ops, offset, 1, KeyOpKind::cWordSpan);
            break;
        }
    }
}

void KeyOpsTableBuilder::addType(Type* type)
{
    TypeID typeID;
    getRuntimeTypeID(type, typeID);
    if (typeID < mTypes.size() && mTypes[typeID].mOpCount != 0)
    {
        // If we already added this type, we don't need to do anything.
        return;
    }

    // Otherwise, we need to build the ops set.
    std::vector<KeyOp> ops;
    // Collect them.
    collectOps(type, 0, ops);
    // Add the types to the registered types and append the ops to the list of ops we have.
    if (typeID >= mTypes.size())
    {
        mTypes.resize(static_cast<usize>(typeID) + 1);
    }
    mTypes[typeID] = {static_cast<u32>(mOps.size()), static_cast<OpWordCount>(ops.size())};
    mOps.insert(mOps.end(), ops.begin(), ops.end());
}

KeyOpsTable KeyOpsTableBuilder::build() &&
{
    // Move everything out.
    return KeyOpsTable{std::move(mTypes), std::move(mOps)};
}

} // namespace simlang
