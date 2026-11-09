#pragma once

#include "symbol/constvalue.h"

namespace simlang
{

class ArenaAllocator;
struct ExpressionNode;
struct Symbol;

enum class ConstEvalFailureKind : u8
{
    cNone,
    cDivisionByZero,
    cShiftCountOutOfRange,
    cInvalidFloatToIntCast
};

struct ConstEvalFailure
{
    ConstEvalFailureKind mKind = ConstEvalFailureKind::cNone;
    ExpressionNode* mExpression = nullptr;
    i32 mShiftCount = 0;
};

bool isValidConstDivisor(const ConstValue& value);
bool isValidConstShiftCount(i32 value);

class ConstEvaluator
{
public:
    explicit ConstEvaluator(ArenaAllocator& allocator);

    bool evaluate(ExpressionNode* node, ConstValue& outValue, ConstEvalFailure* failure = nullptr);
    bool evaluateSymbol(Symbol* symbol, ConstValue& outValue, ConstEvalFailure* failure = nullptr);

    ArenaAllocator& getAllocator() const { return mAllocator; }

private:
    ArenaAllocator& mAllocator;
};

} // namespace simlang
