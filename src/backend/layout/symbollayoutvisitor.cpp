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

using FunctionScope = ScopedValueBinder<FunctionIdx>;

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

FunctionInfo& SymbolLayoutVisitor::getCurrentFunction()
{
    return mCtx.mBackend.mFunctionInfos[mCurrentFunction];
}

bool SymbolLayoutVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    if (node->mSymbol->mSymbolType == SymbolType::cMemberVariable)
    {
        // For member variables we simply register the current member index.
        // The type layout already accounts for alignment.
        node->mSymbol->mIndex = static_cast<i32>(mNextMemberFieldIndex++);
        return visit(node->mInit);
    }

    if (node->mSymbol->mSymbolType == SymbolType::cGlobalVariable)
    {
        // Get the size.
        u32 globalWords = layout::getStorageWordSizeForType(node->mSymbol->mType);

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

        return visit(node->mInit);
    }

    // Get the word size for the local.
    u32 localWords = layout::getStorageWordSizeForType(node->mSymbol->mType);

    // The opcodes treat args and locals as one consecutive index.
    // Thus, we need to offset by mArgWords (and account for that in the limit).
    FunctionInfo& function = getCurrentFunction();
    u64 maxLocalWords = std::min(cMaxFrameWords, (cMaxLocalIdx + 1) - function.mArgWords);
    u64 nextLocalWords = function.mLocalWords + static_cast<u64>(localWords);
    if (nextLocalWords > maxLocalWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mIdentifierRange, nextLocalWords, "local", maxLocalWords);
        return false;
    }

    // The local starts at the current offset of arg and local words.
    u32 localIndex = function.mArgWords + function.mLocalWords;
    node->mSymbol->mIndex = static_cast<i32>(localIndex);
    // We have more local words.
    function.mLocalWords = static_cast<FrameWordCount>(nextLocalWords);

    return visit(node->mInit);
}

bool SymbolLayoutVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // Interface declarations cannot have a body (at the moment).
    if (node->mBody == nullptr)
    {
        return true;
    }

    // Set the symbol's index.
    FunctionIdx functionIndex = static_cast<FunctionIdx>(mCtx.mBackend.mFunctionInfos.size());

    node->mSymbol->mIndex = static_cast<i32>(functionIndex);

    // Create a new function info.
    mCtx.mBackend.mFunctionInfos.emplace_back();

    // Bind the current function.
    FunctionScope functionScope{mCurrentFunction, functionIndex};

    // If this is a member function, we need to account for "this" as an arg.
    if (node->mSymbol->mSymbolType == SymbolType::cMemberFunction)
    {
        getCurrentFunction().mArgWords = 1;
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

    auto* ft = static_cast<FunctionType*>(node->mSymbol->mType);
    u32 retWords = layout::getWordSizeForType(ft->mReturnType);
    getCurrentFunction().mReturnWords = static_cast<ReturnWordCount>(retWords);

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

            mCtx.mBackend.mMainIndex = functionIndex;
        }
    }

    return true;
}

bool SymbolLayoutVisitor::visitLambda(LambdaNode* node)
{
    // Lay out the lambda stuff.
    // Make a new function info for the lambda.
    FunctionIdx functionIndex = static_cast<FunctionIdx>(mCtx.mBackend.mFunctionInfos.size());

    node->mSymbol->mIndex = static_cast<i32>(functionIndex);
    mCtx.mBackend.mFunctionInfos.emplace_back();
    mCtx.mBackend.mLambdas.push_back(node);

    if (node->mCaptures.empty() == false)
    {
        // If we have captures, we need to define an environment.
        // The type ID may have to go from 16 to 32 bits in the future.
        if (mCtx.mBackend.mNextTypeID > cMaxTypeID)
        {
            mCtx.report<cTooManyLambdaEnvironments>(node->mSourceRange, mCtx.mBackend.mNextTypeID, cMaxTypeID);
            return false;
        }

        // Register as a new type.
        node->mEnvironmentTypeID = mCtx.mBackend.mNextTypeID++;

        // Count the words that the environment requires on the heap.
        u64 environmentWords = 0;
        for (LambdaCapture& capture : node->mCaptures)
        {
            // Update the offset while we're at it.
            capture.mEnvironmentOffset = static_cast<u32>(environmentWords);
            // If we have a symbol use its type, if we have "this" use the instance type that we stored in the node.
            Type* captureType =
                (capture.mKind == LambdaCaptureKind::cSymbol) ? capture.mSymbol->mType : node->mLexicalThisType;

            // Don't grow too big.
            u64 nextEnvironmentWords = environmentWords + layout::getStorageWordSizeForType(captureType);
            if (nextEnvironmentWords > cMaxTypeLayoutWordCount)
            {
                mCtx.report<cLambdaEnvironmentTooLarge>(node->mSourceRange,
                                                        nextEnvironmentWords,
                                                        cMaxTypeLayoutWordCount);
                return false;
            }

            environmentWords = nextEnvironmentWords;
        }
    }

    // We handled all the capture offsets.
    // Now make a scope and lay out the params and body.
    FunctionScope functionScope{mCurrentFunction, functionIndex};

    for (ParamNode* param : node->mParams)
    {
        if (visit(param) == false)
        {
            return false;
        }
    }

    if (visit(node->mBody) == false)
    {
        return false;
    }

    auto* functionType = static_cast<FunctionType*>(node->mSymbol->mType);
    u32 returnWords = layout::getWordSizeForType(functionType->mReturnType);
    // Set the return words in the current function.
    getCurrentFunction().mReturnWords = static_cast<ReturnWordCount>(returnWords);

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
    FunctionInfo& function = getCurrentFunction();
    u64 nextArgWords = static_cast<u64>(function.mArgWords) + words;
    if (nextArgWords > cMaxFrameWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(node->mIdentifierRange, nextArgWords, "argument", cMaxFrameWords);
        return false;
    }

    // The symbol index here is the param's offset on the stack.
    node->mSymbol->mIndex = static_cast<i32>(function.mArgWords);
    function.mArgWords = static_cast<FrameWordCount>(nextArgWords);

    return true;
}

} // namespace simlang
