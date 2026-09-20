#include "backend/layout/typelayouttablebuilder.h"

#include <utility>

namespace simlang
{

void TypeLayoutTableBuilder::resizeLayoutTable(u32 count)
{
    mLayouts.resize(count, TypeLayout::makePrimitive());
}

void TypeLayoutTableBuilder::setLayout(TypeID id, const TypeLayout& layout)
{
    if (id >= mLayouts.size())
    {
        mLayouts.resize(id + 1, TypeLayout::makePrimitive());
    }

    mLayouts[id] = layout;
}

bool TypeLayoutTableBuilder::tryAppendRefOffsets(ArrayView<const u32> offsets,
                                                 TypeLayoutRefOffsetIndex& outStart,
                                                 u64& outRequiredIndex)
{
    outStart = 0;
    outRequiredIndex = 0;

    if (offsets.empty())
    {
        return true;
    }

    // See how big we need to grow our ref size array.
    u64 requiredEnd = mRefOffsets.size() + offsets.size();
    // Set the required size for diagnostic reasons in case we fail.
    outRequiredIndex = requiredEnd - 1;

    if (requiredEnd > cMaxTypeLayoutRefOffsetIndex + 1)
    {
        return false;
    }

    // Set the start index to the current size.
    outStart = static_cast<TypeLayoutRefOffsetIndex>(mRefOffsets.size());

    // Append stuff.
    for (u32 offset : offsets)
    {
        mRefOffsets.push_back(static_cast<TypeLayoutRefOffset>(offset));
    }

    return true;
}

bool TypeLayoutTableBuilder::hasLayout(TypeID id) const
{
    return id < mLayouts.size();
}

const TypeLayout& TypeLayoutTableBuilder::getLayout(TypeID id) const
{
    return mLayouts[id];
}

bool TypeLayoutTableBuilder::hasRefOffsetRange(TypeLayoutRefOffsetIndex start, u32 count) const
{
    const usize size = mRefOffsets.size();
    const usize startIndex = start;
    return startIndex <= size && static_cast<usize>(count) <= size - startIndex;
}

TypeLayoutRefOffset TypeLayoutTableBuilder::getRefOffset(TypeLayoutRefOffsetIndex index) const
{
    return mRefOffsets[index];
}

u32 TypeLayoutTableBuilder::getLayoutCount() const
{
    return static_cast<u32>(mLayouts.size());
}

TypeLayoutTable TypeLayoutTableBuilder::build() &&
{
    return TypeLayoutTable{std::move(mLayouts), std::move(mRefOffsets)};
}

} // namespace simlang
