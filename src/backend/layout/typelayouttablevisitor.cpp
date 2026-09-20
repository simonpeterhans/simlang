#include "backend/layout/typelayouttablevisitor.h"

#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
#include "backend/layout/layout.h"
#include "backend/layout/typelayouttablebuilder.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/memory/typelayout.h"
#include "runtime/typeids.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/asserts.h"

namespace simlang
{

TypeLayoutTableVisitor::TypeLayoutTableVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
    TypeLayoutTableBuilder& typeLayouts = mCtx.mBackend.mTypeLayoutTable;

    // Add placeholder layouts.
    typeLayouts.resizeLayoutTable(mCtx.mBackend.mNextTypeID);

    // We represent strings as single-word handles, but they are a reference type.
    typeLayouts.setLayout(cStringTypeID, TypeLayout::pack(1, TypeLayout::Kind::cReference, 0, 0));

    // Lists are single-word heap handles when stored as values, including as elements of another list.
    typeLayouts.setLayout(cListTypeID, TypeLayout::pack(1, TypeLayout::Kind::cReference, 0, 0));

    // Maps follow the same handle representation as lists.
    typeLayouts.setLayout(cMapTypeID, TypeLayout::pack(1, TypeLayout::Kind::cReference, 0, 0));

    // Function values inline a context reference followed by an entry token.
    static constexpr u32 cFunctionContextOffset = cFunctionValueContextWord;
    TypeLayoutRefOffsetIndex functionRefOffset = 0;
    u64 requiredIndex = 0;
    typeLayouts.tryAppendRefOffsets(ArrayView{&cFunctionContextOffset, 1}, functionRefOffset, requiredIndex);

    typeLayouts.setLayout(cFunctionTypeID, TypeLayout::makeInlineValue(cFunctionValueWordCount, functionRefOffset, 1));
}

bool TypeLayoutTableVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool TypeLayoutTableVisitor::addLambdaEnvironmentLayouts()
{
    // Like for class and struct fields, we need to build a layout for GC to know what to scan.
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    TypeLayoutTableBuilder& typeLayouts = mCtx.mBackend.mTypeLayoutTable;

    for (LambdaNode* lambda : mCtx.mBackend.mLambdas)
    {
        // We need captures for that.
        if (lambda->mCaptures.empty())
        {
            continue;
        }

        // Go over all captures and collect the references for them in our offset vector.
        std::vector<u32> offsets;
        u32 environmentWords = 0;
        for (const LambdaCapture& capture : lambda->mCaptures)
        {
            // Get the type as usual.
            Type* captureType =
                capture.mKind == LambdaCaptureKind::cSymbol ? capture.mSymbol->mType : lambda->mLexicalThisType;
            if (collectRefsForField(captureType, capture.mEnvironmentOffset, offsets) == false)
            {
                return false;
            }

            // We (kinda) compute this twice (in the symbol layout visitor as well), but that's okay for now.
            environmentWords = capture.mEnvironmentOffset + layout::getStorageWordSizeForType(captureType);
        }

        // Append any references we need to track.
        TypeLayoutRefOffsetIndex refOffsetStart = 0;
        u64 requiredIndex = 0;
        if (typeLayouts.tryAppendRefOffsets(ArrayView<const u32>{offsets.data(), offsets.size()},
                                            refOffsetStart,
                                            requiredIndex) == false)
        {
            mCtx.report<cLambdaRefOffsetTableTooLarge>(lambda->mSourceRange,
                                                       requiredIndex,
                                                       cMaxTypeLayoutRefOffsetIndex);
            continue;
        }

        // Get the environment ID and register it with the relevant meta data in the table.
        TypeLayoutRefCount refCount = static_cast<TypeLayoutRefCount>(offsets.size());
        TypeID environmentTypeID = static_cast<TypeID>(lambda->mEnvironmentTypeID);
        TypeLayout layout = TypeLayout::makeReferenceObject(static_cast<TypeLayoutWordCount>(environmentWords),
                                                            refOffsetStart,
                                                            refCount);
        typeLayouts.setLayout(environmentTypeID, layout);
    }

    return mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool TypeLayoutTableVisitor::collectRefsForAggregate(AggregateType* agg, u32 baseWords, std::vector<u32>& fieldOffsets)
{
    // The type layout metadata tells GC which words of a value/payload contain references.
    // This means we have to flatten structs, and thus go recursively in collectRefsForField.
    // If the aggregate consists of primitives only, we don't have to scan it.
    if (agg->mIsPrimitive == true)
    {
        return true;
    }

    AggregateLayout* layout = agg->mLayout;
    for (const FieldLayout& field : layout->mFields)
    {
        Symbol* symbol = field.mSymbol;
        // We know the offset of the field, and use that to collect other potential inline refs.
        if (collectRefsForField(symbol->mType, baseWords + field.mOffset, fieldOffsets) == false)
        {
            return false;
        }
    }

    return true;
}

bool TypeLayoutTableVisitor::collectRefsForField(Type* type, u32 baseOffsetWords, std::vector<u32>& fieldOffsets)
{
    // Terminals are primitives, ptrs (classes/lists), and structs without ptrs.
    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        {
            auto* pt = static_cast<PrimitiveType*>(type);

            // If we have a string, we need to track it before we return.
            if (pt->mPrimitiveKind == PrimitiveTypeKind::cString)
            {
                fieldOffsets.push_back(baseOffsetWords);
            }

            return true;
        }
        case TypeKind::cList:
        {
            // Lists are heap references.
            fieldOffsets.push_back(baseOffsetWords);
            return true;
        }
        case TypeKind::cMap:
        {
            // Maps are heap references.
            fieldOffsets.push_back(baseOffsetWords);
            return true;
        }
        case TypeKind::cFunction:
        {
            // Function values reserve their first word for a closure environment reference.
            fieldOffsets.push_back(baseOffsetWords + cFunctionValueContextWord);
            return true;
        }
        case TypeKind::cStruct:
        {
            // If the type is a struct (and thus inlined), recurse.
            auto* a = static_cast<AggregateType*>(type);
            return collectRefsForAggregate(a, baseOffsetWords, fieldOffsets);
        }
        case TypeKind::cClass:
        case TypeKind::cInterface:
        {
            // If the type is a class or interface, the reference is just a pointer on the heap.
            fieldOffsets.push_back(baseOffsetWords);
            return true;
        }
        default:
        {
            SIMLANG_BREAK("Invalid type kind in type layout table visitor.");
            return false;
        }
    }
}

bool TypeLayoutTableVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // We only need to handle template instantiations.
    if (node->isTemplate())
    {
        return true;
    }

    TypeLayoutTableBuilder& typeLayouts = mCtx.mBackend.mTypeLayoutTable;

    Type* t = node->mSymbol->mType;
    if (t->mKind == TypeKind::cInterface)
    {
        // If this is an interface, register the index as type ID.
        TypeID typeID = static_cast<TypeID>(node->mSymbol->mIndex);
        // The interface value consists of object reference & interface dispatch index.
        // We're interested in the first word for GC, hence we need to track the word at 0.
        static constexpr u32 cInterfaceObjectOffset = 0;
        TypeLayoutRefOffsetIndex refOffsetStart = 0;
        u64 requiredIndex = 0;
        if (typeLayouts.tryAppendRefOffsets(ArrayView{&cInterfaceObjectOffset, 1}, refOffsetStart, requiredIndex) ==
            false)
        {
            mCtx.report<cTypeLayoutRefOffsetTableTooLarge>(node->mIdentifierRange,
                                                           node->mIdentifier,
                                                           requiredIndex,
                                                           cMaxTypeLayoutRefOffsetIndex);
            return false;
        }

        // Write the layout metadata.
        // 2 words, the offset start computed just before, for 1 ref.
        TypeLayout layout = TypeLayout::makeInlineValue(2, refOffsetStart, 1);
        typeLayouts.setLayout(typeID, layout);

        return true;
    }

    auto* agg = static_cast<AggregateType*>(t);
    TypeID typeID = static_cast<TypeID>(node->mSymbol->mIndex);
    u32 rawLayoutSizeWords = agg->mLayout->mSize;

    // We currently allow empty classes; if we have that, register a dummy word.
    // This is perhaps a bit messy and should rather go elsewhere.
    TypeLayoutWordCount layoutSizeWords = static_cast<TypeLayoutWordCount>(rawLayoutSizeWords);
    if (rawLayoutSizeWords == 0)
    {
        layoutSizeWords = 1;
    }

    std::vector<u32> offsets;

    if (agg->mIsPrimitive == false)
    {
        // If this is not a primitive, we need to process the refs.
        if (collectRefsForAggregate(agg, 0, offsets) == false)
        {
            return false;
        }
    }

    // Register the collected reference offsets for this type.
    TypeLayoutRefOffsetIndex refOffsetStart = 0;
    u64 requiredIndex = 0;
    if (typeLayouts.tryAppendRefOffsets(ArrayView<const u32>{offsets.data(), offsets.size()},
                                        refOffsetStart,
                                        requiredIndex) == false)
    {
        mCtx.report<cTypeLayoutRefOffsetTableTooLarge>(node->mIdentifierRange,
                                                       node->mIdentifier,
                                                       requiredIndex,
                                                       cMaxTypeLayoutRefOffsetIndex);
        return false;
    }

    // Build the type layout.
    TypeLayoutRefCount refCount = static_cast<TypeLayoutRefCount>(offsets.size());
    if (t->mKind == TypeKind::cStruct)
    {
        typeLayouts.setLayout(typeID, TypeLayout::makeStruct(layoutSizeWords, refOffsetStart, refCount));
    }
    else
    {
        typeLayouts.setLayout(typeID, TypeLayout::makeReferenceObject(layoutSizeWords, refOffsetStart, refCount));
    }

    return true;
}

} // namespace simlang
