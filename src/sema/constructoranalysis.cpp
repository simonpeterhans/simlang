#include "sema/constructoranalysis.h"

#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
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
    // Primitives are always const-evaluable.
    if (type->mKind == TypeKind::cPrimitive)
    {
        return true;
    }

    // Non-struct types (that are not primitives) are never const-evaluable.
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

    // Parameters and fields may contain primitives (including strings) and nested value structs.
    // Type composition is independent of the constructors used to create those values.
    auto* functionType = static_cast<FunctionType*>(constructor->mType);
    for (const FunctionParam& param : functionType->mParamTypes)
    {
        if (param.mIsInOut || isConstEvaluableType(param.mType) == false)
        {
            return result;
        }
    }

    // Otherwise, check the fields.
    std::vector<FieldInitialization> fields;
    for (Symbol* member : type->mSymbol->mMembers)
    {
        if (member->mSymbolType == SymbolType::cMemberVariable)
        {
            if (isConstEvaluableType(member->mType) == false)
            {
                return result;
            }
            fields.push_back({member, nullptr});
        }
    }

    // Look at the constructor body, if there's an explicit return statement, remove that.
    auto* body = static_cast<BlockStatementNode*>(declaration->mBody);
    usize statementCount = body->mStatements.size();
    if (body->mStatements.back()->mNodeType == NodeType::cReturnStatement)
    {
        --statementCount;
    }

    // We expect exactly one statement for each member variable in the constructor.
    if (statementCount != fields.size())
    {
        return result;
    }

    // Then, go over all the fields and make sure it's an assignment statement to the right member.
    // Additionally, we expect the rhs to be const-evaluable.
    for (usize i = 0; i < fields.size(); ++i)
    {
        auto* assignment = static_cast<AssignmentStatementNode*>(body->mStatements[i]);
        auto* member = static_cast<MemberAccessNode*>(assignment->mLHS);
        if (member->mSymbol != fields[i].mField || isConstEvaluableExpression(assignment->mRHS) == false)
        {
            return result;
        }
        fields[i].mExpression = assignment->mRHS;
    }

    // If all of this succeeded, we treat it as const-evaluable.
    result->mFields = makeArrayView(mAllocator, fields);
    result->mIsConstEvaluable = true;

    return result;
}

} // namespace simlang
