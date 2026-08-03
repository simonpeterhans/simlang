#include "sema/passes/typecheckvisitor.h"

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "module/moduleentry.h"
#include "source/sourcerange.h"
#include "type/typeformat.h"
#include "type/typekind.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arrayview.h"

namespace simlang
{

using ModuleScope = ScopedValueBinder<ModuleEntry*>;

TypeCheckVisitor::TypeCheckVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool TypeCheckVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool TypeCheckVisitor::visitTranslationUnit(TranslationUnitNode* node)
{
    ModuleScope ms{mCurrentModule, node->mModuleEntry};

    for (ASTNode* stmt : node->mNodes)
    {
        if (visit(stmt) == false)
        {
            return false;
        }
    }

    return true;
}

Type* TypeCheckVisitor::getErrorType() const
{
    return mCtx.mTypes.getErrorType();
}

bool TypeCheckVisitor::isErrorType(Type* type) const
{
    return (type == nullptr || type->mKind == TypeKind::cError);
}

void TypeCheckVisitor::markError(ExpressionNode* expr) const
{
    if (expr != nullptr)
    {
        expr->mResolvedType = getErrorType();
    }
}

Type* TypeCheckVisitor::requireNonNullType(Type* type, SourceRange range, const char* context) const
{
    // If this is an error type, return that, assuming that the error has already been reported.
    if (isErrorType(type))
    {
        return getErrorType();
    }

    // If this is a null type, we report an error here.
    if (type->mKind == TypeKind::cNull)
    {
        mCtx.report<cInvalidTypeInContext>(range, typeToString(type), context);
        return getErrorType();
    }

    return type;
}

Type* TypeCheckVisitor::requireValueType(Type* type, SourceRange range, const char* context) const
{
    // If this is an error or a null type, return the error type.
    type = requireNonNullType(type, range, context);
    if (isErrorType(type))
    {
        return getErrorType();
    }

    // If this is a void type, we report an error here.
    if (getPrimitiveKind(type) == PrimitiveTypeKind::cVoid)
    {
        mCtx.report<cInvalidTypeInContext>(range, typeToString(type), context);
        return getErrorType();
    }

    return type;
}

ScopedValueBinder<i32> TypeCheckVisitor::enterBreakContext()
{
    // SVB is move-only, so we can just rely on copy elision here.
    return ScopedValueBinder{mBreakContextDepth, mBreakContextDepth + 1};
}

ScopedValueBinder<i32> TypeCheckVisitor::enterContinueContext()
{
    return ScopedValueBinder{mContinueContextDepth, mContinueContextDepth + 1};
}

} // namespace simlang
