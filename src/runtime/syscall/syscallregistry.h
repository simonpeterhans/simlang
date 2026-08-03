#pragma once

#include <string_view>
#include <type_traits>
#include <vector>

#include "runtime/syscall/nativetype.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/syscall/syscaller.h"
#include "runtime/syscall/syscalltypes.h"
#include "util/meta.h"
#include "util/types.h"

namespace simlang
{

class Scope;
struct CompilerContext;
struct Type;

class SyscallRegistry
{
public:
    struct SyscallType
    {
        NativeType mType = NativeType::cInvalid;
        NativeType mElementType = NativeType::cInvalid;
        NativeType mKeyType = NativeType::cInvalid;
        NativeType mValueType = NativeType::cInvalid;
    };

    struct SyscallTemplate
    {
        SyscallType mReturnType;
        std::vector<SyscallType> mParamTypes;
        void* mFunction = nullptr;
        bool (*mCaller)(VM& vm, const SyscallEntry& entry) = nullptr;
    };

    template <typename R, typename... Args>
    static SyscallTemplate makeSyscallTemplate(R (*func)(Args...))
    {
        SyscallTemplate tmp;

        tmp.mReturnType = getSyscallType<R>();
        (tmp.mParamTypes.push_back(getSyscallType<Args>()), ...);
        tmp.mFunction = reinterpret_cast<void*>(func);
        tmp.mCaller = &Syscaller<R, Args...>::call;

        return tmp;
    }

    bool registerSyscall(CompilerContext& ctx,
                         Scope& targetScope,
                         std::string_view name,
                         const SyscallTemplate& templateData);

    const std::vector<SyscallEntry>& getSyscalls() const { return mSyscalls; }

private:
    static Type* getSyscallType(CompilerContext& ctx, SyscallType type);

    template <typename T>
    static constexpr SyscallType getSyscallType()
    {
        using U = RemoveCVRefT<T>;

        if constexpr (std::is_same_v<U, void>)
        {
            return SyscallType{NativeType::cVoid, NativeType::cInvalid};
        }
        else if constexpr (std::is_same_v<U, i32>)
        {
            return SyscallType{NativeType::cInt, NativeType::cInvalid};
        }
        else if constexpr (std::is_same_v<U, f32>)
        {
            return SyscallType{NativeType::cFloat, NativeType::cInvalid};
        }
        else if constexpr (std::is_same_v<U, bool>)
        {
            return SyscallType{NativeType::cBool, NativeType::cInvalid};
        }
        else if constexpr (std::is_same_v<U, VMStringRef> || std::is_same_v<U, VMStringResult>)
        {
            return SyscallType{NativeType::cString, NativeType::cInvalid};
        }
        else if constexpr (is_vm_list_ref_v<U>)
        {
            using E = typename U::ElementType;
            if constexpr (std::is_same_v<E, VMStringResult>)
            {
                static_assert(always_false_v<U>,
                              "VMListRef<VMStringResult> is not allowed; use VMListRef<VMStringRef> for string list "
                              "parameters.");
            }
            else if constexpr (is_valid_vm_list_ref_element_v<E>)
            {
                NativeType elementPrimitive = getNativeType<E>();
                return SyscallType{NativeType::cListRef, elementPrimitive};
            }
            else
            {
                static_assert(always_false_v<U>,
                              "VMListRef<T> supports only i32, f32, bool, and VMStringRef element types.");
            }
        }
        else if constexpr (is_vm_list_result_v<U>)
        {
            using E = typename U::ElementType;
            if constexpr (is_valid_vm_list_result_element_v<E>)
            {
                NativeType elementPrimitive = getNativeType<E>();
                return SyscallType{NativeType::cListResult, elementPrimitive};
            }
            else
            {
                static_assert(always_false_v<U>,
                              "VMListResult<T> supports only i32, f32, bool, and VMStringResult element types.");
            }
        }
        else if constexpr (is_vm_map_ref_v<U>)
        {
            using K = typename U::KeyType;
            using V = typename U::ValueType;
            if constexpr (is_valid_vm_map_ref_key_v<K> && is_valid_vm_map_ref_value_v<V>)
            {
                return SyscallType{NativeType::cMapRef, NativeType::cInvalid, getNativeType<K>(), getNativeType<V>()};
            }
            else
            {
                static_assert(always_false_v<U>,
                              "VMMapRef<K, V> supports only i32, f32, bool, and VMStringRef key/value types.");
            }
        }
        else if constexpr (is_vm_map_result_v<U>)
        {
            using K = typename U::KeyType;
            using V = typename U::ValueType;
            if constexpr (is_valid_vm_map_result_key_v<K> && is_valid_vm_map_result_value_v<V>)
            {
                return SyscallType{NativeType::cMapResult,
                                   NativeType::cInvalid,
                                   getNativeType<K>(),
                                   getNativeType<V>()};
            }
            else
            {
                static_assert(always_false_v<U>,
                              "VMMapResult<K, V> supports only i32, f32, bool, and VMStringResult key/value types.");
            }
        }
        else
        {
            static_assert(always_false_v<U>, "Invalid syscall parameter or return type!");
        }
    }

    std::vector<SyscallEntry> mSyscalls;
};

} // namespace simlang
