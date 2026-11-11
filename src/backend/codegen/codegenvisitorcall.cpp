#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/layout/layout.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/collectionmethods.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

void CodeGenVisitor::emitCallableValue(Symbol* symbol)
{
    // A callable without captures has a null environment and a converted index as its entry token.
    emitIntegerImmediate(cNullRef);

    VMWord entryToken;
    if (symbol->mSymbolType == SymbolType::cSyscall)
    {
        entryToken =
            makeSyscallEntryToken(mCtx.mBackend.mFunctionInfos.size(), static_cast<SyscallIdx>(symbol->mIndex));
    }
    else
    {
        entryToken = makeFunctionEntryToken(static_cast<FunctionIdx>(symbol->mIndex));
    }

    emit<OpCode::cPush32>(entryToken);
}

bool CodeGenVisitor::emitCallArguments(ArrayView<CallArgument> args, const FunctionType* funcType)
{
    // Go over all the args.
    for (usize i = 0; i < args.size(); ++i)
    {
        const FunctionParam& param = funcType->mParamTypes[i];

        // If the param is an in-out param, emit the address of the arg instead of the value.
        if (param.mIsInOut)
        {
            if (emitAddress(args[i].mValue, AddressMode::cStorage) == false)
            {
                return false;
            }
        }
        else
        {
            if (visit(args[i].mValue) == false)
            {
                return false;
            }
        }
    }

    return true;
}

bool CodeGenVisitor::emitMethodReceiver(MemberAccessNode* memberAccess)
{
    // If the receiver is a struct, emit the address of the struct (so it behaves like "this").
    Type* receiverType = memberAccess->mReceiver->mResolvedType;
    if (receiverType->mKind == TypeKind::cStruct)
    {
        return emitStructReceiverAddress(memberAccess->mReceiver);
    }

    return visit(memberAccess->mReceiver);
}

bool CodeGenVisitor::emitInterfaceMethodDispatch(MemberAccessNode* memberAccess, const FunctionType* funcType)
{
    // Here, we want to look up the interface method slot for an interface given a call.
    // When we get here, we assume the receiver, the interface table index, and the args are already on the stack.
    // Find out how many arg words we have for the call.
    u64 argWords = 0;
    for (const FunctionParam& param : funcType->mParamTypes)
    {
        argWords += param.mIsInOut ? 1U : layout::getWordSizeForType(param.mType);
    }
    if (argWords > cMaxOpWordCount)
    {
        mCtx.report<cFunctionFrameTooLarge>(memberAccess->mSourceRange, argWords, "argument", cMaxOpWordCount);
        return false;
    }

    u32 returnWords = layout::getWordSizeForType(funcType->mReturnType);
    if (returnWords > cMaxReturnWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(memberAccess->mSourceRange, returnWords, "return", cMaxReturnWords);
        return false;
    }

    // The interface method slot we can simply take from the member access node.
    auto slot = static_cast<InterfaceMethodSlot>(memberAccess->mSymbol->mIndex);
    // Then, obtain the call index so that at runtime we can obtain the information needed for the call.
    InterfaceCallIdx callIndex = getInterfaceCallIndex(memberAccess->mSourceRange,
                                                       slot,
                                                       static_cast<OpWordCount>(argWords),
                                                       static_cast<ReturnWordCount>(returnWords));
    if (callIndex == cInvalidInterfaceCallIdx)
    {
        return false;
    }

    // Finally emit the index for the call info.
    emit<OpCode::cCallInterface>(callIndex);

    return true;
}

bool CodeGenVisitor::emitCollectionMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess)
{
    SIMLANG_ASSERT(memberAccess->mCollectionMethod != CollectionMethod::cNone);
    auto* funcType = static_cast<FunctionType*>(memberAccess->mResolvedType);

    if (visit(memberAccess->mReceiver) == false || emitCallArguments(node->mArgs, funcType) == false)
    {
        return false;
    }

    Type* receiverType = memberAccess->mReceiver->mResolvedType;
    auto* listType = receiverType->mKind == TypeKind::cList ? static_cast<ListType*>(receiverType) : nullptr;
    auto* mapType = receiverType->mKind == TypeKind::cMap ? static_cast<MapType*>(receiverType) : nullptr;
    OpWordCount elementWords = 0;
    OpWordCount valueWords = 0;
    if (listType != nullptr)
    {
        elementWords = static_cast<OpWordCount>(layout::getWordSizeForType(listType->mElement));
    }
    if (mapType != nullptr)
    {
        valueWords = static_cast<OpWordCount>(layout::getWordSizeForType(mapType->mValue));
    }

    switch (memberAccess->mCollectionMethod)
    {
        case CollectionMethod::cListSize: emit<OpCode::cListSize>(); return true;
        case CollectionMethod::cListIsEmpty: emit<OpCode::cListIsEmpty>(); return true;
        case CollectionMethod::cListAdd: emit<OpCode::cListPush>(elementWords); return true;
        case CollectionMethod::cListAddList: emit<OpCode::cListAddList>(); return true;
        case CollectionMethod::cListPop: emit<OpCode::cListPop>(elementWords); return true;
        case CollectionMethod::cListBack: emit<OpCode::cListBack>(elementWords); return true;
        case CollectionMethod::cListInsertAt: emit<OpCode::cListInsert>(elementWords); return true;
        case CollectionMethod::cListRemoveAt: emit<OpCode::cListRemoveAt>(elementWords); return true;
        case CollectionMethod::cListIndexOf:
            mCtx.mBackend.mKeyOpsTable.addType(listType->mElement);
            emit<OpCode::cListIndexOf>(elementWords);
            return true;
        case CollectionMethod::cListContains:
            mCtx.mBackend.mKeyOpsTable.addType(listType->mElement);
            emit<OpCode::cListContains>(elementWords);
            return true;
        case CollectionMethod::cListRemove:
            mCtx.mBackend.mKeyOpsTable.addType(listType->mElement);
            emit<OpCode::cListRemove>(elementWords);
            return true;
        case CollectionMethod::cListRemoveAll:
            mCtx.mBackend.mKeyOpsTable.addType(listType->mElement);
            emit<OpCode::cListRemoveAll>(elementWords);
            return true;
        case CollectionMethod::cListClear: emit<OpCode::cListClear>(); return true;
        case CollectionMethod::cListReserve: emit<OpCode::cListReserve>(); return true;
        case CollectionMethod::cListResize: emit<OpCode::cListResize>(elementWords); return true;

        case CollectionMethod::cMapGetKeys: emit<OpCode::cMapGetKeys>(); return true;
        case CollectionMethod::cMapGetValues: emit<OpCode::cMapGetValues>(); return true;
        case CollectionMethod::cMapSize: emit<OpCode::cMapSize>(); return true;
        case CollectionMethod::cMapIsEmpty: emit<OpCode::cMapIsEmpty>(); return true;
        case CollectionMethod::cMapClear: emit<OpCode::cMapClear>(); return true;
        case CollectionMethod::cMapContainsKey:
            emit<OpCode::cMapContainsKey>(getMapAccessIndex(mapType, 0, 0));
            return true;
        case CollectionMethod::cMapTryGet:
            emit<OpCode::cMapTryGet>(getMapAccessIndex(mapType, 0, valueWords));
            return true;
        case CollectionMethod::cMapGet:
            emit<OpCode::cLoadMapValue>(getMapAccessIndex(mapType, 0, valueWords));
            return true;
        case CollectionMethod::cMapSet:
            emit<OpCode::cStoreMapValue>(getMapAccessIndex(mapType, 0, valueWords));
            return true;
        case CollectionMethod::cMapRemove: emit<OpCode::cMapRemove>(getMapAccessIndex(mapType, 0, 0)); return true;
        case CollectionMethod::cMapReserve: emit<OpCode::cMapReserve>(); return true;
        case CollectionMethod::cNone: break;
    }

    SIMLANG_BREAK("Invalid collection method during code generation.");
    return false;
}

bool CodeGenVisitor::emitMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess)
{
    auto* funcType = static_cast<FunctionType*>(memberAccess->mSymbol->mType);

    // Push the receiver onto the stack (struct ref or handle value).
    if (emitMethodReceiver(memberAccess) == false)
    {
        return false;
    }

    // Emit the args.
    if (emitCallArguments(node->mArgs, funcType) == false)
    {
        return false;
    }

    // If this is an interface, handle it differently.
    // Interface call info also needs the interface slot on top of the args/return word info.
    Type* receiverType = memberAccess->mReceiver->mResolvedType;
    if (receiverType->mKind == TypeKind::cInterface)
    {
        return emitInterfaceMethodDispatch(memberAccess, funcType);
    }

    // Otherwise, get the function (or method) index from the symbol and emit.
    auto index = static_cast<FunctionIdx>(memberAccess->mSymbol->mIndex);
    if (receiverType->mKind == TypeKind::cClass)
    {
        emit<OpCode::cCallMethod>(index);
    }
    else
    {
        emit<OpCode::cCall>(index);
    }

    return true;
}

bool CodeGenVisitor::emitFreeFunctionOrSyscallCall(FunctionCallNode* node)
{
    // Emit the args.
    auto* funcType = static_cast<FunctionType*>(node->mReceiver->mResolvedType);
    if (emitCallArguments(node->mArgs, funcType) == false)
    {
        return false;
    }

    // Get the index of the function or syscall from the symbol.
    // The symbol is either on an identifier or on a module access node.
    Symbol* symbol = nullptr;
    switch (node->mReceiver->mNodeType)
    {
        case NodeType::cIdentifier:
        {
            auto* id = static_cast<IdentifierNode*>(node->mReceiver);
            symbol = id->mSymbol;
            break;
        }
        case NodeType::cModuleAccess:
        {
            auto* moduleAccess = static_cast<ModuleAccessNode*>(node->mReceiver);
            symbol = moduleAccess->mSymbol;
            break;
        }
        default:
        {
            return false;
        }
    }

    // Function case.
    if (symbol->mSymbolType == SymbolType::cFunction)
    {
        FunctionIdx funIndex = static_cast<FunctionIdx>(symbol->mIndex);
        emit<OpCode::cCall>(funIndex);
        return true;
    }

    // Syscall case.
    if (symbol->mSymbolType != SymbolType::cSyscall)
    {
        return false;
    }

    SyscallIdx syscallIndex = static_cast<SyscallIdx>(symbol->mIndex);
    emit<OpCode::cSyscall>(syscallIndex);

    return true;
}

bool CodeGenVisitor::emitIndirectFunctionCall(FunctionCallNode* node)
{
    // Get the function and resolve the receiver.
    auto* funcType = static_cast<FunctionType*>(node->mReceiver->mResolvedType);

    // Resolve the receiver of the call (stuff before the '(').
    // This will push the callable data.
    if (visit(node->mReceiver) == false)
    {
        return false;
    }

    // Emit the args.
    if (emitCallArguments(node->mArgs, funcType) == false)
    {
        return false;
    }

    // We have the callable already on the stack and pushed the args on top.
    // For now, we give the call instruction information on the arg and return words.
    // The arg words are needed for the VM to know where to find the callable in the first place.
    // (Otherwise, we'd have to push the callable after the params, which we currently don't do.)
    // The return word count is needed in case we call a null pointer and need to keep the stack healthy.
    // Would it make sense to change this if we ever enforce null-safety through '?'?
    // (We currently also use this for stack analysis at compile time since we don't know the callable value then.)
    u64 argWords = 0;
    for (const FunctionParam& param : funcType->mParamTypes)
    {
        argWords += param.mIsInOut ? 1U : layout::getWordSizeForType(param.mType);
    }

    if (argWords > cMaxOpWordCount)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mSourceRange, argWords, "argument", cMaxOpWordCount);
        return false;
    }

    u32 returnWords = layout::getWordSizeForType(funcType->mReturnType);
    if (returnWords > cMaxReturnWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mSourceRange, returnWords, "return", cMaxReturnWords);
        return false;
    }

    emit<OpCode::cCallValue>(static_cast<OpWordCount>(argWords), static_cast<ReturnWordCount>(returnWords));

    return true;
}

} // namespace simlang
