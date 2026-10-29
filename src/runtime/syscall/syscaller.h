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
                // Same as for lists, the proxy needs the VM and the handle.
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
            // For strings, we also need the VM (to get the content) and the string handle.
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
        // Get the start stack index of the arguments (which are on top).
        u32 argBase = vm.mStack.getSize() - entry.mArgWords;
        const VMWord* argWords = vm.mStack.getData() + argBase;

        // Find out where our temporary roots currently are, and restore them once we return from this.
        // Temporary roots can currently be created when we read a string from a list or map (so the string survives
        // in case of list/map manipulation), and they can also be created for multiple heap return results.
        usize tempRootBase = vm.mTempRoots.size();
        OnScopeEnd restoreTempRoots{[&vm, tempRootBase]
                                    {
                                        vm.mTempRoots.resize(tempRootBase);
                                    }};

        // Read the args from the VM (stack).
        auto args = ReadTuple<Args...>::read(vm, argWords);

        // If we failed before somehow, bail right away.
        if (vm.mHalted)
        {
            vm.mStack.setSize(argBase);
            return false;
        }

        if constexpr (std::is_void_v<R>)
        {
            // Make the call.
            std::apply(fn, args);
            // If we have no return value, simply resize the stack to clean up.
            vm.mStack.setSize(argBase);
            return vm.mHalted == false;
        }
        else
        {
            // Make the call.
            auto result = std::apply(fn, args);

            // Check if we failed.
            if (vm.mHalted)
            {
                vm.mStack.setSize(argBase);
                return false;
            }

            // We might have multiple return values.
            if constexpr (IsSyscallTuple<RemoveCVRefT<R>>::value)
            {
                // If we do, we need to root the return values.
                // We're creating one after another, and all return results that are to be on the heap can trigger GC.
                // Thus, we need to root them or earlier results may get freed again from the GC ran by the later ones.
                usize resultBase = vm.mTempRoots.size();

                // Write all of the results to the VM and root what's necessary.
                bool success = std::apply(
                    [&vm](const auto&... values)
                    {
                        return (writeAndRootResult(vm, values) && ...);
                    },
                    result);

                // Pop the args.
                vm.mStack.setSize(argBase);

                // Check if writing them back succeeded.
                if (success == false)
                {
                    return false;
                }

                // Push the results.
                for (usize i = 0; i < entry.mReturnWords; ++i)
                {
                    vm.push<VMWord>(vm.mTempRoots[resultBase + i]);
                }

                return true;
            }
            else
            {
                // If we only have one, write that.
                VMWord word = cNullRef;
                bool success = writeResult(vm, result, word);

                // Pop the stack.
                vm.mStack.setSize(argBase);

                if (success == false)
                {
                    return false;
                }

                // Push the result.
                vm.push<VMWord>(word);

                return true;
            }
        }
    }

private:
    template <typename T>
    static bool writeAndRootResult(VM& vm, const T& value)
    {
        // Writes the value to the VM and roots the address/value received from the VM subsequently.
        VMWord word = cNullRef;
        if (writeResult(vm, value, word) == false)
        {
            return false;
        }

        vm.mTempRoots.push_back(word);

        return true;
    }

    template <typename T>
    static bool writeResult(VM& vm, const T& value, VMWord& word)
    {
        if constexpr (std::is_same_v<T, i32> || std::is_same_v<T, bool>)
        {
            word = static_cast<VMWord>(value);
            return true;
        }
        else if constexpr (std::is_same_v<T, f32>)
        {
            word = bits::bitCast<VMWord>(value);
            return true;
        }
        else if constexpr (std::is_same_v<T, VMStringResult>)
        {
            return vm.makeStringAlloc(value.mData, word);
        }
        else if constexpr (is_vm_list_result_v<T>)
        {
            if (value.mData.size() > std::numeric_limits<u32>::max())
            {
                return false;
            }
            return makeListResult(vm, value.mData, word);
        }
        else if constexpr (is_vm_map_result_v<T>)
        {
            if (value.mData.size() > std::numeric_limits<u32>::max())
            {
                return false;
            }
            return makeMapResult(vm, value.mData, word);
        }
        else
        {
            static_assert(always_false_v<T>, "Unsupported syscall return type.");
        }
    }
};

} // namespace simlang
