#pragma once

#include "util/types.h"

namespace simlang
{

enum class SymbolType : u8
{
    cInvalid,

    cPrimitive,
    cSyscall,
    cGlobalVariable,
    cStackVariable,
    cFunction,
    cParameter,
    cStruct,
    cClass,
    cMemberVariable,
    cMemberFunction,
    cTypeTemplate,
    cInterface,
    cModule
};

constexpr const char* symbolTypeToString(SymbolType type)
{
    // clang-format off
    switch (type)
    {
        case SymbolType::cPrimitive:        return "primitive";
        case SymbolType::cSyscall:          return "syscall";
        case SymbolType::cGlobalVariable:   return "global variable";
        case SymbolType::cStackVariable:    return "stack variable";
        case SymbolType::cFunction:         return "function";
        case SymbolType::cParameter:        return "parameter";
        case SymbolType::cStruct:           return "struct";
        case SymbolType::cClass:            return "class";
        case SymbolType::cMemberVariable:   return "member variable";
        case SymbolType::cMemberFunction:   return "member function";
        case SymbolType::cTypeTemplate:     return "type template";
        case SymbolType::cInterface:        return "interface";
        case SymbolType::cModule:           return "module";

        default:                            return "???";
    }
    // clang-format on
}

} // namespace simlang
