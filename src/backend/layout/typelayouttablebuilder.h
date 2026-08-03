#pragma once

#include <vector>

#include "runtime/memory/typelayout.h"
#include "runtime/memory/typelayouttable.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

class TypeLayoutTableBuilder
{
public:
    void resizeLayoutTable(u32 count);
    void setLayout(TypeID id, const TypeLayout& layout);
    TypeLayoutRefOffsetIndex appendRefOffset(TypeLayoutRefOffset offset);
    void setInterfaceType(TypeID id);

    TypeLayoutRefOffsetIndex getRefOffsetCount() const;
    bool hasLayout(TypeID id) const;
    const TypeLayout& getLayout(TypeID id) const;
    bool hasRefOffsetRange(TypeLayoutRefOffsetIndex start, u32 count) const;
    TypeLayoutRefOffset getRefOffset(TypeLayoutRefOffsetIndex index) const;
    u32 getLayoutCount() const;

    TypeLayoutTable build() &&;

private:
    std::vector<TypeLayout> mLayouts;
    std::vector<TypeLayoutRefOffset> mRefOffsets;
};

} // namespace simlang
