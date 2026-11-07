#include "sema/constructoranalysis.h"

#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "symbol/symbol.h"
#include "type/types.h"
#include "util/arenautils.h"

namespace simlang
{

static bool isConstEvaluableExpression(ExpressionNode* node)
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
            // Parameters (always const), or global/locals that are immutable are eligible.
            Symbol* symbol = static_cast<IdentifierNode*>(node)->mSymbol;
            return symbol->mSymbolType == SymbolType::cParameter ||
                   ((symbol->mSymbolType == SymbolType::cGlobalVariable ||
                     symbol->mSymbolType == SymbolType::cStackVariable) &&
                    symbol->mFlags.test(SymbolFlags::cMutable) == false);
        }
        case NodeType::cModuleAccess:
        {
            // For module access, check the rhs.
            return isConstEvaluableExpression(static_cast<ModuleAccessNode*>(node)->mRight);
        }
        case NodeType::cImplicitCast:
        case NodeType::cCast:
        {
            // Get the target expr to cast.
            ExpressionNode* target = node->mNodeType == NodeType::cImplicitCast
                                         ? static_cast<ImplicitCastNode*>(node)->mTarget
                                         : static_cast<CastNode*>(node)->mTarget;
            // Matching types and int to float are eligible if the expression is const evaluable.
            return (node->mResolvedType == target->mResolvedType ||
                    (getPrimitiveKind(node->mResolvedType) == PrimitiveTypeKind::cFloat &&
                     getPrimitiveKind(target->mResolvedType) == PrimitiveTypeKind::cInt)) &&
                   isConstEvaluableExpression(target);
        }
        case NodeType::cUnaryOp:
        {
            // Only const-evaluable '-' is currently eligible.
            auto* unary = static_cast<UnaryOpNode*>(node);
            return unary->mOp == UnaryOp::cNeg && isConstEvaluableExpression(unary->mExpr);
        }
        case NodeType::cBinaryOp:
        {
            // Only '+', '-', '*', "==" and "!=" are currently eligible.
            auto* binary = static_cast<BinaryOpNode*>(node);
            bool supported = binary->mOp == BinaryOp::cAdd || binary->mOp == BinaryOp::cSub ||
                             binary->mOp == BinaryOp::cMul ||
                             ((binary->mOp == BinaryOp::cEQ || binary->mOp == BinaryOp::cNE) &&
                              getPrimitiveKind(binary->mLeft->mResolvedType) == PrimitiveTypeKind::cBool);
            return supported && isConstEvaluableExpression(binary->mLeft) &&
                   isConstEvaluableExpression(binary->mRight);
        }
        default:
        {
            return false;
        }
    }
}

const ConstructorAnalysis* getConstructorAnalysis(ArenaAllocator& allocator, AggregateType* type, Symbol* constructor)
{
    auto* declaration = static_cast<FunctionDeclarationStatementNode*>(constructor->mDeclNode);

    // If we already computed the analysis for this function, we're done.
    if (declaration->mConstructorAnalysis != nullptr)
    {
        return declaration->mConstructorAnalysis;
    }

    // Otherwise, do the analysis.
    auto* result = allocator.create<ConstructorAnalysis>();
    declaration->mConstructorAnalysis = result;

    // If any parameter is not a primitive or is an inout param, we're not const-evaluable.
    auto* functionType = static_cast<FunctionType*>(constructor->mType);
    for (const FunctionParam& param : functionType->mParamTypes)
    {
        if (param.mIsInOut || param.mType->mKind != TypeKind::cPrimitive)
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
            // If any of the fields are not primitives, we're not const-evaluable either.
            if (member->mType->mKind != TypeKind::cPrimitive)
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
    result->mFields = makeArrayView(allocator, fields);
    result->mIsConstEvaluable = true;

    return result;
}

} // namespace simlang
