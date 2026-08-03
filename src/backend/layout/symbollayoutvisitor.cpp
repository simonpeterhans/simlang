#include "backend/layout/symbollayoutvisitor.h"

#include <algorithm>
#include <vector>

#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
#include "backend/layout/layout.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "symbol/identifiertable.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/scoping.h"

namespace simlang
{

using FunctionScope = ScopedValueBinder<FunctionInfo*>;

SymbolLayoutVisitor::SymbolLayoutVisitor(CompilerContext& context)
    : mCtx(context)
{
}

bool SymbolLayoutVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool SymbolLayoutVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    if (node->mSymbol->mSymbolType == SymbolType::cMemberVariable)
    {
        // For member variables we simply register the current member index.
        // The type layout already accounts for alignment.
        node->mSymbol->mIndex = static_cast<i32>(mNextMemberFieldIndex++);
        return true;
    }

    if (node->mSymbol->mSymbolType == SymbolType::cGlobalVariable)
    {
        // Get the size and make sure it fits.
        u32 globalWords = layout::getStorageWordSizeForType(node->mSymbol->mType);
        if (globalWords > cMaxOpWordCount)
        {
            mCtx.report<cGlobalValueTooLarge>(node->mIdentifierRange, node->mIdentifier, globalWords, cMaxOpWordCount);
            return false;
        }

        u64 nextGlobalIdx = static_cast<u64>(mNextGlobalWordIndex) + globalWords;
        // nextIndex - 1 > cMaxGlobalIndex since nextIndex is exclusive.
        if (nextGlobalIdx > cMaxGlobalIdx + 1)
        {
            mCtx.report<cTooManyGlobals>(node->mIdentifierRange, node->mIdentifier, nextGlobalIdx - 1, cMaxGlobalIdx);
            return false;
        }

        // Base index for this global.
        node->mSymbol->mIndex = static_cast<i32>(mNextGlobalWordIndex);
        // We have more global words.
        mNextGlobalWordIndex += globalWords;
        // Push default values (0) for each word.
        mCtx.mBackend.mInitialGlobals.resize(mNextGlobalWordIndex, 0U);

        return true;
    }

    // Get the word size for the local.
    u32 localWords = layout::getStorageWordSizeForType(node->mSymbol->mType);

    // The opcodes treat args and locals as one consecutive index.
    // Thus, we need to offset by mArgWords (and account for that in the limit).
    u64 maxLocalWords = std::min(cMaxFrameWords, (cMaxLocalIdx + 1) - mCurrentFunction->mArgWords);
    u64 nextLocalWords = mCurrentFunction->mLocalWords + static_cast<u64>(localWords);
    if (nextLocalWords > maxLocalWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mIdentifierRange, nextLocalWords, "local", maxLocalWords);
        return false;
    }

    // The local starts at the current offset of arg and local words.
    u32 localIndex = mCurrentFunction->mArgWords + mCurrentFunction->mLocalWords;
    node->mSymbol->mIndex = static_cast<i32>(localIndex);
    // We have more local words.
    mCurrentFunction->mLocalWords = static_cast<FrameWordCount>(nextLocalWords);

    return true;
}

bool SymbolLayoutVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // Interface declarations cannot have a body (at the moment).
    if (node->mBody == nullptr)
    {
        return true;
    }

    // Set the symbol's index.
    usize functionIndex = mCtx.mBackend.mFunctionInfos.size();

    node->mSymbol->mIndex = static_cast<i32>(functionIndex);

    // Create a new function info.
    mCtx.mBackend.mFunctionInfos.emplace_back();

    // Bind the current function.
    FunctionScope functionScope{mCurrentFunction, &mCtx.mBackend.mFunctionInfos.back()};

    // If this is a member function, we need to account for "this" as an arg.
    if (node->mSymbol->mSymbolType == SymbolType::cMemberFunction)
    {
        mCurrentFunction->mArgWords = 1;
    }

    // Visit the params.
    for (ParamNode* param : node->mParams)
    {
        if (visit(param) == false)
        {
            return false;
        }
    }

    // Visit the body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    // Make sure we fit into the return cap.
    auto* ft = static_cast<FunctionType*>(node->mSymbol->mType);
    u32 retWords = layout::getWordSizeForType(ft->mReturnType);
    if (retWords > cMaxReturnWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mIdentifierRange, retWords, "return", cMaxReturnWords);
        return false;
    }

    mCurrentFunction->mReturnWords = static_cast<ReturnWordCount>(retWords);

    // If this is main, validate that and track the index.
    if (node->mIdentifier == mCtx.mIdentifiers.getMainIdentifier())
    {
        if (node->mSymbol->mSymbolType == SymbolType::cFunction)
        {
            if (mCtx.mBackend.hasValidMain())
            {
                mCtx.report<cDuplicateMainFunction>(node->mIdentifierRange);
                return false;
            }

            if (ft->mReturnType != mCtx.mTypes.getPrimitiveType(PrimitiveTypeKind::cVoid) ||
                ft->mParamTypes.empty() == false)
            {
                mCtx.report<cInvalidMainSignature>(node->mIdentifierRange);
                return false;
            }

            mCtx.mBackend.mMainIndex = static_cast<FunctionIdx>(mCtx.mBackend.mFunctionInfos.size() - 1);
        }
    }

    return true;
}

bool SymbolLayoutVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // Only process instantiated templates, which are not template types.
    if (node->isTemplate())
    {
        return true;
    }

    if (mCtx.mBackend.mNextTypeID > cMaxTypeID)
    {
        mCtx.report<cTooManyTypes>(node->mIdentifierRange, node->mIdentifier, mCtx.mBackend.mNextTypeID, cMaxTypeID);
        return false;
    }

    // Set the symbol's index, which represents the type ID (and increment it).
    node->mSymbol->mIndex = static_cast<i32>(mCtx.mBackend.mNextTypeID++);

    if (node->isInterface())
    {
        // Register the function (method) indices for interfaces.
        u32 methodSlot = 0;
        for (StatementNode* member : node->mMembers)
        {
            auto* funDecl = static_cast<FunctionDeclarationStatementNode*>(member);
            if (methodSlot > cMaxInterfaceMethodSlot)
            {
                mCtx.report<cTooManyInterfaceMethods>(funDecl->mIdentifierRange,
                                                      funDecl->mIdentifier,
                                                      methodSlot,
                                                      cMaxInterfaceMethodSlot);
                return false;
            }

            funDecl->mSymbol->mIndex = static_cast<i32>(methodSlot++);
        }

        return true;
    }

    ScopedValueBinder<u32> fieldCountScope{mNextMemberFieldIndex, 0};

    // Process the member declarations.
    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    return true;
}

bool SymbolLayoutVisitor::visitParamDeclaration(ParamDeclarationNode* node)
{
    // Params are already on the caller's stack, so this must match the value words pushed by the caller.
    u32 words = 1;
    if (node->mSymbol->mFlags.test(SymbolFlags::cInOut) == false)
    {
        words = layout::getWordSizeForType(node->mSymbol->mType);
    }

    // I really doubt this will ever hit, but it's here anyway.
    u64 nextArgWords = static_cast<u64>(mCurrentFunction->mArgWords) + words;
    if (nextArgWords > cMaxFrameWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mIdentifierRange, nextArgWords, "argument", cMaxFrameWords);
        return false;
    }

    // The symbol index here is the param's offset on the stack.
    node->mSymbol->mIndex = static_cast<i32>(mCurrentFunction->mArgWords);
    mCurrentFunction->mArgWords = static_cast<FrameWordCount>(nextArgWords);

    return true;
}

} // namespace simlang
