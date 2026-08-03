#pragma once

#include "runtime/vmdefines.h"
#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

struct TypeLayout
{
    enum class Kind : u8
    {
        cInline,   // Primitive or struct.
        cReference // Class or list.
    };

    // Layout:
    // [ 0..15] (16) refCount       Number of reference words contained in the value/payload.
    // [16..39] (24) refOffsetStart Index of the first entry in the reference-offset table.
    // [40..55] (16) wordCount      Size of the type in words.
    // [56..56] ( 1) kind           Whether the type is stored inline or as a reference.
    // [57..63] unused

    static constexpr u64 cRefCountMask = cMaxTypeLayoutRefCount;
    static constexpr u64 cRefOffsetStartIndexMask = cMaxTypeLayoutRefOffsetIndex;
    static constexpr u64 cWordCountMask = cMaxTypeLayoutWordCount;
    static constexpr u64 cKindMask = 0x1;

    TypeLayoutRefCount getRefCount() const { return static_cast<TypeLayoutRefCount>((mBits >> 0) & cRefCountMask); }
    TypeLayoutRefOffsetIndex getRefOffsetStartIndex() const
    {
        return static_cast<TypeLayoutRefOffsetIndex>((mBits >> 16) & cRefOffsetStartIndexMask);
    }
    TypeLayoutWordCount getWordCount() const
    {
        return static_cast<TypeLayoutWordCount>((mBits >> 40) & cWordCountMask);
    }
    Kind getKind() const { return static_cast<Kind>((mBits >> 56) & cKindMask); }

    TypeLayoutWordCount getSizeAsValue() const
    {
        switch (getKind())
        {
            case Kind::cInline:
            {
                SIMLANG_ASSERT(getWordCount() > 0);
                return getWordCount();
            }
            case Kind::cReference:
            {
                return 1;
            };
            default:
            {
                SIMLANG_BREAK("Invalid TypeLayout kind!");
                return 0;
            }
        }
    }

    TypeLayoutWordCount getSizeOnHeap() const
    {
        if (getKind() == Kind::cReference)
        {
            SIMLANG_ASSERT(getWordCount() > 0);
            return getWordCount();
        }

        SIMLANG_BREAK("Only reference types have a heap payload size!");
        return 0;
    }

    static TypeLayout pack(TypeLayoutWordCount wordCount,
                           Kind kind,
                           TypeLayoutRefOffsetIndex refOffsetStart,
                           TypeLayoutRefCount refCount)
    {
        TypeLayout r;
        // clang-format off
        r.mBits = static_cast<u64>(refCount)
               | (static_cast<u64>(refOffsetStart & cRefOffsetStartIndexMask) << 16)
               | (static_cast<u64>(wordCount) << 40)
               | ((static_cast<u64>(kind) & cKindMask) << 56);
        // clang-format on
        return r;
    }

    static TypeLayout makeInlineValue(TypeLayoutWordCount inlineWords,
                                      TypeLayoutRefOffsetIndex refOffsetStart,
                                      TypeLayoutRefCount refCount)
    {
        return pack(inlineWords, Kind::cInline, refOffsetStart, refCount);
    }

    static TypeLayout makePrimitive() { return makeInlineValue(1, 0, 0); }

    static TypeLayout makeStruct(TypeLayoutWordCount inlineWords,
                                 TypeLayoutRefOffsetIndex refOffsetStart,
                                 TypeLayoutRefCount refCount)
    {
        return makeInlineValue(inlineWords, refOffsetStart, refCount);
    }

    static TypeLayout makeReferenceObject(TypeLayoutWordCount payloadWords,
                                          TypeLayoutRefOffsetIndex refOffsetStart,
                                          TypeLayoutRefCount refCount)
    {
        return pack(payloadWords, Kind::cReference, refOffsetStart, refCount);
    }

    u64 mBits = 0;
};

} // namespace simlang
