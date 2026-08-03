#pragma once

#include <limits>

#include "util/types.h"

// clang-format off

namespace simlang
{

using VMWord           = u32;
using VMAddress        = u32;
using HeapIndex        = u32;
using SyscallIdx       = u32;
using FunctionIdx      = u32;
using InterfaceCallIdx = u16;
using GlobalIdx        = u16;
using LocalIdx         = u16;
using StringLiteralIdx = u32;
using StringFormatIdx  = u32;
using TypeID           = u16;
using FieldOffset      = u16;

using OpWordCount              = u16;
using FrameWordCount           = u16;
using ReturnWordCount          = u16;
using InterfaceMethodSlot      = u16;
using TypeLayoutWordCount      = u16;
using TypeLayoutRefCount       = u16;
using TypeLayoutRefOffset      = u16;
using TypeLayoutRefOffsetIndex = u32;

// Shared null value for reference-like VM values.
// Tagged heap handles, heap addresses, static strings, stack refs, and global refs never use 0.
inline constexpr VMWord cNullRef = 0U;

// Heap values use the top two bits and keep 30 payload bits.
// Objects, strings, and references use heap addresses.
// Resizable heap values (lists as of now) use handles.
inline constexpr VMWord cHeapKindMask           = 0xC0000000U;
inline constexpr VMWord cHeapPayloadMask        = ~cHeapKindMask;
// Heap handle:  0b10... (0x8...).
// Heap address: 0b11... (0xC...).
inline constexpr VMWord cHeapHandleTag          = 0x80000000U;
inline constexpr VMWord cHeapAddressTag         = 0xC0000000U;

// Static string literals: 0b01... (0x4...).
inline constexpr VMWord cStaticStringTag         = 0x40000000U;
inline constexpr VMWord cStaticStringTagMask     = 0xC0000000U;
inline constexpr VMWord cStaticStringPayloadMask = ~cStaticStringTagMask;

// Reference addresses: 0b00XX...
inline constexpr VMWord cRefAddressKindMask      = 0xF0000000U;
inline constexpr VMWord cRefAddressPayloadMask   = ~cRefAddressKindMask;
// Global reference addresses: 0b0001... (0x1...).
inline constexpr VMWord cGlobalRefAddressTag     = 0x10000000U;
// Stack reference addresses: 0b0010... (0x2...).
inline constexpr VMWord cStackRefAddressTag      = 0x20000000U;
// Invalid reference addresses: 0b0011... (0x3...).
inline constexpr VMWord cInvalidAddress          = 0x30000000U;

// The maximum number of stack words is determined by the ref address mask.
// This is because we need all stack addresses to be addressable.
// The count thus is the mask + 1 (include 0).
inline constexpr u32 cMaxVMStackWords = cRefAddressPayloadMask + 1;

inline constexpr VMAddress cInvalidVMAddress               = std::numeric_limits<VMAddress>::max();
inline constexpr SyscallIdx cInvalidSyscallIdx             = std::numeric_limits<SyscallIdx>::max();
inline constexpr FunctionIdx cInvalidFunctionIdx           = std::numeric_limits<FunctionIdx>::max();
inline constexpr InterfaceCallIdx cInvalidInterfaceCallIdx = std::numeric_limits<InterfaceCallIdx>::max();

inline constexpr HeapIndex cMaxHeapPayloadIndex = cHeapPayloadMask;
inline constexpr HeapIndex cMaxHeapHandleIndex  = cMaxHeapPayloadIndex;
inline constexpr u32 cMaxHeapWords              = cMaxHeapPayloadIndex + 1U;
inline constexpr u64 cMaxValidVMAddress        = static_cast<u64>(cInvalidVMAddress) - 1;
inline constexpr u64 cMaxValidInterfaceCallIdx = static_cast<u64>(cInvalidInterfaceCallIdx) - 1;

inline constexpr u64 cMaxGlobalIdx = std::numeric_limits<GlobalIdx>::max();
inline constexpr u64 cMaxLocalIdx  = std::numeric_limits<LocalIdx>::max();
inline constexpr u64 cMaxTypeID    = std::numeric_limits<TypeID>::max();

inline constexpr u64 cMaxOpWordCount         = std::numeric_limits<OpWordCount>::max();
inline constexpr u64 cMaxFrameWords          = std::numeric_limits<FrameWordCount>::max();
inline constexpr u64 cMaxReturnWords         = std::numeric_limits<ReturnWordCount>::max();
inline constexpr u64 cMaxInterfaceMethodSlot = std::numeric_limits<InterfaceMethodSlot>::max();

// These are defined by the packed layout in typelayout.h.
inline constexpr u64 cMaxTypeLayoutWordCount      = std::numeric_limits<TypeLayoutWordCount>::max();
inline constexpr u64 cMaxTypeLayoutRefCount       = std::numeric_limits<TypeLayoutRefCount>::max();
inline constexpr u64 cMaxTypeLayoutRefOffset      = std::numeric_limits<TypeLayoutRefOffset>::max();
inline constexpr u64 cMaxTypeLayoutRefOffsetIndex = (1ULL << 24) - 1;

// clang-format on

} // namespace simlang
