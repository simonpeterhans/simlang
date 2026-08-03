#pragma once

#include "util/types.h"

namespace simlang
{

enum class AssignmentOp : u8
{
    cInvalid,

    cAss,    // =

    cAssAdd, // +=
    cAssSub, // -=
    cAssMul, // *=
    cAssDiv, // /=
    cAssMod, // %=

    cAssShl, // <<=
    cAssShr, // >>=

    cAssAnd, // &=
    cAssXor, // ^=
    cAssOr   // |=
};

constexpr const char* assignmentOpToString(AssignmentOp op)
{
    // clang-format off
    switch (op)
    {
        case AssignmentOp::cAss:     return "=";

        case AssignmentOp::cAssAdd:  return "+=";
        case AssignmentOp::cAssSub:  return "-=";
        case AssignmentOp::cAssMul:  return "*=";
        case AssignmentOp::cAssDiv:  return "/=";
        case AssignmentOp::cAssMod:  return "%=";

        case AssignmentOp::cAssShl:  return "<<=";
        case AssignmentOp::cAssShr:  return ">>=";

        case AssignmentOp::cAssAnd:  return "&=";
        case AssignmentOp::cAssXor:  return "^=";
        case AssignmentOp::cAssOr:   return "|=";

        default:                     return "???";
    }
    // clang-format on
}

} // namespace simlang
