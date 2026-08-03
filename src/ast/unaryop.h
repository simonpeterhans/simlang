#pragma once

#include "util/types.h"

namespace simlang
{

enum class UnaryOp : u8
{
    cInvalid,

    cNeg,   // -
    cBitNot // ~
};

constexpr const char* unaryOpToString(UnaryOp op)
{
    // clang-format off
    switch (op)
    {
        case UnaryOp::cNeg:    return "-";
        case UnaryOp::cBitNot: return "~";

        default:               return "???";
    }
    // clang-format on
}

} // namespace simlang
