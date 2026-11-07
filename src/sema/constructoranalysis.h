#pragma once

#include <unordered_map>

#include "util/arrayview.h"

namespace simlang
{

class ArenaAllocator;
struct AggregateType;
struct ExpressionNode;
struct Symbol;
struct Type;

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

class ConstructorAnalyzer
{
public:
    explicit ConstructorAnalyzer(ArenaAllocator& allocator);

    const ConstructorAnalysis* analyze(AggregateType* type, Symbol* constructor);

private:
    bool isConstEvaluableType(Type* type);
    bool isConstEvaluableExpression(ExpressionNode* node);

    ArenaAllocator& mAllocator;
    std::unordered_map<Type*, bool> mTypes;
};

} // namespace simlang
