#include "backend/codegen/codegenvisitor.h"

#include <vector>

#include "ast/nodes/astnode.h"
#include "backend/backendstate.h"
#include "backend/layout/layout.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/op/opcode.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/types.h"
#include "util/scoping.h"

namespace simlang
{

CodeGenVisitor::CodeGenVisitor(CompilerContext& ctx)
    : mCtx(ctx)
    , mBytecodeBuilder(ctx.mBackend.mProgramBytecode)
{
    mControlStack.reserve(16);

    // Call main.
    emit<OpCode::cCall>(mCtx.mBackend.mMainIndex);
    emit<OpCode::cHalt>();
}

bool CodeGenVisitor::emitLambdaBodies()
{
    // Go through all the lambdas and emit their code.
    for (LambdaNode* lambda : mCtx.mBackend.mLambdas)
    {
        // A lambda gets its own function index (from the symbol).
        FunctionIdx functionIndex = static_cast<FunctionIdx>(lambda->mSymbol->mIndex);

        // Enter a new chunk to generate code for.
        auto bytecodeChunkScope = mBytecodeBuilder.enterFunction(functionIndex);

        // Also set the current function info and lambda.
        ScopedValueBinder functionInfoScope{mCurrentFunctionInfo, &mCtx.mBackend.mFunctionInfos[functionIndex]};
        ScopedValueBinder lambdaScope{mCurrentLambda, lambda};

        // Visit the body.
        // Note that lambdas are not processed recursively here, so an inner lambda is handled later in the loop.
        if (visit(lambda->mBody) == false)
        {
            return false;
        }

        // If we have void, emit the return manually.
        auto* functionType = static_cast<FunctionType*>(lambda->mSymbol->mType);
        if (functionType->mReturnType->mKind == TypeKind::cVoid)
        {
            emit<OpCode::cReturn>();
        }
    }

    return true;
}

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

MapAccessIdx CodeGenVisitor::getMapAccessIndex(MapType* mapType, FieldOffset valueFieldOffset, OpWordCount valueWords)
{
    // Here, we compute the index to look up map access info based on map type, value field offset, and value word
    // count. This covers both key/value information (although the offset and value words are only used for the value).
    // Get the size of the key.
    auto keyWords = static_cast<OpWordCount>(layout::getWordSizeForType(mapType->mKey));
    // Go through all the map access infos to see if we already have an entry (and thus index) for this type of access.
    auto& mais = mCtx.mBackend.mMapAccessInfos;
    for (usize i = 0; i < mais.size(); ++i)
    {
        const MapAccessInfo& mai = mais[i];
        if (mai.mKeyWords == keyWords && mai.mValueFieldOffset == valueFieldOffset && mai.mValueWords == valueWords)
        {
            return static_cast<MapAccessIdx>(i);
        }
    }

    // If we're here, we didn't find anything, so we need to create a new one.
    auto index = static_cast<MapAccessIdx>(mais.size());
    mais.push_back(MapAccessInfo{keyWords, valueFieldOffset, valueWords});
    return index;
}

void CodeGenVisitor::enterNode(ASTNode* node)
{
    // If we're entering a new node, back up the current source range.
    mSourceRangeStack.push_back(mBytecodeBuilder.getSourceRange());
    // Tell the builder what source range we're currently processing.
    mBytecodeBuilder.setSourceRange(node->mSourceRange);
}

void CodeGenVisitor::leaveNode(ASTNode*)
{
    // If we're leaving a node, restore the previous source range.
    mBytecodeBuilder.setSourceRange(mSourceRangeStack.back());
    mSourceRangeStack.pop_back();
}

BytecodeLabel CodeGenVisitor::makeLabel()
{
    return mBytecodeBuilder.makeLabel();
}

} // namespace simlang
