#pragma once

#include <vector>

#include "ast/astwalker.h"

namespace simlang
{

struct CompilerContext;
struct Symbol;

class CaptureAnalysisVisitor : public ASTWalker<CaptureAnalysisVisitor>
{
public:
    explicit CaptureAnalysisVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);

    bool visitIdentifier(IdentifierNode* node);
    bool visitThis(ThisNode* node);
    bool visitLambda(LambdaNode* node);

    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);

private:
    struct CallableContext
    {
        // Symbol for the callable (function, method, or synthesized lambda symbol).
        Symbol* mSymbol = nullptr;
        // Lambda node, if any (null for function/method).
        LambdaNode* mLambda = nullptr;
        // The captures of this context.
        std::vector<LambdaCapture> mCaptures;
    };

    void addCapture(CallableContext& context, LambdaCaptureKind kind, Symbol* symbol, SourceRange useRange);
    void captureFrom(usize ownerIndex, LambdaCaptureKind kind, Symbol* symbol, SourceRange useRange);
    usize findCallable(const Symbol* symbol) const;

    CompilerContext& mCtx;
    std::vector<CallableContext> mCallableStack;
};

} // namespace simlang
