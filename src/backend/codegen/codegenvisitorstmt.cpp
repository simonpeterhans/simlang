#include <utility>
#include <vector>

#include "ast/assignmentop.h"
#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "backend/backendstate.h"
#include "backend/bytecode/bytecodebuilder.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/codegen/place.h"
#include "backend/layout/layout.h"
#include "backend/typeidutils.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "symbol/symbol.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

static BinaryOp getCompoundBinaryOp(AssignmentOp op)
{
    switch (op)
    {
        case AssignmentOp::cAssAdd: return BinaryOp::cAdd;
        case AssignmentOp::cAssSub: return BinaryOp::cSub;
        case AssignmentOp::cAssMul: return BinaryOp::cMul;
        case AssignmentOp::cAssDiv: return BinaryOp::cDiv;
        case AssignmentOp::cAssMod: return BinaryOp::cMod;
        case AssignmentOp::cAssAnd: return BinaryOp::cBitAnd;
        case AssignmentOp::cAssOr: return BinaryOp::cBitOr;
        case AssignmentOp::cAssXor: return BinaryOp::cBitXor;
        case AssignmentOp::cAssShl: return BinaryOp::cShiftL;
        case AssignmentOp::cAssShr: return BinaryOp::cShiftR;
        default: return BinaryOp::cInvalid;
    }
}

bool CodeGenVisitor::emitCompoundAssignmentOpcode(AssignmentOp op, PrimitiveTypeKind typeKind)
{
    // The assignment was already emitted, do the binary op here.
    BinaryOp binaryOp = getCompoundBinaryOp(op);
    // Emit that like any other binary op.
    return emitArithmeticOrBitwiseOpcode(binaryOp, typeKind);
}

bool CodeGenVisitor::emitCompoundAssignment(AssignmentStatementNode* node)
{
    ExpressionNode* lhs = node->mLHS;
    ExpressionNode* rhs = node->mRHS;
    PrimitiveTypeKind ptk = getPrimitiveKind(lhs->mResolvedType);

    // First, we check if we can do this with a direct load/store without needing to compute the address.
    Place directDst;
    if (tryGetDirectPlace(lhs, directDst))
    {
        // If we have a direct place, load it (e.g. a local, global, or stack variable).
        if (emitLoadFromPlace(directDst) == false)
        {
            return false;
        }

        // Emit the rhs value to assign.
        if (visit(rhs) == false)
        {
            return false;
        }

        // Emit the binary opcode from the compound assignment.
        if (emitCompoundAssignmentOpcode(node->mOp, ptk) == false)
        {
            return false;
        }

        // Store the resulting value to the destination.
        return emitStoreToPlace(directDst);
    }

    // If we have an indirect place (class/list), check if we have a simple member access/index call.
    if (canFuseLValueAccess(lhs))
    {
        // If so, do optimized stuff.
        return tryEmitFusedCompoundAssignment(lhs, rhs, node->mOp);
    }

    // Otherwise, we have to go the slow/generic way.
    // Emit the address.
    if (emitAddress(lhs, AddressMode::cStorage) == false)
    {
        return false;
    }

    // Duplicate the address so one copy can be used to load, the other to store.
    emit<OpCode::cDup>();
    // Load the value from the address.
    emit<OpCode::cLoadRef>();

    if (visit(rhs) == false)
    {
        return false;
    }

    // Emit the binary opcode from the compound assignment.
    if (emitCompoundAssignmentOpcode(node->mOp, ptk) == false)
    {
        return false;
    }

    // Store the result back through the address copy we kept.
    emit<OpCode::cStoreRef>();

    return true;
}

bool CodeGenVisitor::visitExpressionStatement(ExpressionStatementNode* node)
{
    // Visit the expression.
    if (visit(node->mExpression) == false)
    {
        return false;
    }

    // If we resolved to something else than void, our stack is not empty and we need to pop whatever is there.
    // This has to be different if we ever allow multiple return values.
    VMWord words = layout::getWordSizeForType(node->mExpression->mResolvedType);
    if (words == 1)
    {
        emit<OpCode::cPop>();
    }
    else if (words > 1)
    {
        emit<OpCode::cPopN>(static_cast<OpWordCount>(words));
    }

    return true;
}

bool CodeGenVisitor::visitAssignmentStatement(AssignmentStatementNode* node)
{
    // Normal assignment: Directly store the rhs into the lhs.
    if (node->mOp == AssignmentOp::cAss)
    {
        return emitStoreIntoLValue(node->mLHS, node->mRHS);
    }

    // Compound assignment: We need to do extra stuff.
    return emitCompoundAssignment(node);
}

bool CodeGenVisitor::visitVariableDeclarationStatement(VariableDeclarationStatementNode* node)
{
    // Globals are handled in the const data layout visitor.
    if (mCurrentFunctionInfo == nullptr)
    {
        return true;
    }

    Symbol* s = node->mSymbol;
    Type* t = s->mType;
    LocalIdx localIdx = static_cast<LocalIdx>(s->mIndex);

    // Get the place for the local based on type and index.
    Place localPlace = Place::makeLocalPlace(t, localIdx);

    // Emit the initializer expression into the local place.
    return emitInto(node->mInit, localPlace);
}

bool CodeGenVisitor::visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* node)
{
    Symbol* s = node->mSymbol;
    FunctionIdx funIndex = static_cast<FunctionIdx>(s->mIndex);

    // Our bytecode chucks are functions, so create a new one.
    auto bytecodeChunkScope = mBytecodeBuilder.enterFunction(funIndex);

    // Also set the function info for this scope so we properly set the data for it (args, locals, etc).
    ScopedValueBinder functionInfoScope{mCurrentFunctionInfo, &mCtx.mBackend.mFunctionInfos[funIndex]};

    if (node->mIsInitMethod)
    {
        // If this is an initializer, we might have to emit some default initializers.
        const AggregateLayout* layout = mCurrentAggregateType->mLayout;
        for (const FieldLayout& field : layout->mFields)
        {
            Symbol* fieldSymbol = field.mSymbol;

            // If the field has no initializer, skip.
            auto* decl = static_cast<VariableDeclarationStatementNode*>(fieldSymbol->mDeclNode);
            if (decl->mInit == nullptr)
            {
                continue;
            }

            // If the field is explicitly assigned in the initializer (constructor) already, bail.
            // (These are all member assignment statements before the first non-assignment statement.)
            bool isExplicitlyAssigned = false;
            for (Symbol* assignedField : node->mInitializerAssignedFields)
            {
                if (assignedField == fieldSymbol)
                {
                    isExplicitlyAssigned = true;
                    break;
                }
            }

            if (isExplicitlyAssigned)
            {
                continue;
            }

            // Otherwise, load "this" at the offset the field is.
            emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));
            emitFieldReference(field.mOffset);

            // Then, emit the initializer of the variable declaration.
            if (emitInto(decl->mInit, Place::makeAddressOnStackPlace(fieldSymbol->mType)) == false)
            {
                return false;
            }
        }
    }

    // Emit the function body.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    // If we have void as the return type we may have to add a return op.
    auto* functionType = static_cast<FunctionType*>(s->mType);
    if (getPrimitiveKind(functionType->mReturnType) == PrimitiveTypeKind::cVoid)
    {
        // If we don't have a return statement, we need to emit a return with no value.
        emit<OpCode::cReturn>();
    }

    return true;
}

bool CodeGenVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If the type declaration is a template, do nothing.
    // Derived types will be processed separately (flagged cStmtIsTemplateInstance).
    if (node->isTemplate())
    {
        return true;
    }

    // For interfaces, also do nothing.
    if (node->isInterface())
    {
        return true;
    }

    // Enter the aggregate scope so the members know their parent.
    ScopedValueBinder aggregateScope{mCurrentAggregateType, static_cast<AggregateType*>(node->mSymbol->mType)};

    // Emit all of our members.
    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    return true;
}

bool CodeGenVisitor::visitIfStatement(IfStatementNode* node)
{
    u32 endLabel = makeLabel();

    // Visit the if branches.
    for (IfBranchStatementNode* branch : node->mBranches)
    {
        u32 trueLabel = makeLabel();
        u32 blockEndLabel = makeLabel();

        // If the condition is true, we jump to the body, otherwise we jump to the end of the block.
        if (visitAsCondition(branch->mCondition, trueLabel, blockEndLabel) == false)
        {
            return false;
        }

        emit<OpCode::cLabel>(trueLabel);

        // Emit the body stuff.
        if (visit(branch->mBody) == false)
        {
            return false;
        }

        // If we executed the block, we get here and have to jump to the end of the if statement.
        emit<OpCode::cJump>(endLabel);

        // Otherwise, we jumped to the end of the block and continue from there.
        emit<OpCode::cLabel>(blockEndLabel);
    }

    // Handle the else branch if we have one.
    if (node->mElseBody)
    {
        // If the else branch is not empty, we emit it now.
        if (visit(node->mElseBody) == false)
        {
            return false;
        }
    }

    emit<OpCode::cLabel>(endLabel);

    return true;
}

bool CodeGenVisitor::visitForStatement(ForStatementNode* node)
{
    // Resolve the init statement before anything else.
    if (node->mInit != nullptr)
    {
        if (visit(node->mInit) == false)
        {
            return false;
        }
    }

    // Make the labels.
    u32 startLabel = makeLabel();
    u32 incrementLabel = makeLabel();
    u32 endLabel = makeLabel();

    // Push the loop onto the stack.
    // If we continue, we need to go to the increment label.
    mControlStack.push_back(ControlContext{endLabel, incrementLabel, true});
    OnScopeEnd popControl(
        [&]()
        {
            mControlStack.pop_back();
        });

    // Emit the start label.
    emit<OpCode::cLabel>(startLabel);

    // Resolve the condition.
    if (node->mCondition != nullptr)
    {
        u32 bodyLabel = makeLabel();

        // If the condition is true, we go to the body label, otherwise to the end label.
        if (visitAsCondition(node->mCondition, bodyLabel, endLabel) == false)
        {
            return false;
        }

        // Emit the body label.
        // This is not needed if we have no condition (duh).
        emit<OpCode::cLabel>(bodyLabel);
    }

    // Emit the body statement.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    // Emit the increment label (where continue statements jump to).
    emit<OpCode::cLabel>(incrementLabel);

    // Execute the increment statement.
    // The optimizer should correctly redirect the jump if we have no increment.
    if (node->mIncrement != nullptr)
    {
        if (visit(node->mIncrement) == false)
        {
            return false;
        }
    }

    // Jump back to the start label to re-evaluate the condition.
    emit<OpCode::cJump>(startLabel);

    // Emit the end label.
    emit<OpCode::cLabel>(endLabel);

    return true;
}

bool CodeGenVisitor::visitWhileStatement(WhileStatementNode* node)
{
    // Make the labels.
    u32 startLabel = makeLabel();
    u32 bodyLabel = makeLabel();
    u32 endLabel = makeLabel();

    // Push the loop onto the stack.
    // If we continue, we need to go to the start label.
    mControlStack.push_back(ControlContext{endLabel, startLabel, true});
    OnScopeEnd popControl(
        [&]()
        {
            mControlStack.pop_back();
        });

    // The one at the start we can emit immediately.
    emit<OpCode::cLabel>(startLabel);

    // Resolve the condition.
    // Emits a jump to the end label after the block if the condition evaluates to false.
    if (visitAsCondition(node->mCondition, bodyLabel, endLabel) == false)
    {
        return false;
    }

    // Emit the body label after the condition.
    emit<OpCode::cLabel>(bodyLabel);

    // Emit the body stuff.
    if (visit(node->mBody) == false)
    {
        return false;
    }

    // After the body, we jump back to the start label.
    emit<OpCode::cJump>(startLabel);

    // Finally, emit the end label after everything.
    emit<OpCode::cLabel>(endLabel);

    return true;
}

bool CodeGenVisitor::visitSwitchStatement(SwitchStatementNode* node)
{
    // Store the expression into a temp.
    // That temp is then compared against every case (until we find a match).
    Place switchValue;
    if (allocateTemporaryPlace(node->mExpression->mResolvedType, switchValue) == false)
    {
        return false;
    }

    if (emitInto(node->mExpression, switchValue) == false)
    {
        return false;
    }

    u32 endLabel = makeLabel();
    u32 defaultLabel = endLabel;

    // Make all the section labels.
    std::vector<u32> sectionLabels(node->mSections.size());
    for (usize i = 0; i < node->mSections.size(); ++i)
    {
        sectionLabels[i] = makeLabel();
    }

    // Assign all the labels to sections (improvised jump table).
    for (usize i = 0; i < node->mSections.size(); ++i)
    {
        SwitchSectionStatementNode* section = node->mSections[i];
        if (section->mCaseExpression == nullptr)
        {
            // Null expression: Default case.
            defaultLabel = sectionLabels[i];
            continue;
        }

        // Load the value we are comparing against.
        if (emitLoadFromPlace(switchValue) == false)
        {
            return false;
        }

        // Emit the case expression.
        emitIntegerImmediate(section->mCaseValue);

        // If the case expression matches, jump to the section.
        emit<OpCode::cIEQ>();
        emit<OpCode::cJumpNZ>(sectionLabels[i]);
    }

    // If none of the cases match, jump to the default label.
    emit<OpCode::cJump>(defaultLabel);

    // Push the control context so we can break out of the cases.
    mControlStack.push_back(ControlContext{endLabel, 0, false});
    OnScopeEnd popControlStackOnEnd(
        [&]()
        {
            mControlStack.pop_back();
        });

    // Emit all section code with the corresponding label.
    for (usize i = 0; i < node->mSections.size(); ++i)
    {
        emit<OpCode::cLabel>(sectionLabels[i]);

        if (visit(node->mSections[i]->mBody) == false)
        {
            return false;
        }
    }

    // Don't forget the end label.
    emit<OpCode::cLabel>(endLabel);

    return true;
}

bool CodeGenVisitor::visitReturnStatement(ReturnStatementNode* node)
{
    // If we have a return value, we need to push it on the stack.
    if (node->mExpression)
    {
        if (visit(node->mExpression) == false)
        {
            return false;
        }
    }

    // Emit the return opcode.
    emit<OpCode::cReturn>();

    return true;
}

bool CodeGenVisitor::visitBreakStatement(BreakStatementNode*)
{
    // We can always break (from switches/loops), so directly look up the label.
    u32 label = mControlStack.back().mBreakLabel;
    // Jump to that.
    emit<OpCode::cJump>(label);

    return true;
}

bool CodeGenVisitor::visitContinueStatement(ContinueStatementNode*)
{
    for (auto it = mControlStack.rbegin(); it != mControlStack.rend(); ++it)
    {
        // For continue, we need to first find the innermost block that allows continuing.
        if (it->mCanContinue == false)
        {
            continue;
        }

        // Jump to that.
        emit<OpCode::cJump>(it->mContinueLabel);

        return true;
    }

    return false;
}

bool CodeGenVisitor::visitPrintStatement(PrintStatementNode* node)
{
    // Visit the expression to print.
    if (visit(node->mExpression) == false)
    {
        return false;
    }

    // Get the runtime print kind from the resolved type.
    PrimitiveTypeKind typeKind = getPrimitiveKind(node->mExpression->mResolvedType);
    // Emit the print operation.
    emit<OpCode::cPrint>(static_cast<u8>(getStringFormatArgKind(typeKind)));

    return true;
}

bool CodeGenVisitor::visitTranslationUnit(TranslationUnitNode* node)
{
    for (ASTNode* stmt : node->mNodes)
    {
        if (visit(stmt) == false)
        {
            return false;
        }
    }

    return true;
}

} // namespace simlang
