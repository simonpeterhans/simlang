#pragma once

#include <string_view>
#include <type_traits>
#include <vector>

#include "runtime/memory/heap.h"
#include "runtime/runtimeerror.h"
#include "runtime/vm/vmstack.h"
#include "runtime/vmdefines.h"
#include "util/bitutils.h"
#include "util/meta.h"
#include "util/types.h"

namespace simlang
{

class RuntimeErrorSink;
class TextSink;
struct ExecutableImage;

class VM : Heap::RootProvider
{
public:
    explicit VM(const ExecutableImage& image);
    virtual ~VM() = default;

    bool run();

    // Output.
    void setOutput(TextSink* output) { mOutput = output; }
    TextSink* getOutput() const { return mOutput; }
    void setRuntimeErrorSink(RuntimeErrorSink* sink) { mRuntimeErrorSink = sink; }
    RuntimeErrorSink* getRuntimeErrorSink() const { return mRuntimeErrorSink; }

    // Error stuff.
    void setRuntimeErrorMode(RuntimeErrorMode mode) { mRuntimeErrorMode = mode; }
    RuntimeErrorMode getRuntimeErrorMode() const { return mRuntimeErrorMode; }
    u32 getRuntimeErrorCount() const { return mRuntimeErrorCount; }
    bool hasRuntimeErrors() const { return mRuntimeErrorCount > 0; }

    Heap::Roots makeRoots() const override;

private:
    // These helpers need access to the stack and VM internals for syscall purposes.
    template <typename R, typename... Args>
    friend struct Syscaller;

    template <typename T>
    friend struct SyscallValue;

    template <typename T>
    friend struct ReadArg;

    template <typename T>
    friend class VMListRef;

    template <typename K, typename V>
    friend class VMMapRef;

    template <typename K, typename V>
    friend struct SyscallMapResult;

    friend class VMStringRef;

    template <typename T>
    void push(T value)
    {
        static_assert(std::is_arithmetic_v<T>, "Only arithmetic types up to word size can be pushed!");
        static_assert(sizeof(T) <= sizeof(VMWord), "Type to be pushed must be at most word size!");

        VMWord bits;
        if constexpr (std::is_floating_point_v<T>)
        {
            bits = bits::bitCast<VMWord>(value);
        }
        else if constexpr (std::is_integral_v<T>)
        {
            bits = static_cast<VMWord>(value);
        }
        else
        {
            static_assert(always_false_v<T>, "Invalid type to be pushed or missing clause!");
        }

        mStack.pushWord(bits);
    }

    template <typename T>
    T pop()
    {
        static_assert(std::is_arithmetic_v<T>, "Only arithmetic types up to word size can be popped!");
        static_assert(sizeof(T) <= sizeof(VMWord), "Type to be popped must be at most word size!");

        VMWord bits = mStack.popWord();

        if constexpr (std::is_floating_point_v<T>)
        {
            T value = bits::bitCast<T>(bits);
            return value;
        }
        else if constexpr (std::is_integral_v<T>)
        {
            return static_cast<T>(bits);
        }
        else
        {
            static_assert(always_false_v<T>, "Invalid type to be popped or missing clause!");
        }
    }

    struct CallFrame
    {
        // Function index within the function table.
        FunctionIdx mFunctionIdx = cInvalidFunctionIdx;
        // Caller stack/frame pointers (indices into the stack).
        u32 mCallerSP = 0;
        u32 mCallerFP = 0;
        // Return program counter (address in bytecode).
        VMAddress mReturnPC = 0;
    };

    void initialize();

    SIMLANG_NOINLINE void reportRuntimeError(VMAddress address, RuntimeErrorKind kind, i64 value0 = 0, i64 value1 = 0);
    SIMLANG_NOINLINE void reportReferenceRuntimeError(VMAddress address, VMWord value);
    SIMLANG_NOINLINE void reportStringRuntimeError(VMAddress address, VMWord value);
    SIMLANG_NOINLINE void reportStackOverflow(VMAddress address, u64 requiredWords);

    // List construction.
    bool makeListRef(TypeID typeID, const VMWord* words, u32 length, VMWord& outRef);

    // Map construction.
    bool makeMapRef(TypeID keyTypeID, TypeID valueTypeID, u32 capacity, VMWord& outRef);

    // List validation/capacity helpers.
    bool getCheckedListBlock(VMWord handle, ListBlock& out, VMAddress address);
    bool ensureListCapacity(VMWord handle, ListBlock& block, u32 minCapacity, VMAddress address);

    // List operations used by syscalls.
    bool getListSize(VMWord handle, u32& outSize);
    bool reserveList(VMWord handle, u32 capacity);
    bool clearList(VMWord handle);
    bool readListElementWords(VMWord handle, u32 index, VMWord* dst, u32 wordCount);
    bool writeListElementWords(VMWord handle, u32 index, const VMWord* src, u32 wordCount);
    bool pushListElementWords(VMWord handle, const VMWord* src, u32 wordCount);
    bool insertListElementWords(VMWord handle, u32 index, const VMWord* src, u32 wordCount);
    bool removeListElementWords(VMWord handle, u32 index, VMWord* dst, u32 wordCount);

    // Map validation/capacity helpers.
    bool getCheckedMapBlock(VMWord handle, MapBlock& out, VMAddress address);
    bool ensureMapCapacity(VMWord handle, MapBlock& block, u32 minCapacity, VMAddress address);
    bool hashMapKey(TypeID keyTypeID, VMWord key, VMAddress address, u32& outHash);
    bool mapKeysEqual(TypeID keyTypeID, VMWord lhs, VMWord rhs, VMAddress address, bool& out);
    bool findMapEntry(const MapBlock& block, VMWord key, u32 hash, VMAddress address, u32& outIndex, bool& outFound);

    // Map operations used by syscalls.
    bool getMapSize(VMWord handle, u32& outSize);
    bool reserveMap(VMWord handle, u32 capacity);
    bool clearMap(VMWord handle);
    bool containsMapKeyWord(VMWord handle, VMWord key, bool& out);
    bool readMapValueWords(VMWord handle, VMWord key, VMWord* dst, u32 wordCount);
    bool writeMapValueWords(VMWord handle, VMWord key, const VMWord* src, u32 wordCount);
    bool removeMapKeyWord(VMWord handle, VMWord key, bool& outRemoved);

    // String allocation used by syscalls.
    bool makeStringAlloc(std::string_view str, VMWord& outRef);
    bool makeStringListAlloc(const std::string_view* strings, u32 length, VMWord& outRef);

    // String utility.
    bool getUncheckedStringView(VMWord value, std::string_view& out);
    bool getCheckedStringView(VMWord value, std::string_view& out, VMAddress address);
    bool stringsEqualChecked(VMWord a, VMWord b, VMAddress address, bool& out);
    bool stringsEqual(VMWord a, VMWord b, VMAddress address);

    // Waste some bytes here since mHalted is very much on the hot path.
    // Does it actually matter?
    bool mHalted = false;

    std::vector<CallFrame> mCallStack;
    std::vector<VMWord> mGlobals;
    std::vector<VMWord> mTempRoots;

    VMStack mStack;
    Heap mHeap;

    const ExecutableImage& mImage;

    TextSink* mOutput;

    RuntimeErrorSink* mRuntimeErrorSink = nullptr;
    RuntimeErrorMode mRuntimeErrorMode = RuntimeErrorMode::cHaltOnError;
    u32 mRuntimeErrorCount = 0;
};

} // namespace simlang
