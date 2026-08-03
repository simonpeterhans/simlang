#include "symbol/constvalue.h"

#include <string_view>

#include "symbol/internedstring.h"

namespace simlang
{

bool ConstValue::operator==(const ConstValue& rhs) const
{
    if (mKind != rhs.mKind)
    {
        return false;
    }

    switch (mKind)
    {
        case ConstValueKind::cPrimitive:
        {
            if (mPrimitiveKind != rhs.mPrimitiveKind)
            {
                return false;
            }

            switch (mPrimitiveKind)
            {
                case PrimitiveTypeKind::cInt: return as.mInteger == rhs.as.mInteger;
                case PrimitiveTypeKind::cFloat: return as.mFloat == rhs.as.mFloat;
                case PrimitiveTypeKind::cBool: return as.mBool == rhs.as.mBool;
                case PrimitiveTypeKind::cString:
                {
                    // Compare string views for interned strings.
                    return as.mString->toView() == rhs.as.mString->toView();
                }
                default: return false;
            }
        }
        case ConstValueKind::cNull:
        {
            // Null values are always equal (we already checked for the type kind).
            return true;
        }
        case ConstValueKind::cStruct:
        {
            const ConstStructValue* lhsStruct = as.mStruct;
            const ConstStructValue* rhsStruct = rhs.as.mStruct;

            // Compare types and sizes.
            if (lhsStruct->mType != rhsStruct->mType || lhsStruct->mFields.size() != rhsStruct->mFields.size())
            {
                return false;
            }

            // Compare all fields.
            for (usize i = 0; i < lhsStruct->mFields.size(); ++i)
            {
                if (*lhsStruct->mFields[i].mValue != *rhsStruct->mFields[i].mValue)
                {
                    return false;
                }
            }

            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool ConstValue::operator!=(const ConstValue& rhs) const
{
    return (*this == rhs) == false;
}

} // namespace simlang
