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
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
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

bool CodeGenVisitor::emitListMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess)
{
    auto* listType = static_cast<ListType*>(memberAccess->mReceiver->mResolvedType);
    u32 elementWords = layout::getWordSizeForType(listType->mElement);

    if (identifierEquals(memberAccess->mMember, "size"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cListSize>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "isEmpty"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cListIsEmpty>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "add") || identifierEquals(memberAccess->mMember, "push"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListPush>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "addList"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListAddList>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "pop"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cListPop>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "back"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cListBack>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "insertAt"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false ||
            visit(node->mArgs[1].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListInsert>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "removeAt"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListRemoveAt>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "indexOf"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListIndexOf>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "contains"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListContains>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "clear"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cListClear>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "reserve"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListReserve>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "resize"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false ||
            visit(node->mArgs[1].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cListResize>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    return false;
}

bool CodeGenVisitor::emitMapMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess)
{
    auto* mapType = static_cast<MapType*>(memberAccess->mReceiver->mResolvedType);
    u32 valueWords = layout::getWordSizeForType(mapType->mValue);

    if (identifierEquals(memberAccess->mMember, "getKeys"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cMapGetKeys>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "getValues"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cMapGetValues>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "size"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cMapSize>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "isEmpty"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cMapIsEmpty>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "clear"))
    {
        if (visit(memberAccess->mReceiver) == false)
        {
            return false;
        }

        emit<OpCode::cMapClear>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "containsKey"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        // Here, we are only interested in the key and so we don't have a value field offset or value words.
        emit<OpCode::cMapContainsKey>(getMapAccessIndex(mapType, 0, 0));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "tryGet"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false ||
            emitAddress(node->mArgs[1].mValue, AddressMode::cStorage) == false)
        {
            return false;
        }

        emit<OpCode::cMapTryGet>(getMapAccessIndex(mapType, 0, static_cast<OpWordCount>(valueWords)));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "get"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cLoadMapValue>(getMapAccessIndex(mapType, 0, static_cast<OpWordCount>(valueWords)));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "set"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false ||
            visit(node->mArgs[1].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cStoreMapValue>(getMapAccessIndex(mapType, 0, static_cast<OpWordCount>(valueWords)));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "remove"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        // Here, we are only interested in the key and so we don't have a value field offset or value words.
        emit<OpCode::cMapRemove>(getMapAccessIndex(mapType, 0, 0));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "reserve"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0].mValue) == false)
        {
            return false;
        }

        emit<OpCode::cMapReserve>();
        return true;
    }

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
