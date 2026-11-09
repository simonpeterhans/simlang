#include "backend/layout/constdatalayoutvisitor.h"

#include <utility>
#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
#include "backend/layout/layout.h"
#include "backend/stringdata.h"
#include "backend/typeidutils.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/stringdata.h"
#include "runtime/vmdefines.h"
#include "sema/constructoranalysis.h"
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

void ConstDataLayoutVisitor::registerConstValueStrings(const ConstValue& value)
{
    if (value.mKind == ConstValueKind::cStruct)
    {
        // If we have a struct, recurse.
        for (const ConstFieldValue& field : value.as.mStruct->mFields)
        {
            registerConstValueStrings(*field.mValue);
        }
    }
    else if (value.mKind == ConstValueKind::cPrimitive && value.mPrimitiveKind == PrimitiveTypeKind::cString)
    {
        // If we have a literal, add that.
        addStringLiteral(value.as.mString);
    }
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
                    word = makeStaticStringHandle(stringIndex);
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
        case TypeKind::cFunction:
        {
            if (value.mKind != ConstValueKind::cFunction || value.as.mFunction == nullptr)
            {
                SIMLANG_BREAK("Invalid function const global value.");
                return false;
            }

            // Constant callable values have no capture environment.
            Symbol* callable = value.as.mFunction;
            VMWord entryToken = cInvalidFunctionEntryToken;
            if (callable->mSymbolType == SymbolType::cFunction || callable->mSymbolType == SymbolType::cLambda)
            {
                entryToken = makeFunctionEntryToken(static_cast<FunctionIdx>(callable->mIndex));
            }
            else if (callable->mSymbolType == SymbolType::cSyscall)
            {
                entryToken = makeSyscallEntryToken(mCtx.mBackend.mFunctionInfos.size(),
                                                   static_cast<SyscallIdx>(callable->mIndex));
            }
            else
            {
                SIMLANG_BREAK("Invalid function const global symbol.");
                return false;
            }

            // We cannot have a context for global stuff, so we just write the entry.
            mCtx.mBackend.mInitialGlobals[globalIdx + cFunctionValueContextWord] = cNullRef;
            mCtx.mBackend.mInitialGlobals[globalIdx + cFunctionValueEntryWord] = entryToken;

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

            // Write the whole null value, including the dispatch word for interfaces.
            for (u32 i = 0; i < layout::getWordSizeForType(type); ++i)
            {
                mCtx.mBackend.mInitialGlobals[globalIdx + i] = cNullRef;
            }
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

    // Constant analysis has already validated this initializer.
    ConstValue value;
    ConstEvalVisitor evaluator{mCtx.mAllocator};
    evaluator.evaluate(node->mInit, value);

    Symbol* symbol = node->mSymbol;
    return writeConstGlobalValue(symbol->mType, static_cast<u32>(symbol->mIndex), value);
}

bool ConstDataLayoutVisitor::visitIdentifier(IdentifierNode* node)
{
    // If this is not constexpr, we can't do anything with it.
    Symbol* symbol = node->mSymbol;
    if (symbol->mConstEvalState != ConstEvalState::cReady)
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

bool ConstDataLayoutVisitor::visitNewObject(NewObjectNode* node)
{
    // If we have a new expression that is a const expr, evaluate it and emit the strings.
    // (They are the only thing we need to lay out here.)
    if (node->mFlags.test(cExprIsConstExpr))
    {
        ConstValue value;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        evaluator.evaluate(node, value);
        registerConstValueStrings(value);
    }

    return ASTWalker::visitNewObject(node);
}

bool ConstDataLayoutVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If this type is a template declaration, ignore it -- we only process instantiated templates.
    if (node->isTemplate())
    {
        return true;
    }

    if (node->isInterface() == false)
    {
        auto* type = static_cast<AggregateType*>(node->mSymbol->mType);
        auto* constructor = static_cast<FunctionDeclarationStatementNode*>(type->mConstructorSymbol->mDeclNode);
        if (constructor->mIsTrivialConstructor)
        {
            // Register strings retained by the initialization plan, independently of folding its source expressions.
            for (const FieldInitialization& field : constructor->mConstructorAnalysis->mFields)
            {
                if (field.mConstantValue != nullptr)
                {
                    registerConstValueStrings(*field.mConstantValue);
                }
            }
        }
    }

    return ASTWalker::visitTypeDeclarationStatement(node);
}

bool ConstDataLayoutVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    // Special case to emit globals.
    if (node->mSymbol->mSymbolType == SymbolType::cGlobalVariable)
    {
        if (writeConstGlobalInitializer(node) == false)
        {
            return false;
        }
    }

    return ASTWalker::visitVariableDeclarationStatement(node);
}

} // namespace simlang
