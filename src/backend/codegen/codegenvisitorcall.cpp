#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "backend/backendstate.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/layout/layout.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

InterfaceCallIdx CodeGenVisitor::getInterfaceCallIndex(const SourceRange& range,
                                                       InterfaceMethodSlot slot,
                                                       OpWordCount argWords,
                                                       ReturnWordCount returnWords)
{
    // Build the call index based on the slot, arg words, and return words.
    // This can be reused by any interface call with the same params.
    for (usize i = 0; i < mCtx.mBackend.mInterfaceCallInfos.size(); ++i)
    {
        const InterfaceCallInfo& info = mCtx.mBackend.mInterfaceCallInfos[i];
        if (info.mSlot == slot && info.mArgWords == argWords && info.mReturnWords == returnWords)
        {
            return static_cast<InterfaceCallIdx>(i);
        }
    }

    usize rawIndex = mCtx.mBackend.mInterfaceCallInfos.size();
    if (rawIndex > cMaxValidInterfaceCallIdx)
    {
        mCtx.report<cTooManyInterfaceCalls>(range, rawIndex, cMaxValidInterfaceCallIdx);
        return cInvalidInterfaceCallIdx;
    }

    // New one, build the info.
    InterfaceCallInfo info;
    info.mSlot = slot;
    info.mArgWords = argWords;
    info.mReturnWords = returnWords;

    // Add it and return its index (previous size).
    mCtx.mBackend.mInterfaceCallInfos.push_back(info);

    return static_cast<InterfaceCallIdx>(rawIndex);
}

u32 CodeGenVisitor::getInterfaceTableIndex(AggregateType* aggregateType, InterfaceType* interfaceType)
{
    // We want to resolve the interface table index for the given interface type within the given aggregate type.
    // Go over all interfaces the aggregate is implementing.
    for (InterfaceImplementation& implementation : aggregateType->mInterfaces)
    {
        // If it's' not the interface we're looking for, bail.
        if (implementation.mInterface != interfaceType)
        {
            continue;
        }

        // If we already set/resolved the index for this interface, return it.
        if (implementation.mTableIndex != cInvalidInterfaceMethodTableIndex)
        {
            return implementation.mTableIndex;
        }

        std::vector<FunctionIdx> methods;
        methods.reserve(interfaceType->mSymbol->mMembers.size());

        // Otherwise, we have to resolve the index for this interface.
        // Go over all interface members (which currently
        for (Symbol* interfaceMember : interfaceType->mSymbol->mMembers)
        {
            Symbol* concreteMember = nullptr;
            for (Symbol* member : aggregateType->mSymbol->mMembers)
            {
                // Find the member function with the same identifier as the interface member.
                // We know it exists because sema checked that already.
                if (member->mSymbolType == SymbolType::cMemberFunction &&
                    member->mIdentifier == interfaceMember->mIdentifier)
                {
                    concreteMember = member;
                    break;
                }
            }

            // Add the aggregate method index to the methods for this interface implementation.
            methods.push_back(static_cast<FunctionIdx>(concreteMember->mIndex));
        }

        // The table index points to where our interface method implementations start.
        // That way, we can use the table index plus the interface method slot to lookup the function index.
        implementation.mTableIndex = static_cast<u32>(mCtx.mBackend.mInterfaceMethods.size());

        // Add all the indices to the interface method implementations.
        for (FunctionIdx method : methods)
        {
            mCtx.mBackend.mInterfaceMethods.push_back(method);
        }

        return implementation.mTableIndex;
    }

    return cInvalidInterfaceMethodTableIndex;
}

bool CodeGenVisitor::emitCallArguments(ArrayView<ExpressionNode*> args, const FunctionType* funcType)
{
    // Go over all the args.
    for (usize i = 0; i < args.size(); ++i)
    {
        const FunctionParam& param = funcType->mParamTypes[i];

        // If the param is an in-out param, emit the address of the arg instead of the value.
        if (param.mIsInOut)
        {
            if (emitAddress(args[i], AddressMode::cStorage) == false)
            {
                return false;
            }
        }
        else
        {
            if (visit(args[i]) == false)
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
        // Allow this for temporaries (for stuff like makeStruct().f()).
        return emitAddress(memberAccess->mReceiver, AddressMode::cReadOnly);
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

    // Also check the return words.
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
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cListPush>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "addList"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
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
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false || visit(node->mArgs[1]) == false)
        {
            return false;
        }

        emit<OpCode::cListInsert>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "removeAt"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cListRemoveAt>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "indexOf"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cListIndexOf>(static_cast<OpWordCount>(elementWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "contains"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
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
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cListReserve>();
        return true;
    }

    return false;
}

bool CodeGenVisitor::emitMapMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess)
{
    auto* mapType = static_cast<MapType*>(memberAccess->mReceiver->mResolvedType);
    u32 valueWords = layout::getWordSizeForType(mapType->mValue);

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
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cMapContainsKey>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "get"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cMapGet>(static_cast<OpWordCount>(valueWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "set"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false || visit(node->mArgs[1]) == false)
        {
            return false;
        }

        emit<OpCode::cMapSet>(static_cast<OpWordCount>(valueWords));
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "remove"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
        {
            return false;
        }

        emit<OpCode::cMapRemove>();
        return true;
    }

    if (identifierEquals(memberAccess->mMember, "reserve"))
    {
        if (visit(memberAccess->mReceiver) == false || visit(node->mArgs[0]) == false)
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

} // namespace simlang
