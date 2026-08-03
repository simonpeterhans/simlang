#include "backend/layout/constdatalayoutvisitor.h"

#include <utility>
#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
#include "backend/stringdata.h"
#include "backend/typeidutils.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/stringdata.h"
#include "runtime/vmdefines.h"
#include "sema/passes/constevalvisitor.h"
#include "source/sourcerange.h"
#include "symbol/constvalue.h"
#include "symbol/internedstring.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/asserts.h"
#include "util/bitutils.h"
#include "util/flags.h"

namespace simlang
{

ConstDataLayoutVisitor::ConstDataLayoutVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool ConstDataLayoutVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

StringLiteralIdx ConstDataLayoutVisitor::addStringLiteral(const InternedString* str)
{
    return mCtx.mBackend.mStrings.getOrAddLiteral(str);
}

bool ConstDataLayoutVisitor::writeConstGlobalValue(Type* type, u32 globalIdx, const ConstValue& value)
{
    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        {
            // Primitives are one word, so write that.
            VMWord word = 0;
            switch (value.mPrimitiveKind)
            {
                case PrimitiveTypeKind::cInt:
                {
                    word = static_cast<VMWord>(value.as.mInteger);
                    break;
                }
                case PrimitiveTypeKind::cFloat:
                {
                    word = bits::bitCast<VMWord>(value.as.mFloat);
                    break;
                }
                case PrimitiveTypeKind::cBool:
                {
                    word = value.as.mBool ? 1U : 0U;
                    break;
                }
                case PrimitiveTypeKind::cString:
                {
                    StringLiteralIdx stringIndex = addStringLiteral(value.as.mString);

                    // The string index is combined with the tag to make the address.
                    word = cStaticStringTag | stringIndex;
                    break;
                }
                default:
                {
                    SIMLANG_BREAK("Invalid primitive const global value.");
                    return false;
                }
            }

            mCtx.mBackend.mInitialGlobals[globalIdx] = word;

            return true;
        }
        case TypeKind::cStruct:
        {
            auto* structType = static_cast<AggregateType*>(type);

            const ConstStructValue* structValue = value.as.mStruct;
            if (structValue == nullptr)
            {
                SIMLANG_BREAK("Struct const value missing during const data layout.");
                return false;
            }

            // For structs, write all of their fields (recursively).
            for (usize i = 0; i < structType->mLayout->mFields.size(); ++i)
            {
                const FieldLayout& field = structType->mLayout->mFields[i];
                const ConstValue& fieldValue = *structValue->mFields[i].mValue;

                // Simply add the offset to the global index.
                if (writeConstGlobalValue(field.mSymbol->mType, globalIdx + field.mOffset, fieldValue) == false)
                {
                    return false;
                }
            }

            return true;
        }
        case TypeKind::cClass:
        case TypeKind::cInterface:
        case TypeKind::cList:
        case TypeKind::cMap:
        {
            // For these, we only allow null as const global values.
            if (value.mKind != ConstValueKind::cNull)
            {
                SIMLANG_BREAK("Reference const global value must be null.");
                return false;
            }

            mCtx.mBackend.mInitialGlobals[globalIdx] = cNullRef;
            return true;
        }
        default:
        {
            SIMLANG_BREAK("Invalid const global type.");
            return false;
        }
    }
}

bool ConstDataLayoutVisitor::writeConstGlobalInitializer(VariableDeclarationStatementNode* node)
{
    SIMLANG_ASSERTM(node->mInit != nullptr, "Global initializer missing after semantic analysis.");
    SIMLANG_ASSERTM(node->mInit->mFlags.test(cExprIsConstExpr),
                    "Global initializer is not constexpr after semantic analysis.");

    // We have to evaluate again here for structs.
    // This is less efficient than what it could be, but that's okay for now.
    ConstValue value;
    ConstEvalVisitor evaluator{mCtx.mAllocator};
    if (evaluator.evaluate(node->mInit, value) == false)
    {
        mCtx.report<cGlobalInitializerNotConstExpr>(node->mInit->mSourceRange, node->mIdentifier);
        return false;
    }

    Symbol* symbol = node->mSymbol;
    return writeConstGlobalValue(symbol->mType, static_cast<u32>(symbol->mIndex), value);
}

bool ConstDataLayoutVisitor::visitIdentifier(IdentifierNode* node)
{
    // If this is not constexpr, we can't do anything with it.
    Symbol* symbol = node->mSymbol;
    if (symbol->mFlags.test(SymbolFlags::cConstExpr) == false)
    {
        return true;
    }

    // If it's not a string, bail as well.
    Type* type = node->mResolvedType;
    if (type->mKind != TypeKind::cPrimitive || symbol->mConstValue.mPrimitiveKind != PrimitiveTypeKind::cString)
    {
        return true;
    }

    // Otherwise, add it.
    addStringLiteral(symbol->mConstValue.as.mString);

    return true;
}

bool ConstDataLayoutVisitor::visitStringLiteral(StringLiteralNode* node)
{
    // Register any string literals.
    addStringLiteral(node->mString);
    return true;
}

bool ConstDataLayoutVisitor::visitFormatString(FormatStringNode* node)
{
    // Register any string formats.
    StringFormatTemplateBuilder builder;

    // Add all the literals to the template.
    for (const InternedString* str : node->mLiterals)
    {
        StringLiteralIdx literalIndex = addStringLiteral(str);

        builder.appendLiteral(literalIndex, str->mLength);
    }

    // Add all the arguments to the template (they have to be constexpr).
    for (ExpressionNode* arg : node->mArgs)
    {
        if (visit(arg) == false)
        {
            return false;
        }

        builder.appendArgKind(getStringFormatArgKind(getPrimitiveKind(arg->mResolvedType)));
    }

    // Build and add the template.
    StringFormatIdx formatIndex;
    StringFormatTemplate tmpl = std::move(builder).build();
    mCtx.mBackend.mStringFormats.getOrAddTemplate(std::move(tmpl), formatIndex);

    return true;
}

bool ConstDataLayoutVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If this type is a template declaration, ignore it -- we only process instantiated templates.
    if (node->isTemplate())
    {
        return true;
    }

    return ASTWalker::visitTypeDeclarationStatement(node);
}

bool ConstDataLayoutVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    // Special case to emit globals.
    if (node->mSymbol->mSymbolType == SymbolType::cGlobalVariable)
    {
        return writeConstGlobalInitializer(node);
    }

    return ASTWalker::visitVariableDeclarationStatement(node);
}

} // namespace simlang
