#pragma once

#include "util/arrayview.h"

namespace simlang
{

class ArenaAllocator;
struct AggregateType;
struct ExpressionNode;
struct Symbol;

struct FieldInitialization
{
    Symbol* mField = nullptr;
    ExpressionNode* mExpression = nullptr;
};

struct ConstructorAnalysis
{
    ArrayView<FieldInitialization> mFields;
    bool mIsConstEvaluable = false;
};

// Analyze a type-checked struct constructor and cache the result on its declaration.
const ConstructorAnalysis* getConstructorAnalysis(ArenaAllocator& allocator, AggregateType* type, Symbol* constructor);

} // namespace simlang
