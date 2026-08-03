#pragma once

#include <utility>

#include "backend/bytecode/bytecodeprogram.h"
#include "runtime/op/op.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

enum class OpCode : u8;

class BytecodeBuilder
{
public:
    using BytecodeChunkScope = ScopedValueBinder<BytecodeChunk*>;

    explicit BytecodeBuilder(ProgramBytecode& code)
        : mCode(code)
        , mCurrentCode(&code.getEntryCode())
    {
    }

    template <OpCode C, typename... Args>
    void emit(Args&&... args)
    {
        Op op = makeOp<C>(std::forward<Args>(args)...);
        getCurrentCode().emit(op, mCurrentSourceRange);
    }

    BytecodeLabel makeLabel() { return getCurrentCode().makeLabel(); }

    void setSourceRange(SourceRange sourceRange) { mCurrentSourceRange = sourceRange; }
    SourceRange getSourceRange() const { return mCurrentSourceRange; }

    BytecodeChunkScope enterFunction(FunctionIdx functionIdx)
    {
        FunctionBytecode& function = mCode.addFunction(functionIdx);
        return BytecodeChunkScope{mCurrentCode, &function.getCode()};
    }

    BytecodeChunk& getCurrentCode() { return *mCurrentCode; }
    const BytecodeChunk& getCurrentCode() const { return *mCurrentCode; }

private:
    ProgramBytecode& mCode;
    BytecodeChunk* mCurrentCode = nullptr;
    SourceRange mCurrentSourceRange = cInvalidSourceRange;
};

} // namespace simlang
