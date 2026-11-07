#include "sema/passes/constevalvisitor.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/unaryop.h"
#include "sema/constructoranalysis.h"
#include "symbol/internedstring.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arena.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/numeric.h"
#include "util/scoping.h"
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

static bool applyConstCast(ConstValue& value, Type* sourceType, Type* targetType)
{
    // Identity casts preserve the value, including nested structs. Valid reference casts preserve null.
    if (sourceType == targetType || value.mKind == ConstValueKind::cNull)
    {
        return true;
    }

    // Type checking leaves only conversions between int, float, and bool.
    auto* primitiveTarget = static_cast<PrimitiveType*>(targetType);
    PrimitiveTypeKind targetKind = primitiveTarget->mPrimitiveKind;
    if (targetKind == PrimitiveTypeKind::cInt)
    {
        if (value.mPrimitiveKind == PrimitiveTypeKind::cFloat)
        {
            i32 intValue = 0;
            if (checkedFloatToInt(value.as.mFloat, intValue) == false)
            {
                return false;
            }
            value = ConstValue::makeInteger(intValue);
        }
        else
        {
            value = ConstValue::makeInteger(value.as.mBool ? 1 : 0);
        }
    }
    else if (targetKind == PrimitiveTypeKind::cFloat)
    {
        value =
            ConstValue::makeFloat(value.mPrimitiveKind == PrimitiveTypeKind::cInt ? static_cast<f32>(value.as.mInteger)
                                                                                  : (value.as.mBool ? 1.0f : 0.0f));
    }
    else
    {
        value = ConstValue::makeBool(value.mPrimitiveKind == PrimitiveTypeKind::cInt ? value.as.mInteger != 0
                                                                                     : value.as.mFloat != 0.0f);
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
    return applyConstCast(mValue, node->mTarget->mResolvedType, node->mResolvedType);
}

bool ConstEvalVisitor::visitCast(CastNode* node)
{
    // Resolve the target type.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // Apply the cast.
    return applyConstCast(mValue, node->mTarget->mResolvedType, node->mResolvedType);
}

bool ConstEvalVisitor::visitIdentifier(IdentifierNode* node)
{
    Symbol* s = node->mSymbol;

    // Check if this is a parameter, in which case we might be evaluating a call.
    if (s->mSymbolType == SymbolType::cParameter)
    {
        // If we are, try to look up the value since it might be const eval (e.g. new Vector(0.0, 0.0, 0.0);).
        if (mFrame != nullptr)
        {
            // This is currently only supported for constructors.
            for (usize i = 0; i < mFrame->mConstructor->mParams.size(); ++i)
            {
                auto* param = static_cast<ParamDeclarationNode*>(mFrame->mConstructor->mParams[i]);
                if (param->mSymbol == s)
                {
                    mValue = mFrame->mArguments[i];
                    return true;
                }
            }
        }

        return false;
    }

    // If we have a function or a syscall, we can directly handle that (sine they're immutable anyway).
    if (s->mSymbolType == SymbolType::cFunction || s->mSymbolType == SymbolType::cSyscall)
    {
        mValue = ConstValue::makeFunction(s);
        return true;
    }

    // Only immutable variables (const) can be constexpr.
    if (s->mFlags.test(SymbolFlags::cMutable))
    {
        return false;
    }

    // Params need to go through the call bindings, fields through their receiver.
    // That means we only accept globals and locals here.
    if (s->mSymbolType != SymbolType::cGlobalVariable && s->mSymbolType != SymbolType::cStackVariable)
    {
        return false;
    }

    // If we already evaluated this, don't go again.
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
    // It's possible for mInit to be null if we have a multi-variable declaration.
    // We do not currently allow this to be const evaluable, and so we need to make sure that is correct.
    // (visit(nullptr) returns true, so visit(decl->mInit) is not enough.)
    if (decl->mInit == nullptr || visit(decl->mInit) == false)
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

bool ConstEvalVisitor::visitLambda(LambdaNode* node)
{
    // Lambdas with captures are... not const evaluable.
    if (node->mCaptures.empty() == false)
    {
        return false;
    }

    mValue = ConstValue::makeFunction(node->mSymbol);
    return true;
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
    // Only struct constructors are currently const eval.
    if (node->mResolvedType->mKind != TypeKind::cStruct)
    {
        return false;
    }

    // Get the constructor analysis.
    auto* type = static_cast<AggregateType*>(node->mResolvedType);
    ConstructorAnalyzer analyzer{mAllocator};
    const ConstructorAnalysis* analysis = analyzer.analyze(type, node->mConstructorSymbol);

    // If it isn't const-evaluable, bail.
    if (analysis->mIsConstEvaluable == false)
    {
        return false;
    }

    // Evaluate every argument in the caller's frame, including unused ones.
    std::vector<ConstValue> arguments(node->mArgs.size());
    bool argumentsOk = true;
    for (usize i = 0; i < node->mArgs.size(); ++i)
    {
        if (evaluate(node->mArgs[i].mValue, arguments[i]) == false)
        {
            argumentsOk = false;
        }
    }

    // If any argument isn't const-evaluable, bail.
    if (argumentsOk == false)
    {
        return false;
    }

    // If they all are, we prepare for the constructor call.
    auto* declaration = static_cast<FunctionDeclarationStatementNode*>(node->mConstructorSymbol->mDeclNode);

    // Store the constructor declaration and its arguments and scope it.
    CallFrame frame{
        declaration,
        {arguments.data(), arguments.size()}
    };
    ScopedValueBinder<const CallFrame*> frameScope{mFrame, &frame};

    // Now, evaluate all the fields of the struct.
    // For this, we take the expressions for the fields from the analayis and evaluate them.
    auto* fields = mAllocator.createArray<ConstFieldValue>(analysis->mFields.size());
    for (usize i = 0; i < analysis->mFields.size(); ++i)
    {
        const FieldInitialization& field = analysis->mFields[i];
        auto* value = mAllocator.create<ConstValue>();
        if (evaluate(field.mExpression, *value) == false)
        {
            return false;
        }
        fields[i] = {field.mField, value};
    }

    // Then, create the const value for the struct and store it.
    auto* result = mAllocator.create<ConstStructValue>();
    result->mType = type;
    result->mFields = {fields, analysis->mFields.size()};
    mValue = ConstValue::makeStruct(result);

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
    // Const eval member access can only be done on structs.
    if (node->mReceiver->mResolvedType->mKind != TypeKind::cStruct)
    {
        return false;
    }

    ConstValue receiverValue;
    if (evaluate(node->mReceiver, receiverValue) == false)
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

    if (evaluate(node->mLeft, l) == false || evaluate(node->mRight, r) == false)
    {
        return false;
    }

    // If we have one of the bool ops for non-primitives, handle that.
    if (node->mOp == BinaryOp::cEQ || node->mOp == BinaryOp::cNE)
    {
        if (l.mKind == ConstValueKind::cStruct || r.mKind == ConstValueKind::cStruct ||
            l.mKind == ConstValueKind::cFunction || r.mKind == ConstValueKind::cFunction ||
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
    if (evaluate(node->mCondition, conditionValue) == false ||
        conditionValue.mPrimitiveKind != PrimitiveTypeKind::cBool)
    {
        return false;
    }

    // Find out which expression was selected.
    ExpressionNode* selectedExpr = conditionValue.as.mBool ? node->mThenExpr : node->mElseExpr;

    // Evaluate that one.
    ConstValue selectedValue;
    if (evaluate(selectedExpr, selectedValue) == false)
    {
        return false;
    }

    // Set the value.
    mValue = selectedValue;

    return true;
}

} // namespace simlang
