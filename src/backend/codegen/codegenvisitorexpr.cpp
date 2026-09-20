#include <utility>

#include "ast/binaryop.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/nodetypes.h"
#include "ast/unaryop.h"
#include "backend/backendstate.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/codegen/place.h"
#include "backend/layout/layout.h"
#include "backend/stringdata.h"
#include "backend/typeidutils.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"
#include "runtime/stringdata.h"
#include "runtime/vmdefines.h"
#include "symbol/constvalue.h"
#include "symbol/internedstring.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/asserts.h"
#include "util/bitutils.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

bool CodeGenVisitor::visitImplicitCast(ImplicitCastNode* node)
{
    // Emit the target.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // If we have the same types, do nothing.
    Type* fromType = node->mTarget->mResolvedType;
    Type* toType = node->mResolvedType;
    if (fromType == toType)
    {
        return true;
    }

    // If we implicitly cast to an interface, handle that (we might have to build the index first).
    if (toType->mKind == TypeKind::cInterface)
    {
        return emitInterfaceConversion(fromType, static_cast<InterfaceType*>(toType));
    }

    // Null is already represented by cNullRef, no cast needed.
    if (fromType->mKind == TypeKind::cNull &&
        (toType->mKind == TypeKind::cClass || toType->mKind == TypeKind::cList || toType->mKind == TypeKind::cMap))
    {
        return true;
    }

    // Otherwise, this is a primitive conversion.
    return emitPrimitiveConversion(getPrimitiveKind(fromType), getPrimitiveKind(toType));
}

bool CodeGenVisitor::visitCast(CastNode* node)
{
    // Emit the target.
    if (visit(node->mTarget) == false)
    {
        return false;
    }

    // If we have the same types, do nothing.
    Type* fromType = node->mTarget->mResolvedType;
    Type* toType = node->mResolvedType;
    if (fromType == toType)
    {
        return true;
    }

    // If we implicitly cast to an interface, handle that (we might have to build the index first).
    if (toType->mKind == TypeKind::cInterface)
    {
        return emitInterfaceConversion(fromType, static_cast<InterfaceType*>(toType));
    }

    // If we're casting from an interface to a class, handle that.
    if (fromType->mKind == TypeKind::cInterface && toType->mKind == TypeKind::cClass)
    {
        // Pop the table index, which we don't need for the conversion.
        emit<OpCode::cPop>();

        TypeID typeID;
        if (getRuntimeTypeID(toType, typeID) == false)
        {
            return false;
        }

        // Push the type we're casting to and do the cast.
        emit<OpCode::cCheckCast>(typeID);

        return true;
    }

    // Null is already represented by cNullRef, no cast needed.
    if (fromType->mKind == TypeKind::cNull &&
        (toType->mKind == TypeKind::cClass || toType->mKind == TypeKind::cList || toType->mKind == TypeKind::cMap))
    {
        return true;
    }

    // Otherwise, this is a primitive conversion.
    return emitPrimitiveConversion(getPrimitiveKind(fromType), getPrimitiveKind(toType));
}

bool CodeGenVisitor::visitIdentifier(IdentifierNode* node)
{
    Symbol* symbol = node->mSymbol;

    // If this is a function or a syscall and we visit it, emit it as callable value.
    if (symbol->mSymbolType == SymbolType::cFunction || symbol->mSymbolType == SymbolType::cSyscall)
    {
        emitIntegerImmediate(cNullRef);

        VMWord entryToken = cInvalidFunctionEntryToken;
        if (symbol->mSymbolType == SymbolType::cFunction)
        {
            entryToken = makeFunctionEntryToken(static_cast<FunctionIdx>(symbol->mIndex));
        }
        else
        {
            entryToken =
                makeSyscallEntryToken(mCtx.mBackend.mFunctionInfos.size(), static_cast<SyscallIdx>(symbol->mIndex));
        }

        emit<OpCode::cPush32>(entryToken);

        return true;
    }

    // If this is a constexpr, we can try to emit the value directly.
    if (symbol->mFlags.test(SymbolFlags::cConstExpr))
    {
        if (node->mResolvedType->mKind == TypeKind::cPrimitive)
        {
            switch (symbol->mConstValue.mPrimitiveKind)
            {
                case PrimitiveTypeKind::cInt:
                {
                    // Don't emit more than we have to (i8/i16 if possible).
                    emitIntegerImmediate(symbol->mConstValue.as.mInteger);
                    return true;
                }
                case PrimitiveTypeKind::cFloat:
                {
                    // Push the float as u32.
                    u32 val = bits::bitCast<u32>(symbol->mConstValue.as.mFloat);
                    emit<OpCode::cPush32>(val);
                    return true;
                }
                case PrimitiveTypeKind::cBool:
                {
                    // Bools are 0 or 1.
                    u8 val = (symbol->mConstValue.as.mBool) ? 1U : 0U;
                    emit<OpCode::cPush8>(val);
                    return true;
                }
                case PrimitiveTypeKind::cString:
                {
                    // Register the string literal and use that index.
                    StringLiteralIdx stringIndex;
                    if (mCtx.mBackend.mStrings.getLiteralIndex(symbol->mConstValue.as.mString, stringIndex) == false)
                    {
                        SIMLANG_BREAK("Constexpr string missing from string layout.");
                        return false;
                    }

                    emit<OpCode::cPushString>(stringIndex);

                    return true;
                }
                default:
                {
                    break;
                }
            }
        }
    }

    // If it is a captured identifier, emit it from the lambda environment (from the heap).
    if (const LambdaCapture* capture = findCurrentCapture(symbol))
    {
        emitEnvironmentValue(*capture, node->mResolvedType);
        return true;
    }

    // Otherwise, load the lvalue.
    return emitLoadFromLValue(node);
}

bool CodeGenVisitor::visitThis(ThisNode* node)
{
    // If we're using "this" from a capture, handle that.
    if (const LambdaCapture* capture = findCurrentThisCapture())
    {
        emitEnvironmentValue(*capture, node->mResolvedType);
        return true;
    }

    // Note that this path is only taken if "this" is used as an rvalue.
    // If it's used as an lvalue, it should go through the storage address path.
    // Classes: "this" is always local 0, so we can just load that and be done.
    if (node->mResolvedType->mKind == TypeKind::cClass)
    {
        emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));
        return true;
    }

    // Structs: Since they are value types, we need to push the entire thing onto the stack.
    // Load "this" (struct address) at local 0.
    emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));

    // Then push the entire thing onto the stack.
    return emitLoadFromPlace(Place::makeAddressOnStackPlace(node->mResolvedType));
}

const LambdaCapture* CodeGenVisitor::findCurrentCapture(Symbol* symbol) const
{
    if (mCurrentLambda == nullptr)
    {
        return nullptr;
    }

    // Find the capture for a specific symbol.
    for (const LambdaCapture& capture : mCurrentLambda->mCaptures)
    {
        if (capture.mKind == LambdaCaptureKind::cSymbol && capture.mSymbol == symbol)
        {
            return &capture;
        }
    }

    return nullptr;
}

const LambdaCapture* CodeGenVisitor::findCurrentThisCapture() const
{
    if (mCurrentLambda == nullptr)
    {
        return nullptr;
    }

    // Go through all the captures and find the one that is the "this".
    for (const LambdaCapture& capture : mCurrentLambda->mCaptures)
    {
        if (capture.mKind == LambdaCaptureKind::cThis)
        {
            return &capture;
        }
    }

    return nullptr;
}

Type* CodeGenVisitor::getLambdaCaptureType(const LambdaNode* lambda, const LambdaCapture& capture) const
{
    // If this is a symbol, return its type.
    if (capture.mKind == LambdaCaptureKind::cSymbol)
    {
        return capture.mSymbol->mType;
    }

    // Otherwise this is "this", so get the type directly from the lambda node.
    return lambda->mLexicalThisType;
}

void CodeGenVisitor::emitEnvironmentValue(const LambdaCapture& capture, Type* type)
{
    // Here, we load something from the lambda environment (from the heap).
    // Get the size of the type we want to emit.
    u32 words = layout::getWordSizeForType(type);
    // Get the offset of the capture from the environment.
    FieldOffset offset = static_cast<FieldOffset>(capture.mEnvironmentOffset);

    // Emit the load.
    if (words == 1)
    {
        emit<OpCode::cLoadCapture>(offset);
    }
    else
    {
        emit<OpCode::cLoadCaptureN>(offset, static_cast<OpWordCount>(words));
    }
}

bool CodeGenVisitor::emitLambdaCaptureValue(const LambdaNode* lambda, const LambdaCapture& capture)
{
    // Push something onto the stack to initialize a field in an environment.
    Type* captureType = getLambdaCaptureType(lambda, capture);

    if (capture.mKind == LambdaCaptureKind::cThis)
    {
        // If it's a "this", check if we're nested.
        if (const LambdaCapture* currentCapture = findCurrentThisCapture())
        {
            // If that is the case, use that.
            emitEnvironmentValue(*currentCapture, captureType);
            return true;
        }

        // Otherwise, it's the local at index 0.
        emit<OpCode::cLoadLocal>(static_cast<LocalIdx>(0));

        // For structs, we need to load the entire thing (at index 0) since it's a copy.
        if (captureType->mKind == TypeKind::cStruct)
        {
            return emitLoadFromPlace(Place::makeAddressOnStackPlace(captureType));
        }

        return true;
    }

    // Otherwise, it's a symbol.
    Symbol* symbol = capture.mSymbol;
    // If it's already captured from an outer lambda, load it from there.
    if (const LambdaCapture* currentCapture = findCurrentCapture(symbol))
    {
        emitEnvironmentValue(*currentCapture, captureType);
        return true;
    }

    // Otherwise, this is a local.
    LocalIdx localIndex = static_cast<LocalIdx>(symbol->mIndex);
    if (symbol->mFlags.test(SymbolFlags::cInOut))
    {
        // An inout param is an address, so load that.
        emit<OpCode::cLoadLocal>(localIndex);
        return emitLoadFromPlace(Place::makeAddressOnStackPlace(captureType));
    }

    // Load it directly.
    return emitLoadFromPlace(Place::makeLocalPlace(captureType, localIndex));
}

bool CodeGenVisitor::visitLambda(LambdaNode* node)
{
    // This emits the pair of lambda closure (environment) and function entry token.
    // Get the function (lambda) index.
    FunctionIdx functionIndex = static_cast<FunctionIdx>(node->mSymbol->mIndex);

    if (node->mCaptures.empty())
    {
        // If we have 0 captures, we have a null environment.
        emitIntegerImmediate(cNullRef);
        // This means we don't have to create an environment and can directly push the converted index as entry token.
        emit<OpCode::cPush32>(makeFunctionEntryToken(functionIndex));
        return true;
    }

    // Push the captured values in environment layout order.
    for (const LambdaCapture& capture : node->mCaptures)
    {
        if (emitLambdaCaptureValue(node, capture) == false)
        {
            return false;
        }
    }

    // Create the closure (with the environment) based on what we just pushed before.
    emit<OpCode::cNewClosure>(functionIndex);

    return true;
}

bool CodeGenVisitor::visitIntLiteral(IntLiteralNode* node)
{
    emitIntegerImmediate(node->mInt);
    return true;
}

bool CodeGenVisitor::visitFloatLiteral(FloatLiteralNode* node)
{
    u32 val = bits::bitCast<u32>(node->mFloat);
    emit<OpCode::cPush32>(val);
    return true;
}

bool CodeGenVisitor::visitBoolLiteral(BoolLiteralNode* node)
{
    u8 val = (node->mBool) ? 1U : 0U;
    emit<OpCode::cPush8>(val);
    return true;
}

bool CodeGenVisitor::visitStringLiteral(StringLiteralNode* node)
{
    StringLiteralIdx stringIndex;
    if (mCtx.mBackend.mStrings.getLiteralIndex(node->mString, stringIndex) == false)
    {
        SIMLANG_BREAK("String literal missing from string layout.");
        return false;
    }

    emit<OpCode::cPushString>(stringIndex);

    return true;
}

bool CodeGenVisitor::visitNullLiteral(NullLiteralNode*)
{
    emitIntegerImmediate(cNullRef);
    return true;
}

bool CodeGenVisitor::visitFormatString(FormatStringNode* node)
{
    StringFormatTemplateBuilder builder;

    // Add all literals we have in the template.
    for (const InternedString* str : node->mLiterals)
    {
        StringLiteralIdx literalIndex;
        if (mCtx.mBackend.mStrings.getLiteralIndex(str, literalIndex) == false)
        {
            SIMLANG_BREAK("Format string literal missing from string layout.");
            return false;
        }

        builder.appendLiteral(literalIndex, str->mLength);
    }

    // Emit all args and track their kinds.
    for (ExpressionNode* arg : node->mArgs)
    {
        if (visit(arg) == false)
        {
            return false;
        }

        builder.appendArgKind(getStringFormatArgKind(getPrimitiveKind(arg->mResolvedType)));
    }

    // Build the template and register it.
    StringFormatIdx formatIndex;
    StringFormatTemplate tmpl = std::move(builder).build();
    if (mCtx.mBackend.mStringFormats.getTemplateIndex(tmpl, formatIndex) == false)
    {
        SIMLANG_BREAK("String format missing from string layout.");
        return false;
    }

    // Finally emit the index.
    emit<OpCode::cFormatString>(formatIndex);

    return true;
}

bool CodeGenVisitor::visitNewObject(NewObjectNode* node)
{
    if (node->mResolvedType->mKind == TypeKind::cList)
    {
        return emitNewList(node, static_cast<ListType*>(node->mResolvedType));
    }

    if (node->mResolvedType->mKind == TypeKind::cMap)
    {
        return emitNewMap(node, static_cast<MapType*>(node->mResolvedType));
    }

    auto* aggregateType = static_cast<AggregateType*>(node->mResolvedType);
    if (aggregateType->mKind == TypeKind::cClass)
    {
        return emitNewClass(node, aggregateType);
    }

    return emitNewStruct(node, aggregateType);
}

bool CodeGenVisitor::visitFunctionCall(FunctionCallNode* node)
{
    if (node->mReceiver->mNodeType == NodeType::cMemberAccess)
    {
        auto* memberAccess = static_cast<MemberAccessNode*>(node->mReceiver);
        if (memberAccess->mReceiver->mResolvedType->mKind == TypeKind::cList)
        {
            return emitListMethodCall(node, memberAccess);
        }

        if (memberAccess->mReceiver->mResolvedType->mKind == TypeKind::cMap)
        {
            return emitMapMethodCall(node, memberAccess);
        }

        // If this is a normal function, emit a direct call.
        if (memberAccess->mSymbol != nullptr && memberAccess->mSymbol->mSymbolType == SymbolType::cMemberFunction)
        {
            return emitMethodCall(node, memberAccess);
        }

        // Otherwise, this is a call to a field, so it is indirect.
        return emitIndirectFunctionCall(node);
    }

    Symbol* directSymbol = nullptr;
    if (node->mReceiver->mNodeType == NodeType::cIdentifier)
    {
        directSymbol = static_cast<IdentifierNode*>(node->mReceiver)->mSymbol;
    }
    else if (node->mReceiver->mNodeType == NodeType::cModuleAccess)
    {
        directSymbol = static_cast<ModuleAccessNode*>(node->mReceiver)->mSymbol;
    }

    if (directSymbol != nullptr &&
        (directSymbol->mSymbolType == SymbolType::cFunction || directSymbol->mSymbolType == SymbolType::cSyscall))
    {
        return emitFreeFunctionOrSyscallCall(node);
    }

    return emitIndirectFunctionCall(node);
}

bool CodeGenVisitor::visitIndexCall(IndexCallNode* node)
{
    return emitLoadFromLValue(node);
}

bool CodeGenVisitor::visitMemberAccess(MemberAccessNode* node)
{
    // Methods are handled via function call.
    if (node->mSymbol->mSymbolType == SymbolType::cMemberFunction)
    {
        // This could also be an assert.
        return false;
    }

    return emitLoadFromLValue(node);
}

bool CodeGenVisitor::visitModuleAccess(ModuleAccessNode* node)
{
    // We simply delegate to the RHS which has all the information needed.
    return visit(node->mRight);
}

bool CodeGenVisitor::visitUnaryOp(UnaryOpNode* node)
{
    if (node->mOp == UnaryOp::cNeg)
    {
        // Push the child node, then emit the negation operation.
        if (visit(node->mExpr) == false)
        {
            return false;
        }

        PrimitiveTypeKind ptk = getPrimitiveKind(node->mExpr->mResolvedType);
        if (ptk == PrimitiveTypeKind::cInt)
        {
            emit<OpCode::cINeg>();
        }
        else
        {
            emit<OpCode::cFNeg>();
        }

        return true;
    }

    // Push the child node, then emit the bitwise not operation.
    if (visit(node->mExpr) == false)
    {
        return false;
    }

    emit<OpCode::cINot>();

    return true;
}

bool CodeGenVisitor::visitBinaryOp(BinaryOpNode* node)
{
    // If this is || or &&, handle it differently with short-circuiting.
    if (node->mOp == BinaryOp::cAnd || node->mOp == BinaryOp::cOr)
    {
        return emitBoolAsValue(node);
    }

    Type* leftType = node->mLeft->mResolvedType;
    Type* rightType = node->mRight->mResolvedType;

    // Equality is defined for more types, so handle these here.
    if (node->mOp == BinaryOp::cEQ || node->mOp == BinaryOp::cNE)
    {
        if (leftType->mKind == TypeKind::cFunction || leftType->mKind == TypeKind::cStruct)
        {
            return emitEqualityOperands(node);
        }

        if (leftType->mKind == TypeKind::cInterface && rightType->mKind == TypeKind::cInterface)
        {
            return emitInterfaceEqualityComparison(node->mOp, node->mLeft, node->mRight);
        }

        // This one is awkward, but interfaces are [objectHandle, interfaceTableIndex].
        // So if we're comparing to null, we need to make the interface expression an object reference.
        // (This is done via pop to remove the table index from the stack.)
        if ((leftType->mKind == TypeKind::cInterface || rightType->mKind == TypeKind::cInterface) &&
            (leftType->mKind == TypeKind::cNull || rightType->mKind == TypeKind::cNull))
        {
            ExpressionNode* interfaceExpr = leftType->mKind == TypeKind::cInterface ? node->mLeft : node->mRight;
            if (emitInterfaceObjectRef(interfaceExpr) == false)
            {
                return false;
            }

            emitIntegerImmediate(cNullRef);

            return emitReferenceEqualityOpCode(node->mOp);
        }
    }

    // Otherwise, do normal stuff.
    // Push lhs and rhs.
    if (visit(node->mLeft) == false || visit(node->mRight) == false)
    {
        return false;
    }

    switch (node->mOp)
    {
        case BinaryOp::cEQ:
        case BinaryOp::cNE:
        case BinaryOp::cLT:
        case BinaryOp::cLE:
        case BinaryOp::cGT:
        case BinaryOp::cGE:
        {
            Type* lhsType = node->mLeft->mResolvedType;
            Type* rhsType = node->mRight->mResolvedType;

            // Reference comparisons operate on raw reference values.
            // These compare reference words, so we use integer comparison.
            if (lhsType->mKind == TypeKind::cNull || rhsType->mKind == TypeKind::cNull ||
                lhsType->mKind == TypeKind::cClass || rhsType->mKind == TypeKind::cClass ||
                lhsType->mKind == TypeKind::cList || rhsType->mKind == TypeKind::cList ||
                lhsType->mKind == TypeKind::cMap || rhsType->mKind == TypeKind::cMap)
            {
                // Only == and != are allowed here (enforced in type checker).
                return emitReferenceEqualityOpCode(node->mOp);
            }

            // Here we are interested in the operand kind (the result is always bool).
            PrimitiveTypeKind operandKind = getPrimitiveKind(lhsType);

            return emitComparisonOpCode(node->mOp, operandKind);
        }
        default:
        {
            // Here, the result is the resolved type that we have on the binary node.
            PrimitiveTypeKind resultKind = getPrimitiveKind(node->mResolvedType);
            return emitArithmeticOrBitwiseOpcode(node->mOp, resultKind);
        }
    }
}

bool CodeGenVisitor::visitTernaryExpr(TernaryExprNode* node)
{
    u32 trueLabel = makeLabel();
    u32 falseLabel = makeLabel();
    u32 endLabel = makeLabel();

    // Resolve the condition, leaving 0 or 1 on the stack.
    if (visitAsCondition(node->mCondition, trueLabel, falseLabel) == false)
    {
        return false;
    }

    // True label and expression.
    emit<OpCode::cLabel>(trueLabel);
    if (visit(node->mThenExpr) == false)
    {
        return false;
    }

    // Jump to the end label.
    emit<OpCode::cJump>(endLabel);

    // False label and expression.
    emit<OpCode::cLabel>(falseLabel);
    if (visit(node->mElseExpr) == false)
    {
        return false;
    }

    emit<OpCode::cLabel>(endLabel);

    return true;
}

} // namespace simlang
