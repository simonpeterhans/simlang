#pragma once

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct Type;

// Storage location to load/store/ref an expression from/to (not necessarily an lvalue!).
struct Place
{
    enum class Kind : u8
    {
        cInvalid,

        cLocal,
        cGlobal,
        cAddressOnStack,
    };

    static Place makeLocalPlace(Type* type, LocalIdx localIdx);
    static Place makeGlobalPlace(Type* type, GlobalIdx globalIdx);
    static Place makeAddressOnStackPlace(Type* type);

    Place derive(Type* type, VMAddress offset) const;

    void addOffset(VMAddress off) { mOffset = mOffset + off; }

    void setLocalOffset(LocalIdx idx)
    {
        mOffset = static_cast<VMAddress>(idx);
        mKind = Kind::cLocal;
    }
    LocalIdx getLocalOffset() const { return static_cast<LocalIdx>(mOffset); }

    void setGlobalOffset(GlobalIdx idx)
    {
        mOffset = static_cast<VMAddress>(idx);
        mKind = Kind::cGlobal;
    }
    GlobalIdx getGlobalOffset() const { return static_cast<GlobalIdx>(mOffset); }

    Type* mType = nullptr;
    VMAddress mOffset = 0;
    VMWord mWords = 0;
    Kind mKind = Kind::cInvalid;
};

} // namespace simlang
