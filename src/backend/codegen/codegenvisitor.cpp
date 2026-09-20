#include "backend/codegen/codegenvisitor.h"

#include "ast/nodes/astnode.h"
#include "backend/backendstate.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"
#include "symbol/symbol.h"
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
        if (getPrimitiveKind(functionType->mReturnType) == PrimitiveTypeKind::cVoid)
        {
            emit<OpCode::cReturn>();
        }
    }

    return true;
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
