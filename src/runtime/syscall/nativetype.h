#pragma once

#include <type_traits>

#include "util/meta.h"
#include "util/types.h"

namespace simlang
{

class VMStringRef;
struct VMStringResult;

enum class NativeType : u8
{
    cInvalid,

    cVoid,
    cInt,
    cFloat,
    cBool,
    cString,
    cListRef,
    cListResult,
    cMapRef,
    cMapResult
};

union NativeData
{
    explicit constexpr NativeData(bool b)
        : mBool(b)
    {
    }
    explicit constexpr NativeData(i32 i)
        : mInteger(i)
    {
    }
    explicit constexpr NativeData(f32 f)
        : mFloat(f)
    {
    }
    explicit constexpr NativeData(const char* s)
        : mString(s)
    {
    }

    bool mBool;
    i32 mInteger = 0;
    f32 mFloat;
    const char* mString;
};

struct NativeValue
{
    NativeType mType = NativeType::cInvalid;
    NativeData as;

    static constexpr NativeValue makeBool(bool b) { return NativeValue{NativeType::cBool, NativeData{b}}; }
    static constexpr NativeValue makeInteger(i32 i) { return NativeValue{NativeType::cInt, NativeData{i}}; }
    static constexpr NativeValue makeFloat(f32 f) { return NativeValue{NativeType::cFloat, NativeData{f}}; }
    static constexpr NativeValue makeString(const char* s) { return NativeValue{NativeType::cString, NativeData{s}}; }
};

template <typename T>
constexpr NativeType getNativeType()
{
    if constexpr (std::is_same_v<T, void>)
    {
        return NativeType::cVoid;
    }
    else if constexpr (std::is_same_v<T, i32>)
    {
        return NativeType::cInt;
    }
    else if constexpr (std::is_same_v<T, f32>)
    {
        return NativeType::cFloat;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return NativeType::cBool;
    }
    else if constexpr (std::is_same_v<T, VMStringRef> || std::is_same_v<T, VMStringResult>)
    {
        return NativeType::cString;
    }
    else
    {
        static_assert(always_false_v<T>, "Invalid syscall primitive!");
    }
}

} // namespace simlang
