#pragma once

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

struct RuntimeSourceLocation;

enum class RuntimeErrorMode : u8
{
    cLogAndContinue,
    cHaltOnError
};

enum class RuntimeErrorKind : u8
{
    cStackOverflow,
    cNullReference,
    cInvalidReference,
    cIndexOutOfBounds,
    cNegativeListCapacity,
    cNegativeMapCapacity,
    cMissingMapKey,
    cAllocationFailed,
    cInvalidCast,
    cDivisionByZero,
    cShiftOutOfRange,
    cInvalidStringHandle,
    cSyscallFailed,
    cEmptyList
};

struct RuntimeError
{
    const RuntimeSourceLocation* mSourceLocation = nullptr;
    i64 mValue0 = 0;
    i64 mValue1 = 0;
    VMAddress mBytecodeAddress = cInvalidVMAddress;
    RuntimeErrorKind mKind = RuntimeErrorKind::cNullReference;
};

constexpr const char* runtimeErrorKindToString(RuntimeErrorKind kind)
{
    switch (kind)
    {
        case RuntimeErrorKind::cStackOverflow: return "stack overflow";
        case RuntimeErrorKind::cNullReference: return "null reference";
        case RuntimeErrorKind::cInvalidReference: return "invalid reference";
        case RuntimeErrorKind::cIndexOutOfBounds: return "index out of bounds";
        case RuntimeErrorKind::cNegativeListCapacity: return "negative list capacity";
        case RuntimeErrorKind::cNegativeMapCapacity: return "negative map capacity";
        case RuntimeErrorKind::cMissingMapKey: return "missing map key";
        case RuntimeErrorKind::cAllocationFailed: return "allocation failed";
        case RuntimeErrorKind::cInvalidCast: return "invalid cast";
        case RuntimeErrorKind::cDivisionByZero: return "division by zero";
        case RuntimeErrorKind::cShiftOutOfRange: return "shift out of range";
        case RuntimeErrorKind::cInvalidStringHandle: return "invalid string handle";
        case RuntimeErrorKind::cSyscallFailed: return "syscall failed";
        case RuntimeErrorKind::cEmptyList: return "empty list";
        default: return "unknown runtime error";
    }
}

} // namespace simlang
