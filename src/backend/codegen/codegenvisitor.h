#pragma once

#include <utility>
#include <vector>

#include "ast/astwalker.h"
#include "backend/bytecode/bytecodebuilder.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "runtime/vmdefines.h"
#include "source/sourcerange.h"
#include "util/types.h"

namespace simlang
{

template <typename T>
class ArrayView;
struct AggregateType;
struct CompilerContext;
struct FunctionInfo;
struct FunctionType;
struct InterfaceType;
struct ListType;
struct MapType;
struct Place;
struct Type;
enum class AssignmentOp : u8;
enum class BinaryOp : u8;
enum class OpCode : u8;
enum class PrimitiveTypeKind : u8;

class CodeGenVisitor : public ASTWalker<CodeGenVisitor>
{
public:
    explicit CodeGenVisitor(CompilerContext& ctx);

    void enterNode(ASTNode* node);
    void leaveNode(ASTNode*);

    // Expressions.
    bool visitImplicitCast(ImplicitCastNode* node);
    bool visitCast(CastNode* node);
    bool visitIdentifier(IdentifierNode* node);
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

    // Statements.
    bool visitExpressionStatement(ExpressionStatementNode* node);
    bool visitAssignmentStatement(AssignmentStatementNode* node);
    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* node);
    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node);
    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* node);
    bool visitIfStatement(IfStatementNode* node);
    bool visitForStatement(ForStatementNode* node);
    bool visitWhileStatement(WhileStatementNode* node);
    bool visitSwitchStatement(SwitchStatementNode* node);
    bool visitReturnStatement(ReturnStatementNode* node);
    bool visitBreakStatement(BreakStatementNode*);
    bool visitContinueStatement(ContinueStatementNode*);
    bool visitPrintStatement(PrintStatementNode* node);

    // Translation unit.
    bool visitTranslationUnit(TranslationUnitNode* node);

private:
    enum class AddressMode : u8
    {
        // Address must point at some non-temporary writable storage.
        cStorage,
        // Address can point at a temporary for read only.
        cReadOnly,
    };

    struct ControlContext
    {
        u32 mBreakLabel = 0;
        u32 mContinueLabel = 0;
        bool mCanContinue = false;
    };

    // General.
    template <OpCode C, typename... Args>
    void emit(Args&&... args)
    {
        mBytecodeBuilder.emit<C>(std::forward<Args>(args)...);
    }

    BytecodeLabel makeLabel();

    // Place storage.
    bool allocateTemporaryLocal(Type* type, LocalIdx& out);
    bool allocateTemporaryPlace(Type* type, Place& out);

    // Place/lvalue emission.
    bool tryGetDirectPlace(ExpressionNode* expr, Place& out);
    bool emitReferenceToPlace(const Place& place);
    bool emitLoadFromPlace(const Place& place);
    bool emitStoreToPlace(const Place& place);
    bool emitInto(ExpressionNode* expr, const Place& dst);
    bool emitTemporaryAddress(ExpressionNode* expr);
    bool emitAddress(ExpressionNode* expr, AddressMode mode);

    bool canFuseLValueAccess(ExpressionNode* expr);
    bool tryEmitFusedLoadFromLValue(ExpressionNode* expr);
    bool tryEmitFusedStoreIntoLValue(ExpressionNode* lhs, ExpressionNode* rhs);
    bool tryEmitFusedCompoundAssignment(ExpressionNode* lhs, ExpressionNode* rhs, AssignmentOp op);

    bool emitLoadFromLValue(ExpressionNode* expr);
    bool emitStoreIntoLValue(ExpressionNode* lhs, ExpressionNode* rhs);

    // Opcode/type emission.
    void emitIntegerImmediate(i32 value);
    void emitFieldReference(u32 offset);
    bool emitComparisonOpCode(BinaryOp op, PrimitiveTypeKind operandKind);
    bool emitReferenceEqualityOpCode(BinaryOp op);
    bool emitInterfaceObjectRefFromPlace(const Place& place);
    bool emitInterfaceObjectRef(ExpressionNode* expr);
    bool emitInterfaceEqualityComparison(BinaryOp op, ExpressionNode* lhs, ExpressionNode* rhs);
    bool emitEqualityComparison(BinaryOp op, Type* type, const Place& lhs, const Place& rhs);
    bool emitStructEqualityComparison(BinaryOp op, AggregateType* structType, const Place& lhs, const Place& rhs);
    bool emitStructEqualityOperands(BinaryOpNode* node);
    bool emitArithmeticOrBitwiseOpcode(BinaryOp op, PrimitiveTypeKind resultKind);
    bool emitInterfaceConversion(Type* fromType, InterfaceType* toType);
    bool emitPrimitiveConversion(PrimitiveTypeKind fromKind, PrimitiveTypeKind toKind);

    // Boolean/control flow.
    bool emitBoolAsValue(ExpressionNode* expr);
    bool visitAsCondition(ExpressionNode* expr, u32 trueLabel, u32 falseLabel);

    // Interface metadata.
    InterfaceCallIdx getInterfaceCallIndex(const SourceRange& range,
                                           InterfaceMethodSlot slot,
                                           OpWordCount argWords,
                                           ReturnWordCount returnWords);
    u32 getInterfaceTableIndex(AggregateType* aggregateType, InterfaceType* interfaceType);

    // Call and interface emission.
    bool emitCallArguments(ArrayView<ExpressionNode*> args, const FunctionType* funcType);
    bool emitMethodReceiver(MemberAccessNode* memberAccess);
    bool emitInterfaceMethodDispatch(MemberAccessNode* memberAccess, const FunctionType* funcType);
    bool emitListMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess);
    bool emitMapMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess);
    bool emitMethodCall(FunctionCallNode* node, MemberAccessNode* memberAccess);
    bool emitFreeFunctionOrSyscallCall(FunctionCallNode* node);

    // Object construction.
    bool emitNewList(NewObjectNode* node, ListType* listType);
    bool emitNewMap(NewObjectNode* node, MapType* mapType);
    bool emitNewClass(NewObjectNode* node, AggregateType* classType);
    bool emitNewStruct(NewObjectNode* node, AggregateType* structType);

    // Assignment.
    bool emitCompoundAssignmentOpcode(AssignmentOp op, PrimitiveTypeKind typeKind);
    bool emitCompoundAssignment(AssignmentStatementNode* node);

    CompilerContext& mCtx;
    BytecodeBuilder mBytecodeBuilder;

    std::vector<ControlContext> mControlStack;
    std::vector<SourceRange> mSourceRangeStack;

    FunctionInfo* mCurrentFunctionInfo = nullptr;
    AggregateType* mCurrentAggregateType = nullptr;
};

} // namespace simlang
