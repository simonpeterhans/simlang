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

TypeLayoutRefOffsetIndex TypeLayoutTableBuilder::appendRefOffset(TypeLayoutRefOffset offset)
{
    TypeLayoutRefOffsetIndex refOffsetStart = static_cast<TypeLayoutRefOffsetIndex>(mRefOffsets.size());
    mRefOffsets.push_back(offset);
    return refOffsetStart;
}

void TypeLayoutTableBuilder::setInterfaceType(TypeID id)
{
    TypeLayoutRefOffsetIndex refOffsetStart = appendRefOffset(0);
    setLayout(id, TypeLayout::makeInlineValue(2, refOffsetStart, 1));
}

TypeLayoutRefOffsetIndex TypeLayoutTableBuilder::getRefOffsetCount() const
{
    return static_cast<TypeLayoutRefOffsetIndex>(mRefOffsets.size());
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
