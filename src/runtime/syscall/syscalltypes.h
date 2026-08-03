#pragma once

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "runtime/typeids.h"
#include "runtime/vm/vm.h"
#include "runtime/vmdefines.h"
#include "util/bitutils.h"
#include "util/meta.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

template <typename T>
struct SyscallValue
{
    static_assert(always_false_v<T>, "Unsupported VM value type for syscalls.");
};

template <>
struct SyscallValue<i32>
{
    static constexpr TypeID cTypeID = cIntTypeID;
    static constexpr u32 cWordCount = 1;

    static bool read(VM& vm, const VMWord* words, i32& out);
    static bool write(VM& vm, VMWord* words, i32 value);
    static bool makeList(VM& vm, const std::vector<i32>& values, VMWord& outRef);
};

template <>
struct SyscallValue<f32>
{
    static constexpr TypeID cTypeID = cFloatTypeID;
    static constexpr u32 cWordCount = 1;

    static bool read(VM& vm, const VMWord* words, f32& out);
    static bool write(VM& vm, VMWord* words, f32 value);
    static bool makeList(VM& vm, const std::vector<f32>& values, VMWord& outRef);
};

template <>
struct SyscallValue<bool>
{
    static constexpr TypeID cTypeID = cBoolTypeID;
    static constexpr u32 cWordCount = 1;

    static bool read(VM& vm, const VMWord* words, bool& out);
    static bool write(VM& vm, VMWord* words, bool value);
    static bool makeList(VM& vm, const std::vector<bool>& values, VMWord& outRef);
};

class VMStringRef
{
public:
    VMStringRef() = default;

    bool isValid() const;
    bool empty() const;

    const char* data() const;
    const char* c_str() const;

    u32 size() const;
    u32 length() const;

    bool getView(std::string_view& out) const;
    std::string_view view() const;

private:
    template <typename T>
    friend struct SyscallValue;

    template <typename T>
    friend struct ReadArg;

    VMStringRef(VM& vm, VMWord ref)
        : mVM(&vm)
        , mRef(ref)
    {
    }

    VM* mVM = nullptr;
    VMWord mRef = cNullRef;
};

struct VMStringResult
{
    std::string mData;
};

template <>
struct SyscallValue<VMStringRef>
{
    static constexpr TypeID cTypeID = cStringTypeID;
    static constexpr u32 cWordCount = 1;

    static bool read(VM& vm, const VMWord* words, VMStringRef& out);
    static bool write(VM& vm, VMWord* words, VMStringRef value);
    static bool makeStringResultList(VM& vm, const std::vector<VMStringResult>& values, VMWord& outRef);
};

template <typename T>
struct SyscallResultValue
{
    static_assert(always_false_v<T>, "Unsupported VM result value type for syscalls.");
};

template <>
struct SyscallResultValue<i32> : SyscallValue<i32>
{
};

template <>
struct SyscallResultValue<f32> : SyscallValue<f32>
{
};

template <>
struct SyscallResultValue<bool> : SyscallValue<bool>
{
};

template <>
struct SyscallResultValue<VMStringResult>
{
    static constexpr TypeID cTypeID = cStringTypeID;
    static constexpr u32 cWordCount = 1;
};

template <typename T>
struct is_valid_vm_list_ref_element : std::bool_constant<std::is_same_v<T, i32> || std::is_same_v<T, f32> ||
                                                         std::is_same_v<T, bool> || std::is_same_v<T, VMStringRef>>
{
};

template <typename T>
inline constexpr bool is_valid_vm_list_ref_element_v = is_valid_vm_list_ref_element<T>::value;

template <typename T>
struct is_valid_vm_list_result_element
    : std::bool_constant<std::is_same_v<T, i32> || std::is_same_v<T, f32> || std::is_same_v<T, bool> ||
                         std::is_same_v<T, VMStringResult>>
{
};

template <typename T>
inline constexpr bool is_valid_vm_list_result_element_v = is_valid_vm_list_result_element<T>::value;

template <typename T>
class VMListRef
{
public:
    using ElementType = T;

    VMListRef() = default;

    bool isValid() const { return mVM != nullptr && mHandle != cNullRef; }
    VMWord getHandle() const { return mHandle; }

    u32 size() const;
    bool isEmpty() const;

    bool reserve(u32 capacity);
    bool clear();

    bool get(u32 index, T& out) const;
    T get(u32 index) const;
    bool set(u32 index, T value);

    bool add(T value);
    bool insertAt(u32 index, T value);
    bool removeAt(u32 index, T& out);
    T removeAt(u32 index);

private:
    template <typename U>
    friend struct ReadArg;

    VMListRef(VM& vm, VMWord handle)
        : mVM(&vm)
        , mHandle(handle)
    {
    }

    VM* mVM = nullptr;
    VMWord mHandle = cNullRef;
};

template <typename K, typename V>
class VMMapRef
{
public:
    using KeyType = K;
    using ValueType = V;

    VMMapRef() = default;

    bool isValid() const { return mVM != nullptr && mHandle != cNullRef; }
    VMWord getHandle() const { return mHandle; }

    u32 size() const;
    bool isEmpty() const;

    bool reserve(u32 capacity);
    bool clear();

    bool containsKey(K key) const;
    bool get(K key, V& out) const;
    V get(K key) const;
    bool set(K key, V value);
    bool remove(K key);

private:
    template <typename U>
    friend struct ReadArg;

    VMMapRef(VM& vm, VMWord handle)
        : mVM(&vm)
        , mHandle(handle)
    {
    }

    VM* mVM = nullptr;
    VMWord mHandle = cNullRef;
};

template <typename T>
struct is_vm_list_ref : std::false_type
{
};

template <typename T>
struct is_vm_list_ref<VMListRef<T>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_vm_list_ref_v = is_vm_list_ref<T>::value;

template <typename T>
struct is_vm_map_ref : std::false_type
{
};

template <typename K, typename V>
struct is_vm_map_ref<VMMapRef<K, V>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_vm_map_ref_v = is_vm_map_ref<T>::value;

template <typename T>
struct VMListResult
{
    using ElementType = T;

    std::vector<T> mData;
};

template <typename T>
struct is_vm_list_result : std::false_type
{
};

template <typename T>
struct is_vm_list_result<VMListResult<T>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_vm_list_result_v = is_vm_list_result<T>::value;

template <typename K, typename V>
struct VMMapEntry
{
    K mKey;
    V mValue;
};

template <typename K, typename V>
struct VMMapResult
{
    using KeyType = K;
    using ValueType = V;

    std::vector<VMMapEntry<K, V>> mData;
};

template <typename T>
struct is_vm_map_result : std::false_type
{
};

template <typename K, typename V>
struct is_vm_map_result<VMMapResult<K, V>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_vm_map_result_v = is_vm_map_result<T>::value;

template <typename T>
struct is_valid_vm_map_ref_key : is_valid_vm_list_ref_element<T>
{
};

template <typename T>
inline constexpr bool is_valid_vm_map_ref_key_v = is_valid_vm_map_ref_key<T>::value;

template <typename T>
struct is_valid_vm_map_ref_value : is_valid_vm_list_ref_element<T>
{
};

template <typename T>
inline constexpr bool is_valid_vm_map_ref_value_v = is_valid_vm_map_ref_value<T>::value;

template <typename T>
struct is_valid_vm_map_result_key : is_valid_vm_list_result_element<T>
{
};

template <typename T>
inline constexpr bool is_valid_vm_map_result_key_v = is_valid_vm_map_result_key<T>::value;

template <typename T>
struct is_valid_vm_map_result_value : is_valid_vm_list_result_element<T>
{
};

template <typename T>
inline constexpr bool is_valid_vm_map_result_value_v = is_valid_vm_map_result_value<T>::value;

template <typename K, typename V>
struct SyscallMapResult
{
    static bool make(VM& vm, const std::vector<VMMapEntry<K, V>>& entries, VMWord& outRef);
};

inline bool SyscallValue<i32>::read(VM&, const VMWord* words, i32& out)
{
    out = static_cast<i32>(words[0]);
    return true;
}

inline bool SyscallValue<i32>::write(VM&, VMWord* words, i32 value)
{
    words[0] = static_cast<VMWord>(value);
    return true;
}

inline bool SyscallValue<f32>::read(VM&, const VMWord* words, f32& out)
{
    out = bits::bitCast<f32>(words[0]);
    return true;
}

inline bool SyscallValue<f32>::write(VM&, VMWord* words, f32 value)
{
    words[0] = bits::bitCast<VMWord>(value);
    return true;
}

inline bool SyscallValue<bool>::read(VM&, const VMWord* words, bool& out)
{
    out = words[0] != 0;
    return true;
}

inline bool SyscallValue<bool>::write(VM&, VMWord* words, bool value)
{
    words[0] = value ? 1U : 0U;
    return true;
}

inline bool SyscallValue<VMStringRef>::read(VM& vm, const VMWord* words, VMStringRef& out)
{
    VMWord ref = words[0];
    std::string_view sv;
    if (vm.getCheckedStringView(ref, sv, cInvalidVMAddress) == false)
    {
        out = {};
        return false;
    }

    vm.mTempRoots.push_back(ref);
    out = VMStringRef{vm, ref};

    return true;
}

inline bool SyscallValue<VMStringRef>::write(VM& vm, VMWord* words, VMStringRef value)
{
    words[0] = cNullRef;
    if (value.mVM != &vm)
    {
        return false;
    }

    std::string_view sv;
    if (value.getView(sv) == false)
    {
        return false;
    }

    words[0] = value.mRef;
    return true;
}

template <typename T>
static std::vector<VMWord> makeListWords(VM& vm, const std::vector<T>& values)
{
    using Element = SyscallValue<T>;

    std::vector<VMWord> words;
    words.resize(values.size() * Element::cWordCount);
    for (usize i = 0; i < values.size(); ++i)
    {
        Element::write(vm, words.data() + i * Element::cWordCount, values[i]);
    }

    return words;
}

inline bool SyscallValue<i32>::makeList(VM& vm, const std::vector<i32>& values, VMWord& outRef)
{
    std::vector<VMWord> words = makeListWords(vm, values);
    return vm.makeListRef(cTypeID, words.data(), static_cast<u32>(values.size()), outRef);
}

inline bool SyscallValue<f32>::makeList(VM& vm, const std::vector<f32>& values, VMWord& outRef)
{
    std::vector<VMWord> words = makeListWords(vm, values);
    return vm.makeListRef(cTypeID, words.data(), static_cast<u32>(values.size()), outRef);
}

inline bool SyscallValue<bool>::makeList(VM& vm, const std::vector<bool>& values, VMWord& outRef)
{
    std::vector<VMWord> words = makeListWords(vm, values);
    return vm.makeListRef(cTypeID, words.data(), static_cast<u32>(values.size()), outRef);
}

inline bool SyscallValue<VMStringRef>::makeStringResultList(VM& vm,
                                                            const std::vector<VMStringResult>& values,
                                                            VMWord& outRef)
{
    std::vector<std::string_view> strings;
    strings.reserve(values.size());
    for (const VMStringResult& value : values)
    {
        strings.push_back(std::string_view{value.mData.data(), value.mData.size()});
    }

    return vm.makeStringListAlloc(strings.data(), static_cast<u32>(strings.size()), outRef);
}

inline bool VMStringRef::isValid() const
{
    return mVM != nullptr && mRef != cNullRef;
}

inline bool VMStringRef::empty() const
{
    return size() == 0;
}

inline const char* VMStringRef::data() const
{
    std::string_view sv;
    if (getView(sv) == false || sv.data() == nullptr)
    {
        return "";
    }

    return sv.data();
}

inline const char* VMStringRef::c_str() const
{
    return data();
}

inline u32 VMStringRef::size() const
{
    std::string_view sv;
    if (getView(sv) == false)
    {
        return 0;
    }

    return static_cast<u32>(sv.size());
}

inline u32 VMStringRef::length() const
{
    return size();
}

inline bool VMStringRef::getView(std::string_view& out) const
{
    out = {};
    if (isValid() == false)
    {
        return false;
    }

    return mVM->getCheckedStringView(mRef, out, cInvalidVMAddress);
}

inline std::string_view VMStringRef::view() const
{
    std::string_view sv;
    getView(sv);
    return sv;
}

template <typename T>
bool makeListResult(VM& vm, const std::vector<T>& values, VMWord& outRef)
{
    static_assert(is_valid_vm_list_result_element_v<T>,
                  "VMListResult<T> supports only i32, f32, bool, and VMStringResult element types.");

    if constexpr (std::is_same_v<T, VMStringResult>)
    {
        return SyscallValue<VMStringRef>::makeStringResultList(vm, values, outRef);
    }
    else
    {
        return SyscallValue<T>::makeList(vm, values, outRef);
    }
}

template <typename T>
u32 VMListRef<T>::size() const
{
    u32 result = 0;
    mVM->getListSize(mHandle, result);
    return result;
}

template <typename T>
bool VMListRef<T>::isEmpty() const
{
    return size() == 0;
}

template <typename T>
bool VMListRef<T>::reserve(u32 capacity)
{
    return mVM->reserveList(mHandle, capacity);
}

template <typename T>
bool VMListRef<T>::clear()
{
    return mVM->clearList(mHandle);
}

template <typename T>
bool VMListRef<T>::get(u32 index, T& out) const
{
    using Element = SyscallValue<T>;

    VMWord words[Element::cWordCount]{};
    if (mVM->readListElementWords(mHandle, index, words, Element::cWordCount) == false)
    {
        return false;
    }
    return Element::read(*mVM, words, out);
}

template <typename T>
T VMListRef<T>::get(u32 index) const
{
    T value{};
    get(index, value);
    return value;
}

template <typename T>
bool VMListRef<T>::set(u32 index, T value)
{
    using Element = SyscallValue<T>;

    VMWord words[Element::cWordCount]{};
    if (Element::write(*mVM, words, value) == false)
    {
        return false;
    }
    return mVM->writeListElementWords(mHandle, index, words, Element::cWordCount);
}

template <typename T>
bool VMListRef<T>::add(T value)
{
    using Element = SyscallValue<T>;

    VMWord words[Element::cWordCount]{};
    if (Element::write(*mVM, words, value) == false)
    {
        return false;
    }
    return mVM->pushListElementWords(mHandle, words, Element::cWordCount);
}

template <typename T>
bool VMListRef<T>::insertAt(u32 index, T value)
{
    using Element = SyscallValue<T>;

    VMWord words[Element::cWordCount]{};
    if (Element::write(*mVM, words, value) == false)
    {
        return false;
    }
    return mVM->insertListElementWords(mHandle, index, words, Element::cWordCount);
}

template <typename T>
bool VMListRef<T>::removeAt(u32 index, T& out)
{
    using Element = SyscallValue<T>;

    VMWord words[Element::cWordCount]{};
    if (mVM->removeListElementWords(mHandle, index, words, Element::cWordCount) == false)
    {
        return false;
    }
    return Element::read(*mVM, words, out);
}

template <typename T>
T VMListRef<T>::removeAt(u32 index)
{
    T value{};
    removeAt(index, value);
    return value;
}

template <typename K, typename V>
u32 VMMapRef<K, V>::size() const
{
    u32 result = 0;
    mVM->getMapSize(mHandle, result);
    return result;
}

template <typename K, typename V>
bool VMMapRef<K, V>::isEmpty() const
{
    return size() == 0;
}

template <typename K, typename V>
bool VMMapRef<K, V>::reserve(u32 capacity)
{
    return mVM->reserveMap(mHandle, capacity);
}

template <typename K, typename V>
bool VMMapRef<K, V>::clear()
{
    return mVM->clearMap(mHandle);
}

template <typename K, typename V>
bool VMMapRef<K, V>::containsKey(K key) const
{
    VMWord keyWord{};
    if (SyscallValue<K>::write(*mVM, &keyWord, key) == false)
    {
        return false;
    }

    bool found = false;
    mVM->containsMapKeyWord(mHandle, keyWord, found);
    return found;
}

template <typename K, typename V>
bool VMMapRef<K, V>::get(K key, V& out) const
{
    VMWord keyWord{};
    if (SyscallValue<K>::write(*mVM, &keyWord, key) == false)
    {
        return false;
    }

    using Value = SyscallValue<V>;

    VMWord words[Value::cWordCount]{};
    if (mVM->readMapValueWords(mHandle, keyWord, words, Value::cWordCount) == false)
    {
        return false;
    }

    return Value::read(*mVM, words, out);
}

template <typename K, typename V>
V VMMapRef<K, V>::get(K key) const
{
    V value{};
    get(key, value);
    return value;
}

template <typename K, typename V>
bool VMMapRef<K, V>::set(K key, V value)
{
    VMWord keyWord{};
    if (SyscallValue<K>::write(*mVM, &keyWord, key) == false)
    {
        return false;
    }

    using Value = SyscallValue<V>;

    VMWord valueWords[Value::cWordCount]{};
    if (Value::write(*mVM, valueWords, value) == false)
    {
        return false;
    }

    return mVM->writeMapValueWords(mHandle, keyWord, valueWords, Value::cWordCount);
}

template <typename K, typename V>
bool VMMapRef<K, V>::remove(K key)
{
    VMWord keyWord{};
    if (SyscallValue<K>::write(*mVM, &keyWord, key) == false)
    {
        return false;
    }

    bool removed = false;
    mVM->removeMapKeyWord(mHandle, keyWord, removed);
    return removed;
}

template <typename K, typename V>
bool SyscallMapResult<K, V>::make(VM& vm, const std::vector<VMMapEntry<K, V>>& entries, VMWord& outRef)
{
    outRef = cNullRef;

    if (entries.size() > std::numeric_limits<u32>::max())
    {
        return false;
    }

    using Key = SyscallResultValue<K>;
    using Value = SyscallResultValue<V>;

    if (vm.makeMapRef(Key::cTypeID, Value::cTypeID, static_cast<u32>(entries.size()), outRef) == false)
    {
        return false;
    }

    if (entries.empty())
    {
        return true;
    }

    usize tempRootBase = vm.mTempRoots.size();
    vm.mTempRoots.push_back(outRef);
    OnScopeEnd restoreTempRoots{[&vm, tempRootBase]
                                {
                                    vm.mTempRoots.resize(tempRootBase);
                                }};

    auto writeResultValue = [&vm](const auto& value, VMWord* words) -> bool
    {
        using T = RemoveCVRefT<decltype(value)>;

        if constexpr (std::is_same_v<T, VMStringResult>)
        {
            words[0] = cNullRef;

            std::string_view view{value.mData.data(), value.mData.size()};
            if (vm.makeStringAlloc(view, words[0]) == false)
            {
                return false;
            }

            vm.mTempRoots.push_back(words[0]);
            return true;
        }
        else
        {
            return SyscallValue<T>::write(vm, words, value);
        }
    };

    for (const VMMapEntry<K, V>& entry : entries)
    {
        usize entryRootBase = vm.mTempRoots.size();

        VMWord keyWord{};
        VMWord valueWords[Value::cWordCount]{};
        if (writeResultValue(entry.mKey, &keyWord) == false || writeResultValue(entry.mValue, valueWords) == false ||
            vm.writeMapValueWords(outRef, keyWord, valueWords, Value::cWordCount) == false)
        {
            outRef = cNullRef;
            return false;
        }

        vm.mTempRoots.resize(entryRootBase);
    }

    return true;
}

template <typename K, typename V>
bool makeMapResult(VM& vm, const std::vector<VMMapEntry<K, V>>& entries, VMWord& outRef)
{
    return SyscallMapResult<K, V>::make(vm, entries, outRef);
}

} // namespace simlang
