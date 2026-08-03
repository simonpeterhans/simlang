#include "ast/binaryop.h"
#include "ast/exprutils.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/codegen/place.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "symbol/symbol.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

void CodeGenVisitor::emitIntegerImmediate(i32 value)
{
    if (value >= 0)
    {
        if (value <= 0xFF)
        { // [0, 255]
            emit<OpCode::cPush8>(static_cast<u8>(value));
        }
        else if (value <= 0xFFFF)
        {
            // [256, 65535]
            emit<OpCode::cPush16>(static_cast<u16>(value));
        }
        else
        {
            // [65536, 2^32 - 1]
            emit<OpCode::cPush32>(static_cast<u32>(value));
        }
    }
    else
    {
        if (value >= -0x80)
        {
            // [-128, -1]
            emit<OpCode::cPush8S>(static_cast<i8>(value));
        }
        else if (value >= -0x8000)
        {
            // [-32768, -129]
            emit<OpCode::cPush16S>(static_cast<i16>(value));
        }
        else
        {
            // [-2^15, -32769]
            emit<OpCode::cPush32>(static_cast<u32>(value));
        }
    }
}

void CodeGenVisitor::emitFieldReference(u32 offset)
{
    // Only do this if the offset is non-zero, otherwise we can directly use the address.
    if (offset != 0)
    {
        emit<OpCode::cRefField>(static_cast<FieldOffset>(offset));
    }
}

bool CodeGenVisitor::emitComparisonOpCode(BinaryOp op, PrimitiveTypeKind operandKind)
{
    switch (operandKind)
    {
        case PrimitiveTypeKind::cBool:
        {
            switch (op)
            {
                case BinaryOp::cEQ: emit<OpCode::cIEQ>(); return true;
                case BinaryOp::cNE: emit<OpCode::cINE>(); return true;
                default: return false;
            }
        }
        case PrimitiveTypeKind::cInt:
        {
            switch (op)
            {
                case BinaryOp::cEQ: emit<OpCode::cIEQ>(); return true;
                case BinaryOp::cNE: emit<OpCode::cINE>(); return true;
                case BinaryOp::cLT: emit<OpCode::cILT>(); return true;
                case BinaryOp::cLE: emit<OpCode::cILE>(); return true;
                case BinaryOp::cGT: emit<OpCode::cIGT>(); return true;
                case BinaryOp::cGE: emit<OpCode::cIGE>(); return true;
                default: return false;
            }
        }
        case PrimitiveTypeKind::cFloat:
        {
            switch (op)
            {
                case BinaryOp::cEQ: emit<OpCode::cFEQ>(); return true;
                case BinaryOp::cNE: emit<OpCode::cFNE>(); return true;
                case BinaryOp::cLT: emit<OpCode::cFLT>(); return true;
                case BinaryOp::cLE: emit<OpCode::cFLE>(); return true;
                case BinaryOp::cGT: emit<OpCode::cFGT>(); return true;
                case BinaryOp::cGE: emit<OpCode::cFGE>(); return true;
                default: return false;
            }
        }
        case PrimitiveTypeKind::cString:
        {
            switch (op)
            {
                case BinaryOp::cEQ: emit<OpCode::cSEQ>(); return true;
                case BinaryOp::cNE: emit<OpCode::cSNE>(); return true;
                default: return false;
            }
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitReferenceEqualityOpCode(BinaryOp op)
{
    // References we can directly compare as integers.
    switch (op)
    {
        case BinaryOp::cEQ:
        {
            emit<OpCode::cIEQ>();
            return true;
        }
        case BinaryOp::cNE:
        {
            emit<OpCode::cINE>();
            return true;
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitInterfaceConversion(Type* fromType, InterfaceType* toType)
{
    // If we're converting from null, we can directly push the null reference value.
    if (fromType->mKind == TypeKind::cNull)
    {
        emitIntegerImmediate(cNullRef);
        return true;
    }

    // Otherwise, require this to be a class to convert from.
    if (fromType->mKind != TypeKind::cClass)
    {
        return false;
    }

    // Get (or build) the interface index.
    u32 tableIndex = getInterfaceTableIndex(static_cast<AggregateType*>(fromType), toType);
    if (tableIndex == cInvalidInterfaceMethodTableIndex)
    {
        return false;
    }

    emit<OpCode::cPush32>(tableIndex);

    return true;
}

bool CodeGenVisitor::emitPrimitiveConversion(PrimitiveTypeKind fromKind, PrimitiveTypeKind toKind)
{
    // We may not have to do anything if the types match already.
    if (fromKind == toKind)
    {
        return fromKind != PrimitiveTypeKind::cInvalid;
    }

    // Note that implicit casting currently only allows int -> float.

    if (fromKind == PrimitiveTypeKind::cInt && toKind == PrimitiveTypeKind::cFloat)
    {
        emit<OpCode::cI2F>();
        return true;
    }

    if (fromKind == PrimitiveTypeKind::cFloat && toKind == PrimitiveTypeKind::cInt)
    {
        emit<OpCode::cF2I>();
        return true;
    }

    // These we can trivially convert.
    if (fromKind == PrimitiveTypeKind::cBool && toKind == PrimitiveTypeKind::cInt)
    {
        return true;
    }

    if (fromKind == PrimitiveTypeKind::cBool && toKind == PrimitiveTypeKind::cFloat)
    {
        emit<OpCode::cI2F>();
        return true;
    }

    // Here we have to be more careful (note that these are explicit casts).
    // We emit 0 for false and 1 for true, so something like cast<bool>(2) should push 1.
    // Otherwise, something like (cast<bool>(2) == true) would return false.
    // Maybe there's a smarter way to do this.

    if (fromKind == PrimitiveTypeKind::cInt && toKind == PrimitiveTypeKind::cBool)
    {
        emit<OpCode::cPush8>(static_cast<u8>(0));
        emit<OpCode::cINE>();
        return true;
    }

    if (fromKind == PrimitiveTypeKind::cFloat && toKind == PrimitiveTypeKind::cBool)
    {
        emit<OpCode::cPush8>(static_cast<u8>(0));
        emit<OpCode::cFNE>();
        return true;
    }

    return false;
}

bool CodeGenVisitor::emitInterfaceObjectRefFromPlace(const Place& place)
{
    // Interfaces are typically passed around as 2 words (due to the table index).
    // Here, we only care about the first word (the object reference) from the place.
    Place objectPlace = place;
    objectPlace.mWords = 1;
    return emitLoadFromPlace(objectPlace);
}

bool CodeGenVisitor::emitInterfaceObjectRef(ExpressionNode* expr)
{
    // Here, we only want the interface object reference (and not the table index).
    // Shortcut for implicit casts since we already have the correct stack shape.
    if (expr->mNodeType == NodeType::cImplicitCast)
    {
        auto* cast = static_cast<ImplicitCastNode*>(expr);
        if (cast->mResolvedType->mKind == TypeKind::cInterface)
        {
            // If we're casting to null or a class, we already have the address on the ToS.
            // That means we don't have to pop the table index.
            TypeKind childKind = cast->mTarget->mResolvedType->mKind;
            if (childKind == TypeKind::cClass || childKind == TypeKind::cNull)
            {
                return visit(cast->mTarget);
            }
        }
    }

    // Otherwise, figure out if we can directly load this.
    Place place;
    if (tryGetDirectPlace(expr, place))
    {
        // If we can, just load the interface object reference without the table index.
        return emitInterfaceObjectRefFromPlace(place);
    }

    // Otherwise, find out if the expression is addressable (possibly on the heap).
    if (isAddressableExpression(expr))
    {
        // If it is, load the thing.
        if (emitAddress(expr, AddressMode::cReadOnly) == false)
        {
            return false;
        }

        // It's now on the stack, so we can load the object reference from that (and ignore the table index).
        Place stackPlace = Place::makeAddressOnStackPlace(expr->mResolvedType);
        return emitInterfaceObjectRefFromPlace(stackPlace);
    }

    // If it's not addressable, we need to go the slow way and evaluate it.
    if (visit(expr) == false)
    {
        return false;
    }

    // Evaluating an interface object left the object reference and the table index on the stack, so pop the latter.
    emit<OpCode::cPop>();

    return true;
}

bool CodeGenVisitor::emitInterfaceEqualityComparison(BinaryOp op, ExpressionNode* lhs, ExpressionNode* rhs)
{
    // Push the object references.
    if (emitInterfaceObjectRef(lhs) == false || emitInterfaceObjectRef(rhs) == false)
    {
        return false;
    }

    // Do the equality through references.
    return emitReferenceEqualityOpCode(op);
}

bool CodeGenVisitor::emitEqualityComparison(BinaryOp op, Type* type, const Place& lhs, const Place& rhs)
{
    // All of these pretty much follow the same pattern -- load lhs/rhs, then do the comparison.
    switch (type->mKind)
    {
        case TypeKind::cPrimitive:
        {
            if (emitLoadFromPlace(lhs) == false || emitLoadFromPlace(rhs) == false)
            {
                return false;
            }

            PrimitiveTypeKind kind = getPrimitiveKind(type);
            return emitComparisonOpCode(op, kind);
        }
        case TypeKind::cClass:
        case TypeKind::cList:
        case TypeKind::cMap:
        {
            if (emitLoadFromPlace(lhs) == false || emitLoadFromPlace(rhs) == false)
            {
                return false;
            }

            return emitReferenceEqualityOpCode(op);
        }
        case TypeKind::cInterface:
        {
            if (emitInterfaceObjectRefFromPlace(lhs) == false || emitInterfaceObjectRefFromPlace(rhs) == false)
            {
                return false;
            }

            return emitReferenceEqualityOpCode(op);
        }
        case TypeKind::cStruct:
        {
            return emitStructEqualityComparison(op, static_cast<AggregateType*>(type), lhs, rhs);
        }
        default:
        {
            return false;
        }
    }
}

bool CodeGenVisitor::emitStructEqualityComparison(BinaryOp op,
                                                  AggregateType* structType,
                                                  const Place& lhs,
                                                  const Place& rhs)
{
    // Compare field by field, jump to the end as soon as we fail.
    u32 notEqualLabel = makeLabel();
    u32 endLabel = makeLabel();
    const AggregateLayout* layout = structType->mLayout;

    // We have places for the struct base, so iterate over all fields.
    for (const FieldLayout& field : layout->mFields)
    {
        Type* fieldType = field.mSymbol->mType;

        // Derive the place as an offset from the base.
        Place lhsField = lhs.derive(fieldType, field.mOffset);
        Place rhsField = rhs.derive(fieldType, field.mOffset);

        // Emit the equality comparison for the fields.
        if (emitEqualityComparison(BinaryOp::cEQ, fieldType, lhsField, rhsField) == false)
        {
            return false;
        }

        // As soon as we have one comparison that failed, jump to the end.
        emit<OpCode::cJumpZ>(notEqualLabel);
    }

    // Do awkward 0/1 pushing based on the result.
    emit<OpCode::cPush8>(static_cast<u8>(op == BinaryOp::cEQ ? 1 : 0));
    emit<OpCode::cJump>(endLabel);
    emit<OpCode::cLabel>(notEqualLabel);
    emit<OpCode::cPush8>(static_cast<u8>(op == BinaryOp::cEQ ? 0 : 1));
    emit<OpCode::cLabel>(endLabel);

    return true;
}

bool CodeGenVisitor::emitStructEqualityOperands(BinaryOpNode* node)
{
    auto* structType = static_cast<AggregateType*>(node->mLeft->mResolvedType);

    Place lhs;
    Place rhs;
    bool lhsDirect = tryGetDirectPlace(node->mLeft, lhs);
    bool rhsDirect = tryGetDirectPlace(node->mRight, rhs);

    // If both operands are direct, we can compare them directly.
    if (lhsDirect && rhsDirect)
    {
        return emitStructEqualityComparison(node->mOp, structType, lhs, rhs);
    }

    // Otherwise, we create a temporary place to hold the value of the left operand.
    // That is a copy in case we do something like a == mutateA().
    Place lhsTemp;
    if (allocateTemporaryPlace(structType, lhsTemp) == false)
    {
        return false;
    }

    // If it's direct, load from lhs and store into the temp.
    // Otherwise, we need to first evaluate the lhs into the temp.
    if (lhsDirect)
    {
        if (emitLoadFromPlace(lhs) == false || emitStoreToPlace(lhsTemp) == false)
        {
            return false;
        }
    }
    else if (emitInto(node->mLeft, lhsTemp) == false)
    {
        return false;
    }

    // We're now using the temp as lhs.
    lhs = lhsTemp;

    if (rhsDirect == false)
    {
        // If the rhs is not direct, evaluate it into the temp.
        if (allocateTemporaryPlace(structType, rhs) == false || emitInto(node->mRight, rhs) == false)
        {
            return false;
        }
    }

    // Emit comparison stuff for the temps.
    return emitStructEqualityComparison(node->mOp, structType, lhs, rhs);
}

bool CodeGenVisitor::emitArithmeticOrBitwiseOpcode(BinaryOp op, PrimitiveTypeKind resultKind)
{
    switch (op)
    {
        case BinaryOp::cAdd:
        {
            switch (resultKind)
            {
                case PrimitiveTypeKind::cInt: emit<OpCode::cIAdd>(); return true;
                case PrimitiveTypeKind::cFloat: emit<OpCode::cFAdd>(); return true;
                default: return false;
            }
        }
        case BinaryOp::cSub:
        {
            switch (resultKind)
            {
                case PrimitiveTypeKind::cInt: emit<OpCode::cISub>(); return true;
                case PrimitiveTypeKind::cFloat: emit<OpCode::cFSub>(); return true;
                default: return false;
            }
        }
        case BinaryOp::cMul:
        {
            switch (resultKind)
            {
                case PrimitiveTypeKind::cInt: emit<OpCode::cIMul>(); return true;
                case PrimitiveTypeKind::cFloat: emit<OpCode::cFMul>(); return true;
                default: return false;
            }
        }
        case BinaryOp::cDiv:
        {
            switch (resultKind)
            {
                case PrimitiveTypeKind::cInt: emit<OpCode::cIDiv>(); return true;
                case PrimitiveTypeKind::cFloat: emit<OpCode::cFDiv>(); return true;
                default: return false;
            }
        }
        case BinaryOp::cMod:
        {
            switch (resultKind)
            {
                case PrimitiveTypeKind::cInt: emit<OpCode::cIMod>(); return true;
                case PrimitiveTypeKind::cFloat: emit<OpCode::cFMod>(); return true;
                default: return false;
            }
        }

        case BinaryOp::cShiftL: emit<OpCode::cIShL>(); return true;
        case BinaryOp::cShiftR: emit<OpCode::cIShR>(); return true;
        case BinaryOp::cBitOr: emit<OpCode::cIOr>(); return true;
        case BinaryOp::cBitAnd: emit<OpCode::cIAnd>(); return true;
        case BinaryOp::cBitXor: emit<OpCode::cIXor>(); return true;

        default:
        {
            return false;
        }
    }
}

} // namespace simlang
