#pragma once

#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct Op;
enum class OpCode : u8;

const char* getOpCodeName(OpCode opCode);
bool hasOpOperands(OpCode opCode);
u8 getOpCodeSize(OpCode opCode);

bool isJumpOp(OpCode opCode);

bool getJumpTarget(const Op& op, VMAddress& outTarget);
bool setJumpTarget(Op& op, VMAddress target);

} // namespace simlang
