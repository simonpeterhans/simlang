#include <utility>

#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/unaryop.h"
#include "backend/backendstate.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/codegen/place.h"
#include "backend/stringdata.h"
#include "backend/typeidutils.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"
#include "runtime/stringdata.h"
#include "runtime/vmdefines.h"
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
#include "util/types.h"

namespace simlang
{

bool CodeGenVisitor::visitImplicitCast(ImplicitCastNode* node)
{
    // Emit the target.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // If we have the same types, do nothing.
    Type* fromType = node->mTarget->mResolvedType;
    Type* toType = node->mResolvedType;
    if (fromType == toType)
    {
        return true;
    }

    // If we implicitly cast to an interface, handle that (we might have to build the index first).
    if (toType->mKind == TypeKind::cInterface)
    {
        return emitInterfaceConversion(fromType, static_cast<InterfaceType*>(toType));
    }

    // Null is already represented by cNullRef, no cast needed.
    if (fromType->mKind == TypeKind::cNull &&
        (toType->mKind == TypeKind::cClass || toType->mKind == TypeKind::cList || toType->mKind == TypeKind::cMap))
    {
        return true;
    }

    // Otherwise, this is a primitive conversion.
    return emitPrimitiveConversion(getPrimitiveKind(fromType), getPrimitiveKind(toType));
}

bool CodeGenVisitor::visitCast(CastNode* node)
{
    // Emit the target.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // If we have the same types, do nothing.
    Type* fromType = node->mTarget->mResolvedType;
    Type* toType = node->mResolvedType;
    if (fromType == toType)
    {
        return true;
    }

    // If we implicitly cast to an interface, handle that (we might have to build the index first).
    if (toType->mKind == TypeKind::cInterface)
    {
        return emitInterfaceConversion(fromType, static_cast<InterfaceType*>(toType));
    }

    // If we're casting from an interface to a class, handle that.
    if (fromType->mKind == TypeKind::cInterface && toType->mKind == TypeKind::cClass)
    {
        // Pop the table index, which we don't need for the conversion.
        emit<OpCode::cPop>();

        TypeID typeID;
        if (getRuntimeTypeID(toType, typeID) == false)
        {
            return false;
        }

        // Push the type we're casting to and do the cast.
        emit<OpCode::cCheckCast>(typeID);

        return true;
    }

    // Null is already represented by cNullRef, no cast needed.
    if (fromType->mKind == TypeKind::cNull &&
        (toType->mKind == TypeKind::cClass || toType->mKind == TypeKind::cList || toType->mKind == TypeKind::cMap))
    {
        return true;
    }

    // Otherwise, this is a primitive conversion.
    return emitPrimitiveConversion(getPrimitiveKind(fromType), getPrimitiveKind(toType));
}

bool CodeGenVisitor::visitIdentifier(IdentifierNode* node)
{
    // If this is a constexpr, we can try to emit the value directly.
    if (node->mSymbol->mFlags.test(SymbolFlags::cConstExpr))
    {
        Symbol* s = node->mSymbol;

        if (node->mResolvedType->mKind == TypeKind::cPrimitive)
        {
            switch (s->mConstValue.mPrimitiveKind)
            {
                case PrimitiveTypeKind::cInt:
                {
                    // Don't emit more than we have to (i8/i16 if possible).
                    emitIntegerImmediate(s->mConstValue.as.mInteger);
                    return true;
                }
                case PrimitiveTypeKind::cFloat:
                {
                    // Push the float as u32.
                    u32 val = bits::bitCast<u32>(s->mConstValue.as.mFloat);
                    emit<OpCode::cPush32>(val);
                    return true;
                }
                case PrimitiveTypeKind::cBool:
                {
                    // Bools are 0 or 1.
                    u8 val = (s->mConstValue.as.mBool) ? 1U : 0U;
                    emit<OpCode::cPush8>(val);
                    return true;
                }
                case PrimitiveTypeKind::cString:
                {
                    // Register the string literal and use that index.
                    StringLiteralIdx stringIndex;
                    if (mCtx.mBackend.mStrings.getLiteralIndex(s->mConstValue.as.mString, stringIndex) == false)
                    {
                        SIMLANG_BREAK("Constexpr string missing from string layout.");
                        return false;
                    }

                    emit<OpCode::cPushString>(stringIndex);

                    return true;
                }
                default:
                {
                    break;
                }
            }
        }
    }

    // Otherwise, load the lvalue.
    return emitLoadFromLValue(node);
}

bool CodeGenVisitor::visitThis(ThisNode* node)
{
    // Note that this path is only taken if "this" is used as an rvalue.
    // If it's used as an lvalue, it should go through the storage address path.
    // Classes: "this" is always local 0, so we can just load that and be done.
    if (node->mResolvedType->mKind == TypeKind::cClass)
    {
        emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));
        return true;
    }

    // Structs: Since they are value types, we need to push the entire thing onto the stack.
    // Load "this" (struct address) at local 0.
    emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));

    // Then push the entire thing onto the stack.
    return emitLoadFromPlace(Place::makeAddressOnStackPlace(node->mResolvedType));
}

bool CodeGenVisitor::visitIntLiteral(IntLiteralNode* node)
{
    emitIntegerImmediate(node->mInt);
    return true;
}

bool CodeGenVisitor::visitFloatLiteral(FloatLiteralNode* node)
{
    u32 val = bits::bitCast<u32>(node->mFloat);
    emit<OpCode::cPush32>(val);
    return true;
}

bool CodeGenVisitor::visitBoolLiteral(BoolLiteralNode* node)
{
    u8 val = (node->mBool) ? 1U : 0U;
    emit<OpCode::cPush8>(val);
    return true;
}

bool CodeGenVisitor::visitStringLiteral(StringLiteralNode* node)
{
    StringLiteralIdx stringIndex;
    if (mCtx.mBackend.mStrings.getLiteralIndex(node->mString, stringIndex) == false)
    {
        SIMLANG_BREAK("String literal missing from string layout.");
        return false;
    }

    emit<OpCode::cPushString>(stringIndex);

    return true;
}

bool CodeGenVisitor::visitNullLiteral(NullLiteralNode*)
{
    emitIntegerImmediate(cNullRef);
    return true;
}

bool CodeGenVisitor::visitFormatString(FormatStringNode* node)
{
    StringFormatTemplateBuilder builder;

    // Add all literals we have in the template.
    for (const InternedString* str : node->mLiterals)
    {
        StringLiteralIdx literalIndex;
        if (mCtx.mBackend.mStrings.getLiteralIndex(str, literalIndex) == false)
        {
            SIMLANG_BREAK("Format string literal missing from string layout.");
            return false;
        }

        builder.appendLiteral(literalIndex, str->mLength);
    }

    // Emit all args and track their kinds.
    for (ExpressionNode* arg : node->mArgs)
    {
        if (visit(arg) == false)
        {
            return false;
        }

        builder.appendArgKind(getStringFormatArgKind(getPrimitiveKind(arg->mResolvedType)));
    }

    // Build the template and register it.
    StringFormatIdx formatIndex;
    StringFormatTemplate tmpl = std::move(builder).build();
    if (mCtx.mBackend.mStringFormats.getTemplateIndex(tmpl, formatIndex) == false)
    {
        SIMLANG_BREAK("String format missing from string layout.");
        return false;
    }

    // Finally emit the index.
    emit<OpCode::cFormatString>(formatIndex);

    return true;
}

bool CodeGenVisitor::visitNewObject(NewObjectNode* node)
{
    if (node->mResolvedType->mKind == TypeKind::cList)
    {
        return emitNewList(node, static_cast<ListType*>(node->mResolvedType));
    }

    if (node->mResolvedType->mKind == TypeKind::cMap)
    {
        return emitNewMap(node, static_cast<MapType*>(node->mResolvedType));
    }

    auto* aggregateType = static_cast<AggregateType*>(node->mResolvedType);
    if (aggregateType->mKind == TypeKind::cClass)
    {
        return emitNewClass(node, aggregateType);
    }

    return emitNewStruct(node, aggregateType);
}

bool CodeGenVisitor::visitFunctionCall(FunctionCallNode* node)
{
    if (node->mReceiver->mNodeType == NodeType::cMemberAccess)
    {
        auto* memberAccess = static_cast<MemberAccessNode*>(node->mReceiver);
        if (memberAccess->mReceiver->mResolvedType->mKind == TypeKind::cList)
        {
            return emitListMethodCall(node, memberAccess);
        }

        if (memberAccess->mReceiver->mResolvedType->mKind == TypeKind::cMap)
        {
            return emitMapMethodCall(node, memberAccess);
        }

        return emitMethodCall(node, memberAccess);
    }

    return emitFreeFunctionOrSyscallCall(node);
}

bool CodeGenVisitor::visitIndexCall(IndexCallNode* node)
{
    return emitLoadFromLValue(node);
}

bool CodeGenVisitor::visitMemberAccess(MemberAccessNode* node)
{
    // Methods are handled via function call.
    if (node->mSymbol->mSymbolType == SymbolType::cMemberFunction)
    {
        // This could also be an assert.
        return false;
    }

    return emitLoadFromLValue(node);
}

bool CodeGenVisitor::visitModuleAccess(ModuleAccessNode* node)
{
    // We simply delegate to the RHS which has all the information needed.
    return visit(node->mRight);
}

bool CodeGenVisitor::visitUnaryOp(UnaryOpNode* node)
{
    if (node->mOp == UnaryOp::cNeg)
    {
        // Push the child node, then emit the negation operation.
        if (visit(node->mExpr) == false)
        {
            return false;
        }

        PrimitiveTypeKind ptk = getPrimitiveKind(node->mExpr->mResolvedType);
        if (ptk == PrimitiveTypeKind::cInt)
        {
            emit<OpCode::cINeg>();
        }
        else
        {
            emit<OpCode::cFNeg>();
        }

        return true;
    }

    // Push the child node, then emit the bitwise not operation.
    if (visit(node->mExpr) == false)
    {
        return false;
    }

    emit<OpCode::cINot>();

    return true;
}

bool CodeGenVisitor::visitBinaryOp(BinaryOpNode* node)
{
    // If this is || or &&, handle it differently with short-circuiting.
    if (node->mOp == BinaryOp::cAnd || node->mOp == BinaryOp::cOr)
    {
        return emitBoolAsValue(node);
    }

    Type* leftType = node->mLeft->mResolvedType;
    Type* rightType = node->mRight->mResolvedType;

    // Equality is defined for more types, so handle these here.
    if (node->mOp == BinaryOp::cEQ || node->mOp == BinaryOp::cNE)
    {
        if (leftType->mKind == TypeKind::cStruct)
        {
            return emitStructEqualityOperands(node);
        }

        if (leftType->mKind == TypeKind::cInterface && rightType->mKind == TypeKind::cInterface)
        {
            return emitInterfaceEqualityComparison(node->mOp, node->mLeft, node->mRight);
        }

        // This one is awkward, but interfaces are [objectHandle, interfaceTableIndex].
        // So if we're comparing to null, we need to make the interface expression an object reference.
        // (This is done via pop to remove the table index from the stack.)
        if ((leftType->mKind == TypeKind::cInterface || rightType->mKind == TypeKind::cInterface) &&
            (leftType->mKind == TypeKind::cNull || rightType->mKind == TypeKind::cNull))
        {
            ExpressionNode* interfaceExpr = leftType->mKind == TypeKind::cInterface ? node->mLeft : node->mRight;
            if (emitInterfaceObjectRef(interfaceExpr) == false)
            {
                return false;
            }

            emitIntegerImmediate(cNullRef);

            return emitReferenceEqualityOpCode(node->mOp);
        }
    }

    // Otherwise, do normal stuff.
    // Push lhs and rhs.
    if (visit(node->mLeft) == false || visit(node->mRight) == false)
    {
        return false;
    }

    switch (node->mOp)
    {
        case BinaryOp::cEQ:
        case BinaryOp::cNE:
        case BinaryOp::cLT:
        case BinaryOp::cLE:
        case BinaryOp::cGT:
        case BinaryOp::cGE:
        {
            Type* lhsType = node->mLeft->mResolvedType;
            Type* rhsType = node->mRight->mResolvedType;

            // Reference comparisons operate on raw reference values.
            // These compare reference words, so we use integer comparison.
            if (lhsType->mKind == TypeKind::cNull || rhsType->mKind == TypeKind::cNull ||
                lhsType->mKind == TypeKind::cClass || rhsType->mKind == TypeKind::cClass ||
                lhsType->mKind == TypeKind::cList || rhsType->mKind == TypeKind::cList ||
                lhsType->mKind == TypeKind::cMap || rhsType->mKind == TypeKind::cMap)
            {
                // Only == and != are allowed here (enforced in type checker).
                return emitReferenceEqualityOpCode(node->mOp);
            }

            // Here we are interested in the operand kind (the result is always bool).
            PrimitiveTypeKind operandKind = getPrimitiveKind(lhsType);

            return emitComparisonOpCode(node->mOp, operandKind);
        }
        default:
        {
            // Here, the result is the resolved type that we have on the binary node.
            PrimitiveTypeKind resultKind = getPrimitiveKind(node->mResolvedType);
            return emitArithmeticOrBitwiseOpcode(node->mOp, resultKind);
        }
    }
}

bool CodeGenVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    u32 trueLabel = makeLabel();
    u32 falseLabel = makeLabel();
    u32 endLabel = makeLabel();

    // Resolve the condition, leaving 0 or 1 on the stack.
    if (visitAsCondition(node->mCondition, trueLabel, falseLabel) == false)
    {
        return false;
    }

    // True label and expression.
    emit<OpCode::cLabel>(trueLabel);
    if (visit(node->mThenExpr) == false)
    {
        return false;
    }

    // Jump to the end label.
    emit<OpCode::cJump>(endLabel);

    // False label and expression.
    emit<OpCode::cLabel>(falseLabel);
    if (visit(node->mElseExpr) == false)
    {
        return false;
    }

    emit<OpCode::cLabel>(endLabel);

    return true;
}

} // namespace simlang
