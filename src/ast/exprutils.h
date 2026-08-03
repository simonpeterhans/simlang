#pragma once

#include <string_view>

namespace simlang
{

struct ExpressionNode;

bool hasValidResolvedType(ExpressionNode* expr);
bool isIdentifierExpressionNamed(ExpressionNode* expr, std::string_view name);
bool isAddressableExpression(ExpressionNode* expr);
void markExpressionUsedAsLValue(ExpressionNode* expr);

} // namespace simlang
