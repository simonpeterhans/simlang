#pragma once

#include "type/typekind.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

struct AggregateType;
struct ConstValue;
struct InternedString;
struct Symbol;

struct ConstFieldValue
{
    Symbol* mField = nullptr;
    ConstValue* mValue = nullptr;
};

struct ConstStructValue
{
    AggregateType* mType = nullptr;
    ArrayView<ConstFieldValue> mFields;
};

enum class ConstEvalState : u8
{
    cUnprocessed,
    cProcessing,
    cReady,
    cFailed
};

enum class ConstValueKind : u8
{
    cInvalid,
    cPrimitive,
    cStruct,
    cNull,
};

struct ConstValue
{
    static ConstValue makeInvalid()
    {
        ConstValue value;
        value.as.mStruct = nullptr;
        value.mKind = ConstValueKind::cInvalid;
        value.mPrimitiveKind = PrimitiveTypeKind::cInvalid;
        return value;
    }

    static ConstValue makeInteger(i32 v)
    {
        ConstValue value;
        value.as.mInteger = v;
        value.mKind = ConstValueKind::cPrimitive;
        value.mPrimitiveKind = PrimitiveTypeKind::cInt;
        return value;
    }

    static ConstValue makeFloat(f32 v)
    {
        ConstValue value;
        value.as.mFloat = v;
        value.mKind = ConstValueKind::cPrimitive;
        value.mPrimitiveKind = PrimitiveTypeKind::cFloat;
        return value;
    }

    static ConstValue makeBool(bool v)
    {
        ConstValue value;
        value.as.mBool = v;
        value.mKind = ConstValueKind::cPrimitive;
        value.mPrimitiveKind = PrimitiveTypeKind::cBool;
        return value;
    }

    static ConstValue makeString(const InternedString* v)
    {
        ConstValue value;
        value.as.mString = v;
        value.mKind = ConstValueKind::cPrimitive;
        value.mPrimitiveKind = PrimitiveTypeKind::cString;
        return value;
    }

    static ConstValue makeStruct(const ConstStructValue* v)
    {
        ConstValue value;
        value.as.mStruct = v;
        value.mKind = ConstValueKind::cStruct;
        value.mPrimitiveKind = PrimitiveTypeKind::cInvalid;
        return value;
    }

    // Null is relevant for global var init, so this takes part in const eval.
    static ConstValue makeNull()
    {
        ConstValue value;
        value.as.mInteger = 0;
        value.mKind = ConstValueKind::cNull;
        value.mPrimitiveKind = PrimitiveTypeKind::cInvalid;
        return value;
    }

    bool operator==(const ConstValue& rhs) const;
    bool operator!=(const ConstValue& rhs) const;

    union
    {
        i32 mInteger;
        f32 mFloat;
        bool mBool;
        const InternedString* mString;
        const ConstStructValue* mStruct;
    } as{};
    ConstValueKind mKind = ConstValueKind::cInvalid;
    PrimitiveTypeKind mPrimitiveKind = PrimitiveTypeKind::cInvalid;
};

} // namespace simlang
