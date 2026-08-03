#pragma once

#include <string_view>

#include "util/types.h"

namespace simlang
{

enum class BinaryOp : u8
{
    cInvalid,

    cAdd,    // +
    cSub,    // -
    cMul,    // *
    cDiv,    // /
    cMod,    // %
    cShiftL, // <<
    cShiftR, // >>

    cEQ,     // ==
    cNE,     // !=
    cLT,     // <
    cLE,     // <=
    cGT,     // >
    cGE,     // >=

    cBitAnd, // &
    cBitXor, // ^
    cBitOr,  // |

    cAnd,    // &&
    cOr      // ||
};

constexpr std::string_view binaryOpToString(BinaryOp op)
{
    // clang-format off
    switch (op)
    {
        case BinaryOp::cAdd:    return "+";
        case BinaryOp::cSub:    return "-";
        case BinaryOp::cMul:    return "*";
        case BinaryOp::cDiv:    return "/";
        case BinaryOp::cMod:    return "%";
        case BinaryOp::cShiftL: return "<<";
        case BinaryOp::cShiftR: return ">>";

        case BinaryOp::cEQ:     return "==";
        case BinaryOp::cNE:     return "!=";
        case BinaryOp::cLT:     return "<";
        case BinaryOp::cLE:     return "<=";
        case BinaryOp::cGT:     return ">";
        case BinaryOp::cGE:     return ">=";

        case BinaryOp::cBitAnd: return "&";
        case BinaryOp::cBitXor: return "^";
        case BinaryOp::cBitOr:  return "|";

        case BinaryOp::cAnd:    return "&&";
        case BinaryOp::cOr:     return "||";

        default:                return "???";
    }
    // clang-format on
}

} // namespace simlang
