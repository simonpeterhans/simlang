#pragma once

#include <cmath>
#include <limits>

#include "util/types.h"

namespace simlang
{

inline bool checkedFloatToInt(f32 value, i32& out)
{
    // If the value is NaN or infinite, bail.
    double d = value;
    if (std::isfinite(d) == false)
    {
        return false;
    }

    // If we don't fit into int range, this is also false.
    static constexpr double cMinInt = std::numeric_limits<i32>::min();
    // The upper bound is exclusive because float-to-int truncates toward zero.
    static constexpr double cMaxIntExclusive = static_cast<double>(std::numeric_limits<i32>::max()) + 1.0;
    if (d < cMinInt || d >= cMaxIntExclusive)
    {
        return false;
    }

    out = static_cast<i32>(value);

    return true;
}

} // namespace simlang
