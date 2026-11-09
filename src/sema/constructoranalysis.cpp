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

bool ConstructorAnalyzer::isConstEvaluableType(Type* type)
{
    // Primitives are always const-evaluable, and reference types can have constant null values.
    if (type->mKind == TypeKind::cPrimitive || type->mKind == TypeKind::cClass || type->mKind == TypeKind::cList ||
        type->mKind == TypeKind::cMap || type->mKind == TypeKind::cInterface)
    {
        return true;
    }

    // Other types must be recursively eligible structs.
    if (type->mKind != TypeKind::cStruct)
    {
        return false;
    }

    // We're currently evaluating this struct, so mark it as false.
    auto [it, inserted] = mTypes.emplace(type, false);
    // If it was already in the map and was resolved at this point, we're good.
    // If it was already in the map and wasn't resolved, we have a recursive struct, which is not allowed.
    if (inserted == false)
    {
        return it->second;
    }

    // Go over all the members and check them.
    auto* aggregate = static_cast<AggregateType*>(type);
    for (Symbol* member : aggregate->mSymbol->mMembers)
    {
        if (member->mSymbolType == SymbolType::cMemberVariable && isConstEvaluableType(member->mType) == false)
        {
            return false;
        }
    }

    // If we didn't bail earlier, mark the type as eligible.
    mTypes[type] = true;

    return true;
}

bool ConstructorAnalyzer::isConstEvaluableExpression(ExpressionNode* node)
{
    // Check whether the expression we're getting is const-evaluable.
    switch (node->mNodeType)
    {
        case NodeType::cIntLiteral:
        case NodeType::cFloatLiteral:
        case NodeType::cBoolLiteral:
        case NodeType::cStringLiteral:
        case NodeType::cNullLiteral:
        {
            // Literals are always eligible.
            return true;
        }
        case NodeType::cIdentifier:
        {
            // Parameters (always const), or globals/locals that are immutable are eligible.
            Symbol* symbol = static_cast<IdentifierNode*>(node)->mSymbol;
            return symbol->mSymbolType == SymbolType::cParameter ||
                   ((symbol->mSymbolType == SymbolType::cGlobalVariable ||
                     symbol->mSymbolType == SymbolType::cStackVariable) &&
                    symbol->mFlags.test(SymbolFlags::cMutable) == false);
        }
        case NodeType::cModuleAccess:
        {
            // For module access, check the rhs for eligibility.
            return isConstEvaluableExpression(static_cast<ModuleAccessNode*>(node)->mRight);
        }
        case NodeType::cMemberAccess:
        {
            // Struct members may be eligible.
            auto* member = static_cast<MemberAccessNode*>(node);
            return member->mReceiver->mResolvedType->mKind == TypeKind::cStruct &&
                   isConstEvaluableExpression(member->mReceiver);
        }
        case NodeType::cNewObject:
        {
            auto* construction = static_cast<NewObjectNode*>(node);
            if (construction->mResolvedType->mKind != TypeKind::cStruct)
            {
                return false;
            }
            for (const CallArgument& argument : construction->mArgs)
            {
                if (isConstEvaluableExpression(argument.mValue) == false)
                {
                    return false;
                }
            }

            auto* type = static_cast<AggregateType*>(construction->mResolvedType);
            return analyze(type, construction->mConstructorSymbol)->mIsConstEvaluable;
        }
        case NodeType::cImplicitCast:
        case NodeType::cCast:
        {
            // Get the target expr to cast.
            ExpressionNode* target = node->mNodeType == NodeType::cImplicitCast
                                         ? static_cast<ImplicitCastNode*>(node)->mTarget
                                         : static_cast<CastNode*>(node)->mTarget;
            // The evaluator applies the type-checked conversion to each call's value.
            return isConstEvaluableExpression(target);
        }
        case NodeType::cUnaryOp:
        {
            // Built-in unary operators use the evaluator's semantics.
            auto* unary = static_cast<UnaryOpNode*>(node);
            return isConstEvaluableExpression(unary->mExpr);
        }
        case NodeType::cBinaryOp:
        {
            // Checked operations may succeed or fail for different arguments to the same constructor.
            auto* binary = static_cast<BinaryOpNode*>(node);
            return isConstEvaluableExpression(binary->mLeft) && isConstEvaluableExpression(binary->mRight);
        }
        default:
        {
            return false;
        }
    }
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

    // Parameters and fields may contain primitives, null references, and nested value structs.
    // Type composition is independent of the constructors used to create those values.
    if (isConstEvaluable)
    {
        for (const FunctionParam& param : functionType->mParamTypes)
        {
            if (param.mIsInOut || isConstEvaluableType(param.mType) == false)
            {
                // If any of them fail, the constructor is not const evaluable.
                isConstEvaluable = false;
                break;
            }
        }
    }

    // Eligible constructors initialize every field in the proper order.
    // Match the field order once, then check triviality and constexpr eligibility independently.
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
                isTrivial = evaluator.evaluate(assignment->mRHS, value) &&
                            (value.mKind == ConstValueKind::cPrimitive || value.mKind == ConstValueKind::cStruct ||
                             value.mKind == ConstValueKind::cNull);
                if (isTrivial)
                {
                    fields[i].mConstantValue = mAllocator.create<ConstValue>(value);
                }
            }
        }

        if (isConstEvaluable)
        {
            // If we're still const evaluable, check that the field type and expression are const evaluable as well.
            isConstEvaluable =
                isConstEvaluableType(fields[i].mField->mType) && isConstEvaluableExpression(assignment->mRHS);
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
