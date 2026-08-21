#pragma once

#include <unordered_set>

#include "ast/astwalker.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

template <typename T>
class ArrayView;
class SourceRange;
struct CompilerContext;
struct FunctionType;
struct InterfaceType;
struct ModuleEntry;
struct Symbol;
struct Type;

class TypeCheckVisitor : public ASTWalker<TypeCheckVisitor>
{
public:
    explicit TypeCheckVisitor(CompilerContext& ctx);

    bool run(ASTNode* node);

    bool visitIdentifier(IdentifierNode* node);
    bool visitCast(CastNode* node);
    bool visitThis(ThisNode* node);
    bool visitIntLiteral(IntLiteralNode* node);
    bool visitFloatLiteral(FloatLiteralNode* node);
    bool visitBoolLiteral(BoolLiteralNode* node);
    bool visitStringLiteral(StringLiteralNode* node);
    bool visitNullLiteral(NullLiteralNode* node);
    bool visitFormatString(FormatStringNode* node);
    bool visitNewObject(NewObjectNode* node);
    bool visitFunctionCall(FunctionCallNode* node);
    bool visitIndexCall(IndexCallNode* node);
    bool visitMemberAccess(MemberAccessNode* node);
    bool visitModuleAccess(ModuleAccessNode* node);
    bool visitUnaryOp(UnaryOpNode* node);
    bool visitBinaryOp(BinaryOpNode* node);
    bool visitTernaryExpr(TernaryExprNode* node);

    bool visitAssignmentStatement(AssignmentStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);
    bool visitIfBranchStatement(IfBranchStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitWhileStatement(WhileStatementNode* node);
    bool visitSwitchStatement(SwitchStatementNode* node);
    bool visitReturnStatement(ReturnStatementNode* node);
    bool visitBreakStatement(BreakStatementNode* node);
    bool visitContinueStatement(ContinueStatementNode* node);
    bool visitPrintStatement(PrintStatementNode* node);
    bool visitTranslationUnit(TranslationUnitNode* node);

    bool visitParamDeclaration(ParamDeclarationNode* node);

    bool visitNamedTypeSpecifier(NamedTypeSpecifierNode* node);

private:
    // Type state.
    Type* getErrorType() const;
    bool isErrorType(Type* type) const;
    void markError(ExpressionNode* expr) const;
    Type* requireNonNullType(Type* type, SourceRange range, const char* context) const;
    Type* requireValueType(Type* type, SourceRange range, const char* context) const;

    // Symbol and signature resolution.
    bool resolveType(Symbol* symbol);
    bool resolveFunctionSignature(FunctionDeclarationStatementNode* node);

    // Conversion and coercion.
    bool allowExplicitCast(Type* fromType, Type* toType);
    bool allowImplicitCast(Type* fromType, Type* toType);
    ImplicitCastNode* createImplicitCast(ExpressionNode* node, Type* targetType) const;
    bool convertExpressionToType(ExpressionNode*& expr, Type* targetType);
    bool convertArgumentToParameterType(ExpressionNode*& expr, Type* targetType, usize argIndex);

    // Semantic requirements and diagnostics.
    bool checkMemberAccess(Symbol* symbol, Symbol* ownerTypeSymbol) const;
    Symbol* getInitializerAssignedField(ExpressionNode* lhs) const;
    bool isInitializerFieldAssignment(AssignmentStatementNode* node) const;
    bool requireAssignableLValue(ExpressionNode* expr, bool allowImmutable = false) const;
    bool requireBoolExpression(ExpressionNode* expr) const;
    bool requireNonVoidPrimitive(ExpressionNode* expr) const;
    void diagnoseInvalidBinaryOperands(BinaryOpNode* node) const;
    void diagnoseInvalidAssignmentOperands(AssignmentStatementNode* node) const;

    // Calls and references.
    bool checkCallArguments(SourceRange range, ArrayView<CallArgument>& args, const FunctionType* funcType);

    // Aggregates and interfaces.
    bool allowClassToInterfaceCast(Type* aggregateType, InterfaceType* interfaceType);
    bool resolveImplementedInterfaces(TypeDeclarationStatementNode* node);
    bool checkInterface(TypeDeclarationStatementNode* node);

    // Operators.
    Type* promoteIntFloatOperands(BinaryOpNode* node);
    void resolveBinaryMathOps(BinaryOpNode* node);
    void resolveBinaryEqualityOps(BinaryOpNode* node);
    void resolveBinaryBitOps(BinaryOpNode* node) const;

    // Control-flow scopes.
    ScopedValueBinder<i32> enterBreakContext();
    ScopedValueBinder<i32> enterContinueContext();

    CompilerContext& mCtx;

    Type* mCurrentReturnType = nullptr;
    FunctionDeclarationStatementNode* mCurrentFunction = nullptr;
    ModuleEntry* mCurrentModule = nullptr;
    Symbol* mCurrentTypeSymbol = nullptr;
    std::unordered_set<Symbol*> mResolvingSymbols;
    i32 mBreakContextDepth = 0;
    i32 mContinueContextDepth = 0;
    bool mInCall = false;
};

} // namespace simlang
