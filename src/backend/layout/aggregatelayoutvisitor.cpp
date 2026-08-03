#include "backend/layout/aggregatelayoutvisitor.h"

#include <vector>

#include "ast/nodes/stmtnodes.h"
#include "backend/layout/layout.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typeformat.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

AggregateLayoutVisitor::AggregateLayoutVisitor(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool AggregateLayoutVisitor::run(ASTNode* node)
{
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();
    bool traversalOk = visit(node);
    return traversalOk && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

bool AggregateLayoutVisitor::buildValueTypeLayout(Type* type)
{
    switch (type->mKind)
    {
        case TypeKind::cStruct:
        {
            // Structs have to be laid out.
            const AggregateLayout* layout = buildLayout(static_cast<AggregateType*>(type));
            return layout != nullptr;
        }
        default:
        {
            // Otherwise, we're good to go.
            return true;
        }
    }
}

bool AggregateLayoutVisitor::isPrimitiveType(Type* type)
{
    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        {
            // If the type holds a string, it's not a primitive.
            auto* pt = static_cast<PrimitiveType*>(type);
            return (pt->mPrimitiveKind != PrimitiveTypeKind::cString);
        }
        case TypeKind::cStruct:
        {
            // For structs, we need to check if they hold a pointer.
            auto* agg = static_cast<AggregateType*>(type);
            if (buildLayout(agg) == nullptr)
            {
                return false;
            }

            return agg->mIsPrimitive;
        }
        case TypeKind::cList:
        case TypeKind::cMap:
        case TypeKind::cClass:
        case TypeKind::cInterface:
        {
            // All of these are references.
            return false;
        }
        case TypeKind::cFunction:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

const AggregateLayout* AggregateLayoutVisitor::buildLayout(AggregateType* type)
{
    // If we already have a layout (e.g. because another type needed the information earlier), we're done.
    if (type->mLayout != nullptr)
    {
        return type->mLayout;
    }

    // Make sure we're not already trying to resolve this.
    if (mInProgress.find(type) != mInProgress.end())
    {
        auto* decl = static_cast<TypeDeclarationStatementNode*>(type->mSymbol->mDeclNode);
        mCtx.report<cRecursiveValueType>(decl->mIdentifierRange, typeToString(type));
        return nullptr;
    }

    // We're currently processing this one.
    mInProgress.insert(type);
    // Remove it when we're done.
    OnScopeEnd removeInProgress(
        [&]()
        {
            mInProgress.erase(type);
        });

    std::vector<FieldLayout> fields;
    fields.reserve(type->mSymbol->mMembers.size());

    u64 wordOffset = 0;
    bool isPrimitive = true;

    // Build the fields.
    for (Symbol* member : type->mSymbol->mMembers)
    {
        if (member->mSymbolType != SymbolType::cMemberVariable)
        {
            // We only care about variables here when layouting.
            continue;
        }

        Type* fieldType = member->mType;
        switch (fieldType->mKind)
        {
            case TypeKind::cStruct:
            {
                // Process structs (everything else has word size).
                if (buildValueTypeLayout(fieldType) == false)
                {
                    return nullptr;
                }
                break;
            }
            default:
            {
                break;
            }
        }

        // The aggregate can only be primitive if all members are primitives.
        isPrimitive = isPrimitive && isPrimitiveType(fieldType);

        // Find out if we've reached the packed runtime layout word limit after processing this field.
        u32 fieldWords = layout::getWordSizeForType(fieldType);
        u64 nextWordOffset = wordOffset + fieldWords;
        if (nextWordOffset > cMaxTypeLayoutWordCount)
        {
            auto* decl = static_cast<TypeDeclarationStatementNode*>(type->mSymbol->mDeclNode);
            mCtx.report<cTypeLayoutTooLarge>(decl->mIdentifierRange,
                                             decl->mIdentifier,
                                             nextWordOffset,
                                             cMaxTypeLayoutWordCount);
            return nullptr;
        }

        fields.push_back(FieldLayout{member, static_cast<u32>(wordOffset)});
        wordOffset = nextWordOffset;
    }

    // Write the layout data.
    auto* layout = mCtx.allocate<AggregateLayout>();
    layout->mSize = static_cast<u32>(wordOffset);
    layout->mFields = makeArrayView(mCtx.mAllocator, fields);

    // Set it for the type.
    type->mLayout = layout;

    // Track whether the type is considered a primitive.
    type->mIsPrimitive = isPrimitive;

    return type->mLayout;
}

bool AggregateLayoutVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // The templates itself don't need to be visited.
    if (node->isTemplate())
    {
        return true;
    }

    // Interfaces need no layout either, we're only interested in structs and classes.
    Type* type = node->mSymbol->mType;
    if (type->mKind != TypeKind::cStruct && type->mKind != TypeKind::cClass)
    {
        return true;
    }

    // Build it.
    auto* aggregateType = static_cast<AggregateType*>(type);
    const AggregateLayout* layout = buildLayout(aggregateType);

    return layout != nullptr;
}

} // namespace simlang
