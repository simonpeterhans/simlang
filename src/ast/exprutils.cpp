#include "ast/exprutils.h"

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/flags.h"

namespace simlang
{

bool hasValidResolvedType(ExpressionNode* expr)
{
    Type* type = expr->mResolvedType;
    return (type != nullptr) && (type->mKind != TypeKind::cError);
}

bool isIdentifierExpressionNamed(ExpressionNode* expr, std::string_view name)
{
    if (expr == nullptr || expr->mNodeType != NodeType::cIdentifier)
    {
        return false;
    }

    return identifierEquals(static_cast<IdentifierNode*>(expr)->mIdentifier, name);
}

bool isAddressableExpression(ExpressionNode* expr)
{
    switch (expr->mNodeType)
    {
        case NodeType::cIdentifier:
        {
            auto* id = static_cast<IdentifierNode*>(expr);
            switch (id->mSymbol->mSymbolType)
            {
                case SymbolType::cStackVariable:
                case SymbolType::cParameter:
                case SymbolType::cGlobalVariable:
                case SymbolType::cMemberVariable:
                {
                    // Stack, global and member variables as well as params are addressable.
                    return true;
                }
                default:
                {
                    // Everything else is not.
                    return false;
                }
            }
        }
        case NodeType::cThis:
        {
            // This is always a pointer and thus addressable.
            return true;
        }
        case NodeType::cImplicitCast:
        case NodeType::cCast:
        {
            // Casts are never addressable.
            return false;
        }
        case NodeType::cModuleAccess:
        {
            // Recurse into the module and check that.
            auto* moduleAccess = static_cast<ModuleAccessNode*>(expr);
            return isAddressableExpression(moduleAccess->mRight);
        }
        case NodeType::cMemberAccess:
        {
            // Check the receiver type.
            auto* memberAccess = static_cast<MemberAccessNode*>(expr);
            if (hasValidResolvedType(memberAccess->mReceiver) == false)
            {
                return false;
            }

            // Get the receiver type.
            Type* receiverType = memberAccess->mReceiver->mResolvedType;
            if (receiverType->mKind == TypeKind::cClass)
            {
                // If it's a class, it's always addressable.
                return true;
            }

            if (receiverType->mKind == TypeKind::cStruct)
            {
                // If it's a struct, check the receiver.
                // This is because we might get a temp like getStruct().x, which is not addressable.
                return isAddressableExpression(memberAccess->mReceiver);
            }

            return false;
        }
        case NodeType::cIndexCall:
        {
            // Check the receiver type.
            auto* indexCall = static_cast<IndexCallNode*>(expr);
            if (hasValidResolvedType(indexCall->mReceiver) == false)
            {
                return false;
            }

            return false;
        }
        default:
        {
            return false;
        }
    }
}

void markExpressionUsedAsLValue(ExpressionNode* expr)
{
    // Mark the node as lvalue.
    expr->mFlags.set(cExprIsUsedAsLValue, true);

    // Recursively mark any receivers as lvalues as well.
    switch (expr->mNodeType)
    {
        case NodeType::cIndexCall:
        {
            auto* indexCall = static_cast<IndexCallNode*>(expr);
            markExpressionUsedAsLValue(indexCall->mReceiver);
            break;
        }
        case NodeType::cMemberAccess:
        {
            auto* memberAccess = static_cast<MemberAccessNode*>(expr);
            markExpressionUsedAsLValue(memberAccess->mReceiver);
            break;
        }
        case NodeType::cModuleAccess:
        {
            auto* moduleAccess = static_cast<ModuleAccessNode*>(expr);
            markExpressionUsedAsLValue(moduleAccess->mRight);
            break;
        }
        default:
        {
            break;
        }
    }
}

} // namespace simlang
