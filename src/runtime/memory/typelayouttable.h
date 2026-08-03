#pragma once

#include <utility>
#include <vector>

#include "runtime/memory/typelayout.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

class TypeLayoutTable
{
public:
    TypeLayoutTable() = default;
    explicit TypeLayoutTable(std::vector<TypeLayout> layouts, std::vector<TypeLayoutRefOffset> refOffsets)
        : mLayouts(std::move(layouts))
        , mRefOffsets(std::move(refOffsets))
    {
    }

    bool hasLayout(TypeID id) const { return id < mLayouts.size(); }
    const TypeLayout& getLayout(TypeID id) const { return mLayouts[id]; }

    bool hasRefOffsetRange(TypeLayoutRefOffsetIndex start, u32 count) const
    {
        const usize size = mRefOffsets.size();
        const usize startIndex = start;
        return (startIndex <= size) && (static_cast<usize>(count) <= size - startIndex);
    }

    TypeLayoutRefOffset getRefOffset(TypeLayoutRefOffsetIndex index) const { return mRefOffsets[index]; }

private:
    std::vector<TypeLayout> mLayouts;
    std::vector<TypeLayoutRefOffset> mRefOffsets;
};

} // namespace simlang
