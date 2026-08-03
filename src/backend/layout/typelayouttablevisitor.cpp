#include "backend/layout/typelayouttablevisitor.h"

#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
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
}

bool TypeLayoutTableVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
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
        TypeLayoutRefOffsetIndex refOffsetStart = typeLayouts.getRefOffsetCount();
        u64 requiredEnd = static_cast<u64>(refOffsetStart) + 1U;
        if (requiredEnd > cMaxTypeLayoutRefOffsetIndex + 1)
        {
            u64 requiredIndex = requiredEnd - 1;
            mCtx.report<cTypeLayoutRefOffsetTableTooLarge>(node->mIdentifierRange,
                                                           node->mIdentifier,
                                                           requiredIndex,
                                                           cMaxTypeLayoutRefOffsetIndex);
            return false;
        }

        typeLayouts.setInterfaceType(typeID);
        return true;
    }

    auto* agg = static_cast<AggregateType*>(t);
    TypeID typeID = static_cast<TypeID>(node->mSymbol->mIndex);
    u32 rawLayoutSizeWords = agg->mLayout->mSize;
    SIMLANG_ASSERTM(rawLayoutSizeWords <= cMaxTypeLayoutWordCount,
                    "Type layout exceeded packed word count after aggregate layout.");

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

    // We collected all offsets that have to be checked for this type.
    // Register the offsets.
    TypeLayoutRefOffsetIndex refOffsetStart = 0;
    SIMLANG_ASSERTM(offsets.size() <= cMaxTypeLayoutRefCount,
                    "Type layout reference count exceeded packed ref count after aggregate layout.");

    for (u32 offset : offsets)
    {
        SIMLANG_ASSERTM(offset <= cMaxTypeLayoutRefOffset,
                        "Type layout reference offset exceeded packed ref offset after aggregate layout.");
    }

    TypeLayoutRefCount offsetCount = static_cast<TypeLayoutRefCount>(offsets.size());

    if (offsetCount > 0)
    {
        refOffsetStart = typeLayouts.getRefOffsetCount();
        u64 requiredEnd = static_cast<u64>(refOffsetStart) + offsetCount;
        if (requiredEnd > cMaxTypeLayoutRefOffsetIndex + 1)
        {
            u64 requiredIndex = requiredEnd - 1;
            mCtx.report<cTypeLayoutRefOffsetTableTooLarge>(node->mIdentifierRange,
                                                           node->mIdentifier,
                                                           requiredIndex,
                                                           cMaxTypeLayoutRefOffsetIndex);
            return false;
        }

        for (u32 offset : offsets)
        {
            typeLayouts.appendRefOffset(static_cast<TypeLayoutRefOffset>(offset));
        }
    }

    // Build the type layout.
    if (t->mKind == TypeKind::cStruct)
    {
        typeLayouts.setLayout(typeID, TypeLayout::makeStruct(layoutSizeWords, refOffsetStart, offsetCount));
    }
    else
    {
        typeLayouts.setLayout(typeID, TypeLayout::makeReferenceObject(layoutSizeWords, refOffsetStart, offsetCount));
    }

    return true;
}

} // namespace simlang
