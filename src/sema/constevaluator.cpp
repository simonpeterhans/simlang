#include "sema/constevaluator.h"

#include "sema/passes/constevalvisitor.h"

namespace simlang
{

ConstEvaluator::ConstEvaluator(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
}

bool ConstEvaluator::evaluate(ExpressionNode* node, ConstValue& outValue, ConstEvalFailure* failure)
{
    ConstEvalVisitor visitor{*this};
    return visitor.evaluate(node, outValue, failure);
}

bool ConstEvaluator::evaluateSymbol(Symbol* symbol, ConstValue& outValue, ConstEvalFailure* failure)
{
    ConstEvalVisitor visitor{*this};
    return visitor.evaluateSymbol(symbol, outValue, failure);
}

bool isValidConstDivisor(const ConstValue& value)
{
    return (value.mPrimitiveKind != PrimitiveTypeKind::cInt || value.as.mInteger != 0) &&
           (value.mPrimitiveKind != PrimitiveTypeKind::cFloat || value.as.mFloat != 0.0f);
}

bool isValidConstShiftCount(i32 value)
{
    return value >= 0 && value < 32;
}

} // namespace simlang
