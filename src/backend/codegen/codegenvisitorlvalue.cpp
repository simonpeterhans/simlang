#include <algorithm>

#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "backend/bytecode/bytecodebuilder.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/codegen/place.h"
#include "backend/layout/layout.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

enum class AssignmentOp : u8;

bool CodeGenVisitor::allocateTemporaryLocal(Type* type, LocalIdx& out)
{
    VMWord words = layout::getStorageWordSizeForType(type);

    // Find out if this would exceed the max local count, or the function frame size.
    u64 maxLocalWords = std::min(cMaxFrameWords, (cMaxLocalIdx + 1) - mCurrentFunctionInfo->mArgWords);
    u64 nextLocalWords = mCurrentFunctionInfo->mLocalWords + static_cast<u64>(words);
    if (nextLocalWords > maxLocalWords)
    {
        mCtx.report<cFunctionFrameTooLarge>(mBytecodeBuilder.getSourceRange(), nextLocalWords, "local", maxLocalWords);
        return false;
    }

    // The index starts at the next word after args and locals.
    u32 localIndex = mCurrentFunctionInfo->mArgWords + mCurrentFunctionInfo->mLocalWords;
    out = static_cast<LocalIdx>(localIndex);
    mCurrentFunctionInfo->mLocalWords = static_cast<FrameWordCount>(nextLocalWords);

    return true;
}

bool CodeGenVisitor::allocateTemporaryPlace(Type* type, Place& out)
{
    // Simple wrapper to allocate a temp and wrap it into a place.
    LocalIdx localIdx;
    if (allocateTemporaryLocal(type, localIdx) == false)
    {
        return false;
    }

    out = Place::makeLocalPlace(type, localIdx);

    return true;
}

static FieldOffset getMemberFieldOffset(MemberAccessNode* member)
{
    // Everything here has to be valid or one of the previous passes did something wrong.
    // Get the aggregate type and its layout.
    auto* aggregateType = static_cast<AggregateType*>(member->mReceiver->mResolvedType);
    const AggregateLayout* layout = aggregateType->mLayout;

    // Get the member index and use it to get the member offset from the layout.
    u32 memberIndex = static_cast<u32>(member->mSymbol->mIndex);
    return static_cast<FieldOffset>(layout->mFields[memberIndex].mOffset);
}

static IndexCallNode* findIndexedField(MemberAccessNode* member, FieldOffset& outOffset)
{
    // Handle stuff like a[i].b.c.d (e.g. for assignment).
    // We start from a member access and consume member accesses until we get to an index access.
    u32 offset = 0;
    ExpressionNode* receiver = member;
    while (receiver->mNodeType == NodeType::cMemberAccess)
    {
        auto* field = static_cast<MemberAccessNode*>(receiver);
        // Only inline struct fields contribute to the offset within a collection element.
        if (field->mReceiver->mResolvedType->mKind != TypeKind::cStruct)
        {
            return nullptr;
        }

        offset += getMemberFieldOffset(field);
        receiver = field->mReceiver;
    }

    // Once we stop having chained member accesses, we expect an index call.
    if (receiver->mNodeType != NodeType::cIndexCall)
    {
        return nullptr;
    }

    outOffset = static_cast<FieldOffset>(offset);

    return static_cast<IndexCallNode*>(receiver);
}

bool CodeGenVisitor::emitStructReceiverAddress(ExpressionNode* expr)
{
    // Handle stuff like a[i].m();
    FieldOffset offset = 0;
    IndexCallNode* index = nullptr;
    if (expr->mNodeType == NodeType::cIndexCall)
    {
        // If we're already at the index call, handle that.
        index = static_cast<IndexCallNode*>(expr);
    }
    else if (expr->mNodeType == NodeType::cMemberAccess)
    {
        // If we have chained access, handle that first (recursively).
        // We need to track the offset to understand where the field with the collection can be found.
        index = findIndexedField(static_cast<MemberAccessNode*>(expr), offset);
    }

    // If we had no index call, we can directly emit the address.
    if (index == nullptr)
    {
        return emitAddress(expr, AddressMode::cReadOnly);
    }

    // Otherwise, emit the receiver and index first.
    if (visit(index->mReceiver) == false || visit(index->mIndex) == false)
    {
        return false;
    }

    // Emit the map or list reference.
    if (index->mReceiver->mResolvedType->mKind == TypeKind::cMap)
    {
        emit<OpCode::cRefMapValue>();
    }
    else
    {
        emit<OpCode::cRefListElement>();
    }

    // Emit the offset (if we have one).
    emitFieldReference(offset);

    return true;
}

void CodeGenVisitor::emitIndexedFieldLoad(IndexCallNode* index, FieldOffset offset, OpWordCount words)
{
    if (index->mReceiver->mResolvedType->mKind == TypeKind::cMap)
    {
        emit<OpCode::cLoadMapValueField>(offset, words);
    }
    else
    {
        emit<OpCode::cLoadListElementField>(offset, words);
    }
}

void CodeGenVisitor::emitIndexedFieldStore(IndexCallNode* index, FieldOffset offset, OpWordCount words)
{
    if (index->mReceiver->mResolvedType->mKind == TypeKind::cMap)
    {
        emit<OpCode::cStoreMapValueField>(offset, words);
    }
    else
    {
        emit<OpCode::cStoreListElementField>(offset, words);
    }
}

bool CodeGenVisitor::tryGetDirectPlace(ExpressionNode* expr, Place& out)
{
    // Find out if we can directly emit into something without computing its address.
    // This is (currently?) the case for params, locals, globals, and struct access.

    out = Place();

    switch (expr->mNodeType)
    {
        case NodeType::cIdentifier:
        {
            auto* id = static_cast<IdentifierNode*>(expr);
            Symbol* s = id->mSymbol;

            if (findCurrentCapture(s) != nullptr)
            {
                return false;
            }

            if (s->mFlags.test(SymbolFlags::cInOut))
            {
                // If this is an inout parameter, we cannot directly emit into it.
                return false;
            }

            switch (s->mSymbolType)
            {
                case SymbolType::cStackVariable:
                case SymbolType::cParameter:
                {
                    // If this is a local, we can directly emit into it.
                    out = Place::makeLocalPlace(expr->mResolvedType, static_cast<LocalIdx>(s->mIndex));
                    return true;
                }
                case SymbolType::cGlobalVariable:
                {
                    // If this is a global, we can directly emit into it.
                    out = Place::makeGlobalPlace(expr->mResolvedType, static_cast<GlobalIdx>(s->mIndex));
                    return true;
                }
                default:
                {
                    return false;
                }
            }
        }
        case NodeType::cModuleAccess:
        {
            // For modules, only globals can be directly emitted into.
            auto* ma = static_cast<ModuleAccessNode*>(expr);
            Symbol* s = ma->mSymbol;

            if (s->mSymbolType != SymbolType::cGlobalVariable)
            {
                return false;
            }

            out = Place::makeGlobalPlace(expr->mResolvedType, static_cast<GlobalIdx>(s->mIndex));

            return true;
        }
        case NodeType::cMemberAccess:
        {
            // Member access can be direct if the receiver is a struct and the member is a variable.
            auto* member = static_cast<MemberAccessNode*>(expr);
            if (member->mSymbol->mSymbolType != SymbolType::cMemberVariable)
            {
                return false;
            }

            // Classes are on the heap, so that won't work.
            Type* receiverType = member->mReceiver->mResolvedType;
            if (receiverType->mKind != TypeKind::cStruct)
            {
                return false;
            }

            // Recurse in case we have something like a.b.c.
            Place basePlace;
            if (tryGetDirectPlace(member->mReceiver, basePlace) == false)
            {
                return false;
            }

            FieldOffset fieldOffset = getMemberFieldOffset(member);

            // Derive the place for the field from the base place.
            out = basePlace.derive(expr->mResolvedType, fieldOffset);

            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitReferenceToPlace(const Place& place)
{
    switch (place.mKind)
    {
        case Place::Kind::cLocal:
        {
            emit<OpCode::cRefLocal>(place.getLocalOffset());
            return true;
        }
        case Place::Kind::cGlobal:
        {
            emit<OpCode::cRefGlobal>(place.getGlobalOffset());
            return true;
        }
        case Place::Kind::cAddressOnStack:
        {
            // If the address is already on the stack, we don't need to do anything.
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitLoadFromPlace(const Place& place)
{
    if (place.mWords == 0)
    {
        // If we're trying to load 0 words from an address, we need to clean up the stack manually.
        // This is because the Load/Store ops pop the address off the stack as well.
        if (place.mKind == Place::Kind::cAddressOnStack)
        {
            emit<OpCode::cPop>();
        }

        return place.mKind != Place::Kind::cInvalid;
    }

    switch (place.mKind)
    {
        case Place::Kind::cLocal:
        {
            if (place.mWords == 1)
            {
                emit<OpCode::cLoadLocal>(place.getLocalOffset());
            }
            else
            {
                emit<OpCode::cLoadLocalN>(place.getLocalOffset(), static_cast<OpWordCount>(place.mWords));
            }
            return true;
        }
        case Place::Kind::cGlobal:
        {
            if (place.mWords == 1)
            {
                emit<OpCode::cLoadGlobal>(place.getGlobalOffset());
            }
            else
            {
                emit<OpCode::cLoadGlobalN>(place.getGlobalOffset(), static_cast<OpWordCount>(place.mWords));
            }
            return true;
        }
        case Place::Kind::cAddressOnStack:
        {
            if (place.mWords == 1)
            {
                emit<OpCode::cLoadRef>();
            }
            else
            {
                emit<OpCode::cLoadRefN>(static_cast<OpWordCount>(place.mWords));
            }
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitStoreToPlace(const Place& place)
{
    if (place.mWords == 0)
    {
        // Same idea as for the load, clean up the stack manually since we don't actually do a load/store.
        if (place.mKind == Place::Kind::cAddressOnStack)
        {
            emit<OpCode::cPop>();
        }

        return place.mKind != Place::Kind::cInvalid;
    }

    switch (place.mKind)
    {
        case Place::Kind::cLocal:
        {
            if (place.mWords == 1)
            {
                emit<OpCode::cStoreLocal>(place.getLocalOffset());
            }
            else
            {
                emit<OpCode::cStoreLocalN>(place.getLocalOffset(), static_cast<OpWordCount>(place.mWords));
            }
            return true;
        }
        case Place::Kind::cGlobal:
        {
            if (place.mWords == 1)
            {
                emit<OpCode::cStoreGlobal>(place.getGlobalOffset());
            }
            else
            {
                emit<OpCode::cStoreGlobalN>(place.getGlobalOffset(), static_cast<OpWordCount>(place.mWords));
            }
            return true;
        }
        case Place::Kind::cAddressOnStack:
        {
            if (place.mWords == 1)
            {
                emit<OpCode::cStoreRef>();
            }
            else
            {
                emit<OpCode::cStoreRefN>(static_cast<OpWordCount>(place.mWords));
            }
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitInto(ExpressionNode* expr, const Place& dst)
{
    // Resolve.
    if (visit(expr) == false)
    {
        return false;
    }

    // Emit the store into the place.
    return emitStoreToPlace(dst);
}

bool CodeGenVisitor::emitTemporaryAddress(ExpressionNode* expr)
{
    // We're trying to get a temporary address (e.g., for makeStruct().x).
    // Make a temporary place for the address.
    Place tempPlace;
    if (allocateTemporaryPlace(expr->mResolvedType, tempPlace) == false)
    {
        return false;
    }

    // Emit the value into the temporary place.
    if (emitInto(expr, tempPlace) == false)
    {
        return false;
    }

    // Push a reference to the temporary place.
    return emitReferenceToPlace(tempPlace);
}

bool CodeGenVisitor::emitAddress(ExpressionNode* expr, AddressMode mode)
{
    // If we can get a direct place for the expression, use that and push a reference to it.
    Place directPlace;
    if (tryGetDirectPlace(expr, directPlace))
    {
        return emitReferenceToPlace(directPlace);
    }

    switch (expr->mNodeType)
    {
        case NodeType::cIdentifier:
        {
            auto* idNode = static_cast<IdentifierNode*>(expr);
            Symbol* s = idNode->mSymbol;

            if (const LambdaCapture* capture = findCurrentCapture(s))
            {
                emit<OpCode::cRefCapture>(static_cast<FieldOffset>(capture->mEnvironmentOffset));
                return true;
            }

            // If this is an identifier, it can only be a reference.
            // All other addresses of direct identifiers are known at compile time.
            // (A local class variable is a direct place, so it will be handled earlier.)
            if ((s->mSymbolType == SymbolType::cStackVariable || s->mSymbolType == SymbolType::cParameter) &&
                s->mFlags.test(SymbolFlags::cInOut))
            {
                // The inout parameter holds an address word, so we can simply load that.
                emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(s->mIndex));
                return true;
            }

            return false;
        }
        case NodeType::cMemberAccess:
        {
            auto* memberNode = static_cast<MemberAccessNode*>(expr);
            Type* receiverType = memberNode->mReceiver->mResolvedType;

            FieldOffset fieldOffset = getMemberFieldOffset(memberNode);

            if (receiverType->mKind == TypeKind::cClass)
            {
                // If this is a class, resolve the receiver to get the class reference on the stack.
                if (visit(memberNode->mReceiver) == false)
                {
                    return false;
                }

                // Then, emit the field offset of the member we want the address for.
                emit<OpCode::cRefObjField>(fieldOffset);

                return true;
            }

            // If this is a struct, check whether this is a temporary (e.g., makeStruct().field).
            if (isAddressableExpression(memberNode->mReceiver) == false)
            {
                // Only allow reading from temporaries.
                if (mode != AddressMode::cReadOnly)
                {
                    return false;
                }

                // Emit the address of the temporary.
                if (emitTemporaryAddress(memberNode->mReceiver) == false)
                {
                    return false;
                }
            }
            // If it's not, directly emit the address.
            else if (emitAddress(memberNode->mReceiver, mode) == false)
            {
                return false;
            }

            // Then, emit the field as a reference.
            emitFieldReference(fieldOffset);

            return true;
        }
        case NodeType::cIndexCall:
        {
            // General indexed reads use a temporary snapshot; struct method receivers
            // are handled separately by emitStructReceiverAddress.
            if (mode != AddressMode::cReadOnly)
            {
                return false;
            }

            return emitTemporaryAddress(expr);
        }
        case NodeType::cModuleAccess:
        {
            return false;
        }
        case NodeType::cThis:
        {
            // Only struct receivers are addressable. Class receivers are object-reference
            // values and whole-"this" rebinding is rejected during type checking.
            if (expr->mResolvedType->mKind != TypeKind::cStruct)
            {
                return false;
            }

            if (const LambdaCapture* capture = findCurrentThisCapture())
            {
                FieldOffset offset = static_cast<FieldOffset>(capture->mEnvironmentOffset);
                emit<OpCode::cRefCapture>(offset);
                return true;
            }

            // A struct method's local 0 already contains its borrowed receiver address.
            emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));
            return true;
        }
        default:
        {
            // In all other cases, we only allow read-only from temporaries.
            // condition ? pointA : pointB
            // make Point { x: 1, y: 2 }
            // someFunctionReturningStruct()
            // cast<Point>(...)
            if (mode == AddressMode::cReadOnly)
            {
                return emitTemporaryAddress(expr);
            }
            return false;
        }
    }
}

bool CodeGenVisitor::canFuseLValueAccess(ExpressionNode* expr)
{
    // This is arguably less confusing than the name implies.
    // Fusing is possible if we either:
    // - Have member access for a variable.
    // - Have an index call.
    switch (expr->mNodeType)
    {
        case NodeType::cMemberAccess:
        {
            auto* member = static_cast<MemberAccessNode*>(expr);
            if (member->mSymbol->mSymbolType == SymbolType::cMemberVariable)
            {
                return true;
            }
            return false;
        }
        case NodeType::cIndexCall:
        {
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::tryEmitFusedLoadFromLValue(ExpressionNode* expr)
{
    // Here, we try to prevent the pattern of pushing an object, referencing a field/element, and then doing LoadRef.
    // Instead, we try to use direct field/element load ops after evaluating the receiver.
    switch (expr->mNodeType)
    {
        case NodeType::cMemberAccess:
        {
            // RefObjField, LoadRef -> cLoadObjField
            // RefField, LoadRef -> LoadRefField

            auto* member = static_cast<MemberAccessNode*>(expr);

            // Handle stuff like x = a[i].b.c.d.
            FieldOffset indexedOffset;
            if (IndexCallNode* index = findIndexedField(member, indexedOffset))
            {
                // Emit the receiver and index.
                if (visit(index->mReceiver) == false || visit(index->mIndex) == false)
                {
                    return false;
                }

                // Find out how many words we want to load and then do that.
                auto words = static_cast<OpWordCount>(layout::getWordSizeForType(expr->mResolvedType));
                emitIndexedFieldLoad(index, indexedOffset, words);

                return true;
            }

            // Get the receiver type and field offset.
            Type* receiverType = member->mReceiver->mResolvedType;
            FieldOffset fieldOffset = getMemberFieldOffset(member);
            u32 words = layout::getWordSizeForType(expr->mResolvedType);

            if (receiverType->mKind == TypeKind::cClass)
            {
                // If this is a class, first resolve the receiver expression to get it onto the stack.
                if (visit(member->mReceiver) == false)
                {
                    return false;
                }

                // Then, do a class field load based on whether we have one or multiple words.
                if (words == 1)
                {
                    emit<OpCode::cLoadObjField>(fieldOffset);
                }
                else
                {
                    emit<OpCode::cLoadObjFieldN>(fieldOffset, static_cast<OpWordCount>(words));
                }

                return true;
            }

            // If we're here, this is a struct member access through an address (class, inout param).
            if (isAddressableExpression(member->mReceiver))
            {
                // If the receiver is addressable, we can directly emit its address and load from that.
                if (emitAddress(member->mReceiver, AddressMode::cReadOnly) == false)
                {
                    return false;
                }
            }
            else
            {
                // If we need a temporary address, we have to make it.
                // That can be the case for something like getStruct().x.
                if (emitTemporaryAddress(member->mReceiver) == false)
                {
                    return false;
                }
            }

            // Then, do a ref field load based on whether we have one or multiple words.
            if (words == 1)
            {
                emit<OpCode::cLoadRefField>(fieldOffset);
            }
            else
            {
                emit<OpCode::cLoadRefFieldN>(fieldOffset, static_cast<OpWordCount>(words));
            }

            return true;
        }
        case NodeType::cIndexCall:
        {
            // Get the index call node and resolve the receiver and the index expressions.
            auto* index = static_cast<IndexCallNode*>(expr);
            u32 words = layout::getWordSizeForType(expr->mResolvedType);

            // Resolve the list and index.
            if (visit(index->mReceiver) == false || visit(index->mIndex) == false)
            {
                return false;
            }

            Type* receiverType = index->mReceiver->mResolvedType;
            if (receiverType->mKind == TypeKind::cMap)
            {
                emit<OpCode::cLoadMapValue>(static_cast<OpWordCount>(words));
            }
            else
            {
                emit<OpCode::cLoadListElement>(static_cast<OpWordCount>(words));
            }

            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::tryEmitFusedStoreIntoLValue(ExpressionNode* lhs, ExpressionNode* rhs)
{
    switch (lhs->mNodeType)
    {
        case NodeType::cMemberAccess:
        {
            auto* member = static_cast<MemberAccessNode*>(lhs);

            // Handle stuff like a[i].b.c.d = x.
            FieldOffset indexedOffset;
            if (IndexCallNode* index = findIndexedField(member, indexedOffset))
            {
                // Emit the receiver, index, and the rhs.
                if (visit(index->mReceiver) == false || visit(index->mIndex) == false || visit(rhs) == false)
                {
                    return false;
                }

                // Find out how many words we want to store and then do that.
                auto words = static_cast<OpWordCount>(layout::getWordSizeForType(lhs->mResolvedType));
                emitIndexedFieldStore(index, indexedOffset, words);

                return true;
            }

            // Get the receiver type and field offset.
            Type* receiverType = member->mReceiver->mResolvedType;
            FieldOffset fieldOffset = getMemberFieldOffset(member);
            u32 words = layout::getWordSizeForType(lhs->mResolvedType);

            if (receiverType->mKind == TypeKind::cClass)
            {
                // If this is a class, first resolve the receiver expression to get it onto the stack.
                // Also resolve the RHS to get the value to store onto the stack.
                if (visit(member->mReceiver) == false || visit(rhs) == false)
                {
                    return false;
                }

                // Then, do a class field store based on whether we have one or multiple words.

                if (words == 1)
                {
                    emit<OpCode::cStoreObjField>(fieldOffset);
                }
                else
                {
                    emit<OpCode::cStoreObjFieldN>(fieldOffset, static_cast<OpWordCount>(words));
                }
                return true;
            }

            if (emitAddress(member->mReceiver, AddressMode::cStorage) == false || visit(rhs) == false)
            {
                return false;
            }

            // If we're here, this is a struct member store through an address (class, inout param).
            // We don't allow something like getStruct().x = y, so this is simpler than the load case.
            if (words == 1)
            {
                emit<OpCode::cStoreRefField>(fieldOffset);
            }
            else
            {
                emit<OpCode::cStoreRefFieldN>(fieldOffset, static_cast<OpWordCount>(words));
            }
            return true;
        }
        case NodeType::cIndexCall:
        {
            // Get the index call node and resolve the receiver and the index expressions.
            // Also resolve the RHS to get the value to store onto the stack.
            auto* index = static_cast<IndexCallNode*>(lhs);
            u32 words = layout::getWordSizeForType(lhs->mResolvedType);

            // Emit the receiver, index, and push the rhs.
            if (visit(index->mReceiver) == false || visit(index->mIndex) == false || visit(rhs) == false)
            {
                return false;
            }

            Type* receiverType = index->mReceiver->mResolvedType;
            if (receiverType->mKind == TypeKind::cMap)
            {
                emit<OpCode::cStoreMapValue>(static_cast<OpWordCount>(words));
            }
            else
            {
                emit<OpCode::cStoreListElement>(static_cast<OpWordCount>(words));
            }

            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::tryEmitFusedCompoundAssignment(ExpressionNode* lhs, ExpressionNode* rhs, AssignmentOp op)
{
    // Fused compound assignment only works with 1 word types (i.e., addresses).
    // This method is for indirect (address) things like:
    // obj.field += rhs for classes.
    // refParam.field += rhs or obj.structField.field += rhs for structs.
    u32 words = layout::getWordSizeForType(lhs->mResolvedType);
    if (words != 1)
    {
        return false;
    }

    PrimitiveTypeKind typeKind = getPrimitiveKind(lhs->mResolvedType);

    switch (lhs->mNodeType)
    {
        case NodeType::cMemberAccess:
        {
            auto* member = static_cast<MemberAccessNode*>(lhs);

            // Handle stuff like a[i].b.c.d += x.
            FieldOffset indexedOffset;
            if (IndexCallNode* index = findIndexedField(member, indexedOffset))
            {
                // Emit the receiver and index.
                if (visit(index->mReceiver) == false || visit(index->mIndex) == false)
                {
                    return false;
                }

                // Keep the evaluated handle and index/key for the store after the RHS has run.
                // The list/map have their handle as well as the index/key on the stack, so copy that.
                emit<OpCode::cDupN>(static_cast<OpWordCount>(2));
                // Load the index.
                emitIndexedFieldLoad(index, indexedOffset, 1);

                // Resolve the rhs and emit the opcode.
                if (visit(rhs) == false || emitCompoundAssignmentOpcode(op, typeKind) == false)
                {
                    return false;
                }

                // Do the field store.
                // We only allow one word stuff here at the moment for compound, so do that.
                emitIndexedFieldStore(index, indexedOffset, 1);

                return true;
            }

            // Get the receiver type and field offset.
            Type* receiverType = member->mReceiver->mResolvedType;
            FieldOffset fieldOffset = getMemberFieldOffset(member);

            if (receiverType->mKind == TypeKind::cClass)
            {
                // If this is a class, first resolve the receiver expression to get it onto the stack.
                if (visit(member->mReceiver) == false)
                {
                    return false;
                }

                // Duplicate the receiver on the stack since we need it for the store afterward.
                emit<OpCode::cDup>();
                // Emit the class field load.
                emit<OpCode::cLoadObjField>(fieldOffset);
            }
            else
            {
                // Load the struct member from the address.
                // Emit the address.
                if (emitAddress(member->mReceiver, AddressMode::cStorage) == false)
                {
                    return false;
                }

                // Duplicate the receiver on the stack since we need it for the store afterward.
                emit<OpCode::cDup>();
                // Emit the ref field load.
                emit<OpCode::cLoadRefField>(fieldOffset);
            }

            // Now push the RHS and the compound assignment onto the stack.
            if (visit(rhs) == false || emitCompoundAssignmentOpcode(op, typeKind) == false)
            {
                return false;
            }

            // Emit the store based on whether we have a class or a struct.
            if (receiverType->mKind == TypeKind::cClass)
            {
                emit<OpCode::cStoreObjField>(fieldOffset);
            }
            else
            {
                emit<OpCode::cStoreRefField>(fieldOffset);
            }
            return true;
        }
        case NodeType::cIndexCall:
        {
            // Get the index call node and resolve the receiver and the index expressions.
            auto* index = static_cast<IndexCallNode*>(lhs);
            if (visit(index->mReceiver) == false || visit(index->mIndex) == false)
            {
                return false;
            }

            // Here, we need to duplicate the receiver and index expressions since we need both for load and store.
            emit<OpCode::cDupN>(static_cast<OpWordCount>(2));

            Type* receiverType = index->mReceiver->mResolvedType;
            if (receiverType->mKind == TypeKind::cMap)
            {
                emit<OpCode::cLoadMapValue>(static_cast<OpWordCount>(1));
            }
            else
            {
                emit<OpCode::cLoadListElement>(static_cast<OpWordCount>(1));
            }

            // Now push the RHS and the compound assignment onto the stack.
            if (visit(rhs) == false || emitCompoundAssignmentOpcode(op, typeKind) == false)
            {
                return false;
            }

            if (receiverType->mKind == TypeKind::cMap)
            {
                emit<OpCode::cStoreMapValue>(static_cast<OpWordCount>(1));
            }
            else
            {
                emit<OpCode::cStoreListElement>(static_cast<OpWordCount>(1));
            }

            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitLoadFromLValue(ExpressionNode* expr)
{
    // We're trying to load something.
    // Find out if we know the address at compile time.
    Place place;
    if (tryGetDirectPlace(expr, place))
    {
        return emitLoadFromPlace(place);
    }

    // Find out if we can fuse the load (a.b, a[i]).
    if (canFuseLValueAccess(expr))
    {
        return tryEmitFusedLoadFromLValue(expr);
    }

    // Emit a regular address.
    if (emitAddress(expr, AddressMode::cReadOnly) == false)
    {
        return false;
    }

    // Emit the load from the address on the stack.
    return emitLoadFromPlace(Place::makeAddressOnStackPlace(expr->mResolvedType));
}

bool CodeGenVisitor::emitStoreIntoLValue(ExpressionNode* lhs, ExpressionNode* rhs)
{
    // We're trying to store something.
    // Find out if we know the address at compile time.
    Place dst;
    if (tryGetDirectPlace(lhs, dst))
    {
        return emitInto(rhs, dst);
    }

    // Find out if we can fuse the load (a.b, a[i]).
    if (canFuseLValueAccess(lhs))
    {
        return tryEmitFusedStoreIntoLValue(lhs, rhs);
    }

    // Emit a regular address.
    if (emitAddress(lhs, AddressMode::cStorage) == false)
    {
        return false;
    }

    // Emit the store into the address on the stack.
    return emitInto(rhs, Place::makeAddressOnStackPlace(lhs->mResolvedType));
}

} // namespace simlang
