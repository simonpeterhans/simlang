#include "sema/passes/captureanalysisvisitor.h"

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "diag/diagnosticmanager.h"
#include "driver/compilercontext.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "util/arenautils.h"

namespace simlang
{

CaptureAnalysisVisitor::CaptureAnalysisVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool CaptureAnalysisVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

usize CaptureAnalysisVisitor::findCallable(const Symbol* symbol) const
{
    // Go through the stack to find the symbol of the callable we're looking for.
    for (usize i = mCallableStack.size(); i > 0; --i)
    {
        if (mCallableStack[i - 1].mSymbol == symbol)
        {
            return i - 1;
        }
    }

    return mCallableStack.size();
}

void CaptureAnalysisVisitor::addCapture(CallableContext& context,
                                        LambdaCaptureKind kind,
                                        Symbol* symbol,
                                        SourceRange useRange)
{
    // Don't capture twice.
    for (const LambdaCapture& capture : context.mCaptures)
    {
        if (capture.mKind == kind && capture.mSymbol == symbol)
        {
            return;
        }
    }

    // 0 is a placeholder since we don't know the offset yet at this stage (symbol layout needs to handle that).
    context.mCaptures.push_back(LambdaCapture{symbol, useRange, 0, kind});
}

void CaptureAnalysisVisitor::captureFrom(usize ownerIndex, LambdaCaptureKind kind, Symbol* symbol, SourceRange useRange)
{
    // Here, we capture the variable "upwards" from the owner index.
    // Every context inside (i.e., on top) of the owner needs it captured if it's a lambda.
    // The reason for that is that they all need their own "version" of the variable even if they don't use it.
    // (Since it's needed to construct a nested instance, they have to pass it on!)
    for (usize i = ownerIndex + 1; i < mCallableStack.size(); ++i)
    {
        CallableContext& context = mCallableStack[i];
        if (context.mLambda != nullptr)
        {
            addCapture(context, kind, symbol, useRange);
        }
    }
}

bool CaptureAnalysisVisitor::visitIdentifier(IdentifierNode* node)
{
    // If we're visiting an identifier, check if we have to capture it.
    Symbol* symbol = node->mSymbol;
    // This is only necessary for stack vars & params.
    // Globals are accessed directly, and members are accessed through "this".
    if (symbol->mSymbolType != SymbolType::cStackVariable && symbol->mSymbolType != SymbolType::cParameter)
    {
        return true;
    }

    // Get the symbol of the owner of the identifier's symbol.
    Symbol* owner = symbol->mOwningCallable;

    // Find the owner context in the stack based on the symbol.
    usize callableIdx = findCallable(owner);
    captureFrom(callableIdx, LambdaCaptureKind::cSymbol, symbol, node->mSourceRange);

    return true;
}

bool CaptureAnalysisVisitor::visitThis(ThisNode* node)
{
    // If we're visiting "this", check if we have to capture it.
    for (usize i = mCallableStack.size(); i > 0; --i)
    {
        // Go down the stack until we have the member function this belongs to.
        // Currently, we could also simply check the bottom frame (I think?).
        Symbol* symbol = mCallableStack[i - 1].mSymbol;
        if (symbol->mSymbolType == SymbolType::cMemberFunction)
        {
            captureFrom(i - 1, LambdaCaptureKind::cThis, nullptr, node->mSourceRange);
            break;
        }
    }

    return true;
}

bool CaptureAnalysisVisitor::visitLambda(LambdaNode* node)
{
    // Push a new callable onto the stack for the node, starting with an empty capture.
    mCallableStack.push_back(CallableContext{node->mSymbol, node, {}});

    // Figure out the captures and register them in the node.
    bool success = visit(node->mBody);
    node->mCaptures = makeArrayView(mCtx.mAllocator, mCallableStack.back().mCaptures);

    mCallableStack.pop_back();

    return success;
}

bool CaptureAnalysisVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    // If we're inside a (top-level) function declaration, push a new (empty) callable.
    mCallableStack.push_back(CallableContext{node->mSymbol, nullptr, {}});

    bool success = visit(node->mBody);

    mCallableStack.pop_back();

    return success;
}

bool CaptureAnalysisVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If this is a template, we can bail since we only need to handle instantiations.
    if (node->isTemplate())
    {
        return true;
    }

    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    return true;
}

} // namespace simlang
