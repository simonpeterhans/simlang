#pragma once

#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "runtime/syscall/syscallentry.h"
#include "runtime/syscall/syscalltypes.h"
#include "runtime/vm/vm.h"
#include "util/meta.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

template <typename T>
struct ReadArg
{
    using U = RemoveCVRefT<T>;

    static U read(VM& vm, const VMWord* words)
    {
        if constexpr (is_vm_list_ref_v<U>)
        {
            using E = typename U::ElementType;
            if constexpr (is_valid_vm_list_ref_element_v<E>)
            {
                // For lists, create the list proxy.
                // This will use the VM and the list handle to directly access data.
                // That way, we can directly manipulate a VM list.
                return U{vm, words[0]};
            }
            else
            {
                static_assert(always_false_v<T>,
                              "VMListRef<T> supports only i32, f32, bool, and VMStringRef element types.");
            }
        }
        else if constexpr (is_vm_map_ref_v<U>)
        {
            using K = typename U::KeyType;
            using V = typename U::ValueType;
            if constexpr (is_valid_vm_map_ref_key_v<K> && is_valid_vm_map_ref_value_v<V>)
            {
                return U{vm, words[0]};
            }
            else
            {
                static_assert(always_false_v<T>,
                              "VMMapRef<K, V> supports only i32, f32, bool, and VMStringRef key/value types.");
            }
        }
        else if constexpr (std::is_same_v<U, VMStringRef>)
        {
            VMWord ref = words[0];
            std::string_view sv;

            if (vm.getCheckedStringView(ref, sv, cInvalidVMAddress) == false)
            {
                return U{};
            }

            return U{vm, ref};
        }
        else if constexpr (std::is_same_v<U, i32>)
        {
            return static_cast<i32>(words[0]);
        }
        else if constexpr (std::is_same_v<U, f32>)
        {
            return bits::bitCast<f32>(words[0]);
        }
        else if constexpr (std::is_same_v<U, bool>)
        {
            return words[0] != 0;
        }
        else
        {
            static_assert(always_false_v<T>, "Unsupported syscall parameter type.");
        }
    }
};

template <typename... Ts>
struct ReadTuple;

template <>
struct ReadTuple<>
{
    static std::tuple<> read(VM&, const VMWord*) { return {}; }
};

template <typename T, typename... Rest>
struct ReadTuple<T, Rest...>
{
    static std::tuple<RemoveCVRefT<T>, RemoveCVRefT<Rest>...> read(VM& vm, const VMWord* words)
    {
        auto head = std::make_tuple(ReadArg<T>::read(vm, words));
        auto tail = ReadTuple<Rest...>::read(vm, words + 1);
        return std::tuple_cat(head, tail);
    }
};

template <typename R, typename... Args>
struct Syscaller
{
    static bool call(VM& vm, const SyscallEntry& entry)
    {
        // Get the function pointer that we want to call.
        auto* fn = reinterpret_cast<R (*)(Args...)>(entry.mFunction);
        // Keep the raw arguments on the VM stack while the syscall runs, so heap handles stay rooted.
        u32 stackSize = vm.mStack.getSize();
        if (entry.mArgWords > stackSize)
        {
            return false;
        }

        u32 argBase = stackSize - entry.mArgWords;
        const VMWord* argWords = vm.mStack.getData() + argBase;

        usize tempRootBase = vm.mTempRoots.size();
        OnScopeEnd restoreTempRoots{[&vm, tempRootBase]
                                    {
                                        vm.mTempRoots.resize(tempRootBase);
                                    }};

        auto args = ReadTuple<Args...>::read(vm, argWords);

        if (vm.mHalted)
        {
            vm.mStack.setSize(argBase);
            return false;
        }

        if constexpr (std::is_void_v<R>)
        {
            // Make the call.
            std::apply(fn, args);
            // Pop the args.
            vm.mStack.setSize(argBase);
            if (vm.mHalted)
            {
                return false;
            }
        }
        else if constexpr (std::is_same_v<R, i32> || std::is_same_v<R, f32> || std::is_same_v<R, bool>)
        {
            // Make the call.
            R result = std::apply(fn, args);
            // Pop the args.
            vm.mStack.setSize(argBase);
            if (vm.mHalted)
            {
                return false;
            }
            // Push the result.
            vm.push<R>(result);
        }
        else if constexpr (std::is_same_v<RemoveCVRefT<R>, VMStringResult>)
        {
            using Result = RemoveCVRefT<R>;

            // Make the call.
            Result result = std::apply(fn, args);
            if (vm.mHalted)
            {
                vm.mStack.setSize(argBase);
                return false;
            }

            // Create a string on the heap as the result.
            std::string_view resultView{result.mData.data(), result.mData.size()};
            VMWord resultRef = cNullRef;
            bool success = vm.makeStringAlloc(resultView, resultRef);

            // Pop the args.
            vm.mStack.setSize(argBase);
            // Push the result.
            vm.push<VMWord>(resultRef);

            if (success == false && vm.mHalted)
            {
                return false;
            }
        }
        else if constexpr (is_vm_list_result<RemoveCVRefT<R>>::value)
        {
            using Result = RemoveCVRefT<R>;
            using Element = typename Result::ElementType;

            // Make the call.
            Result result = std::apply(fn, args);
            if (vm.mHalted)
            {
                vm.mStack.setSize(argBase);
                return false;
            }

            // If the vector is too large, this is an error.
            if (result.mData.size() > std::numeric_limits<u32>::max())
            {
                vm.mStack.setSize(argBase);
                vm.push<VMWord>(cNullRef);
                return false;
            }

            if constexpr (is_valid_vm_list_result_element_v<Element>)
            {
                // Create the list on the heap as the result.
                VMWord resultRef = cNullRef;
                bool success = makeListResult(vm, result.mData, resultRef);

                // Pop the args.
                vm.mStack.setSize(argBase);
                // Push the result list.
                vm.push<VMWord>(resultRef);

                if (success == false && vm.mHalted)
                {
                    return false;
                }
            }
            else
            {
                static_assert(always_false_v<R>,
                              "VMListResult<T> supports only i32, f32, bool, and VMStringResult element types.");
            }
        }
        else if constexpr (is_vm_map_result<RemoveCVRefT<R>>::value)
        {
            using Result = RemoveCVRefT<R>;
            using Key = typename Result::KeyType;
            using Value = typename Result::ValueType;

            Result result = std::apply(fn, args);
            if (vm.mHalted)
            {
                vm.mStack.setSize(argBase);
                return false;
            }

            if (result.mData.size() > std::numeric_limits<u32>::max())
            {
                vm.mStack.setSize(argBase);
                vm.push<VMWord>(cNullRef);
                return false;
            }

            if constexpr (is_valid_vm_map_result_key_v<Key> && is_valid_vm_map_result_value_v<Value>)
            {
                VMWord resultRef = cNullRef;
                bool success = makeMapResult(vm, result.mData, resultRef);

                vm.mStack.setSize(argBase);
                vm.push<VMWord>(resultRef);

                if (success == false && vm.mHalted)
                {
                    return false;
                }
            }
            else
            {
                static_assert(always_false_v<R>,
                              "VMMapResult<K, V> supports only i32, f32, bool, and VMStringResult key/value types.");
            }
        }
        else
        {
            static_assert(always_false_v<R>, "Unsupported syscall return type.");
        }

        return true;
    }
};

} // namespace simlang
