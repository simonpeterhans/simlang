#include "backend/codegen/codegenvisitor.h"

#include "ast/nodes/astnode.h"
#include "backend/backendstate.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"

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
