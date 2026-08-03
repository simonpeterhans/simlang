#pragma once

#include <memory>
#include <string_view>
#include <type_traits>

#include "runtime/executableimage.h"
#include "runtime/syscall/nativetype.h"
#include "runtime/syscall/syscallregistry.h"
#include "util/meta.h"
#include "util/types.h"

namespace simlang
{

class TextSink;
struct CompilerContext;
struct CompilerOptions;

class Compiler
{
public:
    Compiler();
    explicit Compiler(TextSink& output);
    ~Compiler();

    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    template <typename T>
    bool registerConst(std::string_view name, T value)
    {
        return registerConst(std::string_view{}, name, makeNativeValue(value));
    }

    template <typename T>
    bool registerConst(std::string_view moduleName, std::string_view name, T value)
    {
        return registerConst(moduleName, name, makeNativeValue(value));
    }

    template <typename R, typename... Args>
    bool registerSyscall(std::string_view name, R (*func)(Args...))
    {
        return registerSyscall(std::string_view{}, name, SyscallRegistry::makeSyscallTemplate(func));
    }

    template <typename R, typename... Args>
    bool registerSyscall(std::string_view moduleName, std::string_view name, R (*func)(Args...))
    {
        return registerSyscall(moduleName, name, SyscallRegistry::makeSyscallTemplate(func));
    }

    std::unique_ptr<ExecutableImage> compile(const CompilerOptions& options);

    void emitDiagnostics() const;

private:
    bool registerConst(std::string_view moduleName, std::string_view name, NativeValue value);
    bool registerSyscall(std::string_view moduleName,
                         std::string_view name,
                         const SyscallRegistry::SyscallTemplate& syscall);

    void registerPrimitives();

    template <typename T>
    static constexpr NativeValue makeNativeValue(T value)
    {
        using U = RemoveCVRefT<T>;

        if constexpr (std::is_same_v<U, i32>)
        {
            return NativeValue::makeInteger(value);
        }
        else if constexpr (std::is_same_v<U, f32>)
        {
            return NativeValue::makeFloat(value);
        }
        else if constexpr (std::is_same_v<U, bool>)
        {
            return NativeValue::makeBool(value);
        }
        else if constexpr (std::is_same_v<U, const char*>)
        {
            return NativeValue::makeString(value);
        }
        else
        {
            static_assert(always_false_v<T>, "Invalid primitive to be registered!");
        }
    }

    std::unique_ptr<CompilerContext> mCtx;
};

} // namespace simlang
