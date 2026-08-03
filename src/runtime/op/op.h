#pragma once

#include <type_traits>
#include <utility>

#include "runtime/op/opcode.h"
#include "runtime/op/opmacros.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

// Opcode param data structs for the union in Op.
#define X(NAME, COUNT, T1, N1, T2, N2) \
    struct NAME##Data \
    { \
        SIMLANG_OP_IFGE1(COUNT, T1 N1;) \
        SIMLANG_OP_IF2(COUNT, T2 N2;) \
    };
#include "runtime/op/opcodes.def"
#undef X

struct Op
{
    // Opcode param data as union.
    union
    {
#define X(NAME, COUNT, T1, N1, T2, N2) NAME##Data m##NAME;

#include "runtime/op/opcodes.def"

#undef X
    } as;

    OpCode mOpCode;
};

// It's not a huge problem if this doesn't hold anymore, but it's still worth noting.
static_assert(sizeof(Op) == 8);

template <OpCode C>
struct OpBuilder;

// Builder template based on how many params we have; this allows something like:
// makeOp<OpCode::cSomeOpWithNoArgs>();
// makeOp<OpCode::cSomeOpWith1Arg>(arg1);
// makeOp<OpCode::cSomeOpWith2Args>(arg1, arg2);
#define X(NAME, COUNT, T1, N1, T2, N2) \
    template <> \
    struct OpBuilder<OpCode::c##NAME> \
    { \
        /* If we have 0 params, the builder takes no args and only sets the type. */ \
        SIMLANG_OP_IF0( \
            COUNT, \
            static Op build() { \
                Op o; \
                o.mOpCode = OpCode::c##NAME; \
                return o; \
            }) \
        /* If we have 1 param, make the builder take that. */ \
        SIMLANG_OP_IF1( \
            COUNT, \
            template <typename A1, typename = std::enable_if_t<std::is_same<std::decay_t<A1>, T1>::value>> \
            static Op build(A1&& N1) { \
                Op o; \
                o.mOpCode = OpCode::c##NAME; \
                o.as.m##NAME.N1 = std::forward<A1>(N1); \
                return o; \
            }) \
        /* If we have 2 params, make the builder take both of them. */ \
        SIMLANG_OP_IF2( \
            COUNT, \
            template <typename A1, \
                      typename = std::enable_if_t<std::is_same<std::decay_t<A1>, T1>::value>, \
                      typename A2, \
                      typename = std::enable_if_t<std::is_same<std::decay_t<A2>, T2>::value>> \
            static Op build(A1&& N1, A2&& N2) { \
                Op o; \
                o.mOpCode = OpCode::c##NAME; \
                o.as.m##NAME.N1 = std::forward<A1>(N1); \
                o.as.m##NAME.N2 = std::forward<A2>(N2); \
                return o; \
            }) \
    };

#include "runtime/op/opcodes.def"

#undef X

template <OpCode C, typename... Args>
Op makeOp(Args&&... args)
{
    return OpBuilder<C>::build(std::forward<Args>(args)...);
}

} // namespace simlang
