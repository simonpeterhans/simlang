#include "sema/constructoranalysis.h"

#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "sema/passes/constevalvisitor.h"
#include "symbol/symbol.h"
#include "type/types.h"
#include "util/arenautils.h"

namespace simlang
{

ConstructorAnalyzer::ConstructorAnalyzer(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
}

const ConstructorAnalysis* ConstructorAnalyzer::analyze(AggregateType* type, Symbol* constructor)
{
    auto* declaration = static_cast<FunctionDeclarationStatementNode*>(constructor->mDeclNode);

    // If we already computed the analysis for this function, we're done.
    if (declaration->mConstructorAnalysis != nullptr)
    {
        return declaration->mConstructorAnalysis;
    }

    // Otherwise, do the analysis.
    auto* result = mAllocator.create<ConstructorAnalysis>();
    declaration->mConstructorAnalysis = result;

    // Collect all stored fields.
    std::vector<FieldInitialization> fields;
    for (Symbol* member : type->mSymbol->mMembers)
    {
        if (member->mSymbolType == SymbolType::cMemberVariable)
        {
            fields.push_back({member, nullptr});
        }
    }

    // Look at the constructor body, if there's an explicit return statement, remove that.
    auto* body = static_cast<BlockStatementNode*>(declaration->mBody);
    usize statementCount = body->mStatements.size();
    if (statementCount > 0 && body->mStatements.back()->mNodeType == NodeType::cReturnStatement)
    {
        // A return expression can have side effects, so only a void return is allowed.
        auto* returnStatement = static_cast<ReturnStatementNode*>(body->mStatements.back());
        if (returnStatement->mExpressions.empty() == false)
        {
            return result;
        }

        --statementCount;
    }

    // We expect exactly one statement for each member variable in the constructor.
    if (statementCount != fields.size())
    {
        return result;
    }

    auto* functionType = static_cast<FunctionType*>(constructor->mType);
    // Only structs are const evaluable.
    bool isConstEvaluable = type->mKind == TypeKind::cStruct;
    bool isTrivial = true;
    usize parameterIndex = 0;

    // Constant evaluation does not support borrowed arguments.
    if (isConstEvaluable)
    {
        for (const FunctionParam& param : functionType->mParamTypes)
        {
            if (param.mIsInOut)
            {
                // Borrowed parameters cannot be bound to constant values.
                isConstEvaluable = false;
                break;
            }
        }
    }

    // Eligible constructors initialize every field in the proper order.
    // Match the field order once, then retain the assignments for per-call constant evaluation.
    for (usize i = 0; i < fields.size(); ++i)
    {
        auto* assignment = static_cast<AssignmentStatementNode*>(body->mStatements[i]);
        auto* member = static_cast<MemberAccessNode*>(assignment->mLHS);
        if (member->mSymbol != fields[i].mField)
        {
            // If the field order does not match, the constructor is not eligible (for now).
            return result;
        }

        // Set the expression so that we can use it later.
        fields[i].mExpression = assignment->mRHS;

        // Trivial constructors forward each parameter once in order, with constant fields in between.
        if (isTrivial)
        {
            bool forwardsParameter = false;
            if (parameterIndex < declaration->mParams.size() && assignment->mRHS->mNodeType == NodeType::cIdentifier)
            {
                const FunctionParam& paramType = functionType->mParamTypes[parameterIndex];
                auto* parameter = static_cast<ParamDeclarationNode*>(declaration->mParams[parameterIndex]);
                auto* value = static_cast<IdentifierNode*>(assignment->mRHS);
                // inout is not allowed, the types must match, and the parameter and value must match.
                forwardsParameter = paramType.mIsInOut == false && paramType.mType == fields[i].mField->mType &&
                                    value->mSymbol == parameter->mSymbol;
            }

            if (forwardsParameter)
            {
                fields[i].mParameterIndex = parameterIndex++;
            }
            else
            {
                // Evaluate without parameter bindings so that the constant is independent of each call.
                ConstValue value;
                ConstEvalVisitor evaluator{mAllocator};
                isTrivial = evaluator.evaluate(assignment->mRHS, value);
                if (isTrivial)
                {
                    fields[i].mConstantValue = mAllocator.create<ConstValue>(value);
                }
            }
        }
    }

    // Set our stuff.
    declaration->mIsTrivialConstructor = isTrivial && parameterIndex == declaration->mParams.size();
    if (declaration->mIsTrivialConstructor || isConstEvaluable)
    {
        result->mFields = makeArrayView(mAllocator, fields);
    }
    result->mIsConstEvaluable = isConstEvaluable;

    return result;
}

} // namespace simlang
