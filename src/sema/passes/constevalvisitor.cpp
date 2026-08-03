#include "sema/passes/constevalvisitor.h"

#include <limits>
#include <string_view>
#include <vector>

#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/unaryop.h"
#include "symbol/internedstring.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arena.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/numeric.h"
#include "util/types.h"

namespace simlang
{

ConstEvalVisitor::ConstEvalVisitor(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
}

bool ConstEvalVisitor::evaluate(ExpressionNode* node, ConstValue& outValue)
{
    // Reset our state before we go, this visitor is reusable.
    mValue = ConstValue::makeInvalid();

    if (visit(node) == false)
    {
        outValue = ConstValue::makeInvalid();
        return false;
    }

    outValue = mValue;

    return true;
}

static bool applyConstCast(ConstValue& value, Type* targetType)
{
    // Allow cast to null for classes, lists, maps, and interfaces.
    if (value.mKind == ConstValueKind::cNull)
    {
        return targetType->mKind == TypeKind::cClass || targetType->mKind == TypeKind::cList ||
               targetType->mKind == TypeKind::cMap || targetType->mKind == TypeKind::cInterface;
    }

    // Otherwise, the type has to be a primitive.
    if (targetType->mKind != TypeKind::cPrimitive)
    {
        return false;
    }

    // The value also has to be a primitive, or the cast will fail.
    if (value.mKind != ConstValueKind::cPrimitive)
    {
        return false;
    }

    auto* primitiveTarget = static_cast<PrimitiveType*>(targetType);
    PrimitiveTypeKind targetKind = primitiveTarget->mPrimitiveKind;

    // If we already have the right type, we're done.
    if (targetKind == value.mPrimitiveKind)
    {
        return true;
    }

    // Otherwise, do int/float/bool casts.
    if (targetKind == PrimitiveTypeKind::cInt && value.mPrimitiveKind == PrimitiveTypeKind::cFloat)
    {
        i32 intValue = 0;
        if (checkedFloatToInt(value.as.mFloat, intValue) == false)
        {
            return false;
        }

        value = ConstValue::makeInteger(intValue);
    }
    else if (targetKind == PrimitiveTypeKind::cInt && value.mPrimitiveKind == PrimitiveTypeKind::cBool)
    {
        value = ConstValue::makeInteger(value.as.mBool ? 1 : 0);
    }
    else if (targetKind == PrimitiveTypeKind::cFloat && value.mPrimitiveKind == PrimitiveTypeKind::cInt)
    {
        value = ConstValue::makeFloat(static_cast<f32>(value.as.mInteger));
    }
    else if (targetKind == PrimitiveTypeKind::cFloat && value.mPrimitiveKind == PrimitiveTypeKind::cBool)
    {
        value = ConstValue::makeFloat(value.as.mBool ? 1.0f : 0.0f);
    }
    else if (targetKind == PrimitiveTypeKind::cBool && value.mPrimitiveKind == PrimitiveTypeKind::cInt)
    {
        value = ConstValue::makeBool(value.as.mInteger != 0);
    }
    else if (targetKind == PrimitiveTypeKind::cBool && value.mPrimitiveKind == PrimitiveTypeKind::cFloat)
    {
        value = ConstValue::makeBool(value.as.mFloat != 0.0f);
    }
    else
    {
        return false;
    }

    return true;
}

bool ConstEvalVisitor::visitImplicitCast(ImplicitCastNode* node)
{
    // Resolve the target type.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // Apply the cast.
    return applyConstCast(mValue, node->mResolvedType);
}

bool ConstEvalVisitor::visitCast(CastNode* node)
{
    // Resolve the target type.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // Apply the cast.
    return applyConstCast(mValue, node->mResolvedType);
}

bool ConstEvalVisitor::visitIdentifier(IdentifierNode* node)
{
    Symbol* s = node->mSymbol;

    // Only immutable variables (val) can be constexpr.
    if (s->mFlags.test(SymbolFlags::cMutable))
    {
        return false;
    }

    // Only symbols explicitly marked constexpr participate in constexpr evaluation.
    // (TypeCheckVisitor already sets SymbolFlags::cConstExpr when the initializer is constexpr.)
    if (s->mFlags.test(SymbolFlags::cConstExpr) == false)
    {
        return false;
    }

    // IF we already evaluated this, don't go again.f
    switch (s->mConstEvalState)
    {
        case ConstEvalState::cReady:
        {
            mValue = s->mConstValue;
            return true;
        }
        case ConstEvalState::cProcessing:
        {
            return false;
        }
        case ConstEvalState::cFailed:
        {
            return false;
        }
        case ConstEvalState::cUnprocessed:
        default:
        {
            break;
        }
    }

    // If we get here, we need to compute stuff.
    s->mConstEvalState = ConstEvalState::cProcessing;

    auto* decl = static_cast<VariableDeclarationStatementNode*>(s->mDeclNode);
    if (visit(decl->mInit) == false)
    {
        s->mConstEvalState = ConstEvalState::cFailed;
        return false;
    }

    s->mConstValue = mValue;
    s->mConstEvalState = ConstEvalState::cReady;

    return true;
}

bool ConstEvalVisitor::visitThis(ThisNode*)
{
    return false;
}

bool ConstEvalVisitor::visitIntLiteral(IntLiteralNode* node)
{
    mValue = ConstValue::makeInteger(node->mInt);
    return true;
}

bool ConstEvalVisitor::visitFloatLiteral(FloatLiteralNode* node)
{
    mValue = ConstValue::makeFloat(node->mFloat);
    return true;
}

bool ConstEvalVisitor::visitBoolLiteral(BoolLiteralNode* node)
{
    mValue = ConstValue::makeBool(node->mBool);
    return true;
}

bool ConstEvalVisitor::visitStringLiteral(StringLiteralNode* node)
{
    mValue = ConstValue::makeString(node->mString);
    return true;
}

bool ConstEvalVisitor::visitNullLiteral(NullLiteralNode*)
{
    mValue = ConstValue::makeNull();
    return true;
}

bool ConstEvalVisitor::visitFormatString(FormatStringNode*)
{
    return false;
}

bool ConstEvalVisitor::visitNewObject(NewObjectNode* node)
{
    // Only structs can be consteval.
    if (node->mResolvedType->mKind != TypeKind::cStruct)
    {
        return false;
    }

    auto* structType = static_cast<AggregateType*>(node->mResolvedType);

    std::vector<ConstFieldValue> fields;
    fields.reserve(structType->mSymbol->mMembers.size());

    for (Symbol* member : structType->mSymbol->mMembers)
    {
        if (member->mSymbolType != SymbolType::cMemberVariable)
        {
            continue;
        }

        // Go through all field initializers to look up our current member.
        ExpressionNode* valueExpr = nullptr;
        for (FieldInitializer* init : node->mFieldInitializers)
        {
            if (init->mResolvedField == member)
            {
                valueExpr = init->mValue;
                break;
            }
        }

        // If we have no match, use the initializer from the declaration.
        if (valueExpr == nullptr)
        {
            auto* decl = static_cast<VariableDeclarationStatementNode*>(member->mDeclNode);
            valueExpr = decl->mInit;
        }

        // See if we can evaluate whatever we have now.
        ConstValue fieldValue;
        ConstEvalVisitor fieldEvaluator{mAllocator};
        if (fieldEvaluator.evaluate(valueExpr, fieldValue) == false)
        {
            // If not, the whole thing isn't consteval.
            return false;
        }

        ConstValue* storedValue = mAllocator.create<ConstValue>(fieldValue);
        fields.push_back(ConstFieldValue{member, storedValue});
    }

    // If we collected all of them, make the struct based on the values that we got.
    auto* structValue = mAllocator.create<ConstStructValue>();
    structValue->mType = structType;
    structValue->mFields = makeArrayView(mAllocator, fields);

    mValue = ConstValue::makeStruct(structValue);
    return true;
}

bool ConstEvalVisitor::visitFunctionCall(FunctionCallNode*)
{
    // Function calls are currently never consteval.
    return false;
}

bool ConstEvalVisitor::visitIndexCall(IndexCallNode*)
{
    return false;
}

bool ConstEvalVisitor::visitMemberAccess(MemberAccessNode* node)
{
    if (node->mSymbol->mSymbolType != SymbolType::cMemberVariable)
    {
        return false;
    }

    // Const eval member access can only be done on structs.
    ConstValue receiverValue;
    ConstEvalVisitor receiverEvaluator{mAllocator};
    if (receiverEvaluator.evaluate(node->mReceiver, receiverValue) == false ||
        receiverValue.mKind != ConstValueKind::cStruct)
    {
        return false;
    }

    // Find the field.
    for (const ConstFieldValue& constField : receiverValue.as.mStruct->mFields)
    {
        if (constField.mField == node->mSymbol)
        {
            mValue = *constField.mValue;
            return true;
        }
    }

    return false;
}

bool ConstEvalVisitor::visitModuleAccess(ModuleAccessNode* node)
{
    return visit(node->mRight);
}

bool ConstEvalVisitor::visitUnaryOp(UnaryOpNode* node)
{
    if (visit(node->mExpr) == false)
    {
        return false;
    }

    switch (node->mOp)
    {
        case UnaryOp::cNeg:
        {
            if (mValue.mPrimitiveKind == PrimitiveTypeKind::cInt)
            {
                // Go through u32 to handle wrapping.
                // That is the case if mInteger is cMinInt.
                u32 value = static_cast<u32>(mValue.as.mInteger);
                mValue = ConstValue::makeInteger(static_cast<i32>(0U - value));
                return true;
            }
            else if (mValue.mPrimitiveKind == PrimitiveTypeKind::cFloat)
            {
                mValue = ConstValue::makeFloat(-mValue.as.mFloat);
                return true;
            }

            break;
        }
        case UnaryOp::cBitNot:
        {
            if (mValue.mPrimitiveKind == PrimitiveTypeKind::cInt)
            {
                // Cast to unsigned before applying ~, and then back.
                mValue = ConstValue::makeInteger(static_cast<i32>(~static_cast<u32>(mValue.as.mInteger)));
                return true;
            }

            break;
        }
        default:
        {
            break;
        }
    }

    return false;
}

bool ConstEvalVisitor::visitBinaryOp(BinaryOpNode* node)
{
    ConstValue l;
    ConstValue r;

    ConstEvalVisitor lv{mAllocator};
    ConstEvalVisitor rv{mAllocator};

    if (lv.evaluate(node->mLeft, l) == false || rv.evaluate(node->mRight, r) == false)
    {
        return false;
    }

    // If we have one of the bool ops for non-primitives, handle that.
    if (node->mOp == BinaryOp::cEQ || node->mOp == BinaryOp::cNE)
    {
        if (l.mKind == ConstValueKind::cStruct || r.mKind == ConstValueKind::cStruct ||
            l.mKind == ConstValueKind::cNull || r.mKind == ConstValueKind::cNull)
        {
            bool equal = (l == r);
            mValue = ConstValue::makeBool((node->mOp == BinaryOp::cEQ) ? equal : (equal == false));
            return true;
        }
    }

    if (l.mKind != ConstValueKind::cPrimitive || r.mKind != ConstValueKind::cPrimitive)
    {
        return false;
    }

    PrimitiveTypeKind kind = l.mPrimitiveKind;

    switch (node->mOp)
    {
        case BinaryOp::cAdd:
        case BinaryOp::cSub:
        case BinaryOp::cMul:
        case BinaryOp::cDiv:
        case BinaryOp::cMod:
        {
            if (kind == PrimitiveTypeKind::cInt)
            {
                // Do the integer operations in unsigned to handle over/underflow correctly.
                u32 lhs = static_cast<u32>(l.as.mInteger);
                u32 rhs = static_cast<u32>(r.as.mInteger);
                i32 result = 0;

                switch (node->mOp)
                {
                    case BinaryOp::cAdd: result = static_cast<i32>(lhs + rhs); break;
                    case BinaryOp::cSub: result = static_cast<i32>(lhs - rhs); break;
                    case BinaryOp::cMul: result = static_cast<i32>(lhs * rhs); break;
                    case BinaryOp::cDiv:
                    {
                        if (r.as.mInteger == 0)
                        {
                            return false;
                        }
                        else if (l.as.mInteger == std::numeric_limits<i32>::min() && r.as.mInteger == -1)
                        {
                            result = std::numeric_limits<i32>::min();
                        }
                        else
                        {
                            result = l.as.mInteger / r.as.mInteger;
                        }
                        break;
                    }
                    case BinaryOp::cMod:
                    {
                        if (r.as.mInteger == 0)
                        {
                            return false;
                        }
                        else if (l.as.mInteger == std::numeric_limits<i32>::min() && r.as.mInteger == -1)
                        {
                            result = 0;
                        }
                        else
                        {
                            result = l.as.mInteger % r.as.mInteger;
                        }
                        break;
                    }
                    default:
                    {
                        return false;
                    }
                }

                mValue = ConstValue::makeInteger(result);

                return true;
            }
            if (kind == PrimitiveTypeKind::cFloat)
            {
                f32 result = 0.0f;

                switch (node->mOp)
                {
                    case BinaryOp::cAdd: result = l.as.mFloat + r.as.mFloat; break;
                    case BinaryOp::cSub: result = l.as.mFloat - r.as.mFloat; break;
                    case BinaryOp::cMul: result = l.as.mFloat * r.as.mFloat; break;
                    case BinaryOp::cDiv:
                    {
                        if (r.as.mFloat == 0.0f)
                        {
                            return false;
                        }
                        else
                        {
                            result = l.as.mFloat / r.as.mFloat;
                        }
                        break;
                    }
                    case BinaryOp::cMod:
                    {
                        if (r.as.mFloat == 0.0f)
                        {
                            return false;
                        }
                        else
                        {
                            // Does it really make sense to even provide % for floats?
                            // q = l / r
                            f32 q = l.as.mFloat / r.as.mFloat;
                            // n = floor(q)
                            i32 n = static_cast<i32>(q);
                            // l % r = l - (n * r)
                            result = l.as.mFloat - (static_cast<f32>(n) * r.as.mFloat);
                        }
                        break;
                    }
                    default:
                    {
                        return false;
                    }
                }

                mValue = ConstValue::makeFloat(result);

                return true;
            }
            break;
        }
        case BinaryOp::cBitOr:
        case BinaryOp::cBitAnd:
        case BinaryOp::cBitXor:
        case BinaryOp::cShiftL:
        case BinaryOp::cShiftR:
        {
            i32 result = 0;

            switch (node->mOp)
            {
                case BinaryOp::cBitOr: result = l.as.mInteger | r.as.mInteger; break;
                case BinaryOp::cBitAnd: result = l.as.mInteger & r.as.mInteger; break;
                case BinaryOp::cBitXor: result = l.as.mInteger ^ r.as.mInteger; break;
                case BinaryOp::cShiftL:
                {
                    if (r.as.mInteger < 0 || r.as.mInteger >= 32)
                    {
                        return false;
                    }

                    result = static_cast<i32>(static_cast<u32>(l.as.mInteger) << r.as.mInteger);
                    break;
                }
                case BinaryOp::cShiftR:
                {
                    if (r.as.mInteger < 0 || r.as.mInteger >= 32)
                    {
                        return false;
                    }

                    result = static_cast<i32>(static_cast<u32>(l.as.mInteger) >> r.as.mInteger);
                    break;
                }
                default: return false;
            }

            mValue = ConstValue::makeInteger(result);

            return true;
        }
        case BinaryOp::cEQ:
        case BinaryOp::cNE:
        case BinaryOp::cLT:
        case BinaryOp::cLE:
        case BinaryOp::cGT:
        case BinaryOp::cGE:
        {
            bool result = false;

            if (kind == PrimitiveTypeKind::cInt)
            {
                switch (node->mOp)
                {
                    case BinaryOp::cEQ: result = (l.as.mInteger == r.as.mInteger); break;
                    case BinaryOp::cNE: result = (l.as.mInteger != r.as.mInteger); break;
                    case BinaryOp::cLT: result = (l.as.mInteger < r.as.mInteger); break;
                    case BinaryOp::cLE: result = (l.as.mInteger <= r.as.mInteger); break;
                    case BinaryOp::cGT: result = (l.as.mInteger > r.as.mInteger); break;
                    case BinaryOp::cGE: result = (l.as.mInteger >= r.as.mInteger); break;
                    default: return false;
                }
            }
            else if (kind == PrimitiveTypeKind::cFloat)
            {
                switch (node->mOp)
                {
                    case BinaryOp::cEQ: result = (l.as.mFloat == r.as.mFloat); break;
                    case BinaryOp::cNE: result = (l.as.mFloat != r.as.mFloat); break;
                    case BinaryOp::cLT: result = (l.as.mFloat < r.as.mFloat); break;
                    case BinaryOp::cLE: result = (l.as.mFloat <= r.as.mFloat); break;
                    case BinaryOp::cGT: result = (l.as.mFloat > r.as.mFloat); break;
                    case BinaryOp::cGE: result = (l.as.mFloat >= r.as.mFloat); break;
                    default: return false;
                }
            }
            else if (kind == PrimitiveTypeKind::cBool)
            {
                switch (node->mOp)
                {
                    case BinaryOp::cEQ: result = (l.as.mBool == r.as.mBool); break;
                    case BinaryOp::cNE: result = (l.as.mBool != r.as.mBool); break;
                    default: return false;
                }
            }
            else if (kind == PrimitiveTypeKind::cString)
            {
                bool equal = l.as.mString->toView() == r.as.mString->toView();
                switch (node->mOp)
                {
                    case BinaryOp::cEQ: result = equal; break;
                    case BinaryOp::cNE: result = !equal; break;
                    default: return false;
                }
            }
            else
            {
                return false;
            }

            mValue = ConstValue::makeBool(result);

            return true;
        }
        case BinaryOp::cOr:
        case BinaryOp::cAnd:
        {
            bool result = false;

            switch (node->mOp)
            {
                case BinaryOp::cOr: result = (l.as.mBool || r.as.mBool); break;
                case BinaryOp::cAnd: result = (l.as.mBool && r.as.mBool); break;
                default: return false;
            }

            mValue = ConstValue::makeBool(result);

            return true;
        }
        default:
        {
            break;
        }
    }

    return false;
}

bool ConstEvalVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    // Try to evaluate the condition.
    ConstValue conditionValue;
    ConstEvalVisitor conditionVisitor{mAllocator};
    if (conditionVisitor.evaluate(node->mCondition, conditionValue) == false ||
        conditionValue.mPrimitiveKind != PrimitiveTypeKind::cBool)
    {
        return false;
    }

    // Find out which expression was selected.
    ExpressionNode* selectedExpr = conditionValue.as.mBool ? node->mThenExpr : node->mElseExpr;

    // Evaluate that one.
    ConstValue selectedValue;
    ConstEvalVisitor branchVisitor{mAllocator};
    if (branchVisitor.evaluate(selectedExpr, selectedValue) == false)
    {
        return false;
    }

    // Set the value.
    mValue = selectedValue;

    return true;
}

} // namespace simlang
