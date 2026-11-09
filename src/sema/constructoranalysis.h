#pragma once

#include "util/arrayview.h"

namespace simlang
{

class ArenaAllocator;
struct AggregateType;
struct ConstValue;
struct ExpressionNode;
struct Symbol;

struct FieldInitialization
{
    Symbol* mField = nullptr;
    ExpressionNode* mExpression = nullptr;
    const ConstValue* mConstantValue = nullptr;
    usize mParameterIndex = 0;
};

struct ConstructorAnalysis
{
    ArrayView<FieldInitialization> mFields;
    bool mIsConstEvaluable = false;
};

class ConstructorAnalyzer
{
public:
    explicit ConstructorAnalyzer(ArenaAllocator& allocator);

    const ConstructorAnalysis* analyze(AggregateType* type, Symbol* constructor);

private:
    ArenaAllocator& mAllocator;
};

} // namespace simlang
