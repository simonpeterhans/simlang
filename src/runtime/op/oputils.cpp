#include "runtime/op/oputils.h"

#include "runtime/op/op.h"
#include "runtime/op/opcode.h"
#include "runtime/op/opmacros.h"

namespace simlang
{

const char* getOpCodeName(OpCode opCode)
{
    switch (opCode)
    {
#define X(NAME, COUNT, T1, N1, T2, N2) \
    case OpCode::c##NAME: return #NAME;
#include "runtime/op/opcodes.def"

#undef X
        default: return "???";
    }
}

bool hasOpOperands(OpCode opCode)
{
    switch (opCode)
    {
#define X(NAME, COUNT, T1, N1, T2, N2) \
    case OpCode::c##NAME: return COUNT > 0;
#include "runtime/op/opcodes.def"

#undef X
        default: return false;
    }
}

u8 getOpCodeSize(OpCode opCode)
{
    // Label is only a compile-time thing.
    if (opCode == OpCode::cLabel)
    {
        return 0;
    }

    switch (opCode)
    {
#define X(NAME, COUNT, T1, N1, T2, N2) \
    case OpCode::c##NAME: \
    { \
        return sizeof(OpCode) SIMLANG_OP_IFGE1(COUNT, +sizeof(T1)) SIMLANG_OP_IF2(COUNT, +sizeof(T2)); \
    }
#include "runtime/op/opcodes.def"

#undef X
        default: return 0;
    }
}

bool isJumpOp(OpCode opCode)
{
    switch (opCode)
    {
        case OpCode::cJump:
        case OpCode::cJumpZ:
        case OpCode::cJumpNZ:
        case OpCode::cTestZ:
        case OpCode::cTestNZ:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool getJumpTarget(const Op& op, VMAddress& outTarget)
{
    switch (op.mOpCode)
    {
        case OpCode::cJump:
        {
            outTarget = op.as.mJump.mAddress;
            return true;
        }
        case OpCode::cJumpZ:
        {
            outTarget = op.as.mJumpZ.mAddress;
            return true;
        }
        case OpCode::cJumpNZ:
        {
            outTarget = op.as.mJumpNZ.mAddress;
            return true;
        }
        case OpCode::cTestZ:
        {
            outTarget = op.as.mTestZ.mAddress;
            return true;
        }
        case OpCode::cTestNZ:
        {
            outTarget = op.as.mTestNZ.mAddress;
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool setJumpTarget(Op& op, VMAddress target)
{
    switch (op.mOpCode)
    {
        case OpCode::cJump:
        {
            op.as.mJump.mAddress = target;
            return true;
        }
        case OpCode::cJumpZ:
        {
            op.as.mJumpZ.mAddress = target;
            return true;
        }
        case OpCode::cJumpNZ:
        {
            op.as.mJumpNZ.mAddress = target;
            return true;
        }
        case OpCode::cTestZ:
        {
            op.as.mTestZ.mAddress = target;
            return true;
        }
        case OpCode::cTestNZ:
        {
            op.as.mTestNZ.mAddress = target;
            return true;
        }
        default:
        {
            return false;
        }
    }
}

} // namespace simlang
