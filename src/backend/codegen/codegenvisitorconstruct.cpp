#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/layout/layout.h"
#include "backend/typeidutils.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "sema/constructoranalysis.h"
#include "sema/passes/constevalvisitor.h"
#include "symbol/constvalue.h"
#include "symbol/symbol.h"
#include "type/types.h"
#include "util/bitutils.h"

namespace simlang
{

bool CodeGenVisitor::emitNewList(ListType* listType)
{
    // Get the element type.
    TypeID elementTypeID;
    if (getRuntimeTypeID(listType->mElement, elementTypeID) == false)
    {
        return false;
    }

    // That's already all we need to create the list.
    emit<OpCode::cNewList>(elementTypeID);

    return true;
}

bool CodeGenVisitor::emitNewMap(MapType* mapType)
{
    // Register the key type so that we know how it can be accessed subsequently.
    mCtx.mBackend.mKeyOpsTable.addType(mapType->mKey);

    // Get the k/v type pair.
    TypeID keyTypeID;
    TypeID valueTypeID;
    if (getRuntimeTypeID(mapType->mKey, keyTypeID) == false || getRuntimeTypeID(mapType->mValue, valueTypeID) == false)
    {
        return false;
    }

    // Emit the op.
    emit<OpCode::cNewMap>(keyTypeID, valueTypeID);

    return true;
}

bool CodeGenVisitor::emitConstructorConstant(const ConstValue& value, Type* type)
{
    switch (value.mKind)
    {
        case ConstValueKind::cStruct:
        {
            for (const ConstFieldValue& field : value.as.mStruct->mFields)
            {
                if (emitConstructorConstant(*field.mValue, field.mField->mType) == false)
                {
                    return false;
                }
            }
            break;
        }
        case ConstValueKind::cNull:
        {
            // Interfaces need both the object handle and the dispatch word.
            for (u32 i = 0; i < layout::getWordSizeForType(type); ++i)
            {
                emitIntegerImmediate(cNullRef);
            }
            break;
        }
        case ConstValueKind::cPrimitive:
        {
            switch (value.mPrimitiveKind)
            {
                case PrimitiveTypeKind::cInt: emitIntegerImmediate(value.as.mInteger); break;
                case PrimitiveTypeKind::cFloat: emit<OpCode::cPush32>(bits::bitCast<u32>(value.as.mFloat)); break;
                case PrimitiveTypeKind::cBool: emit<OpCode::cPush8>(static_cast<u8>(value.as.mBool)); break;
                case PrimitiveTypeKind::cString:
                {
                    StringLiteralNode literal{mBytecodeBuilder.getSourceRange(), value.as.mString};
                    return visitStringLiteral(&literal);
                }
                default:
                {
                    break;
                }
            }
            break;
        }
        default:
        {
            break;
        }
    }

    return true;
}

bool CodeGenVisitor::emitTrivialConstructorFields(NewObjectNode* node, const ConstructorAnalysis* analysis)
{
    for (const FieldInitialization& field : analysis->mFields)
    {
        if (field.mConstantValue != nullptr)
        {
            // If we have a const eval value, emit that.
            if (emitConstructorConstant(*field.mConstantValue, field.mField->mType) == false)
            {
                return false;
            }
        }
        else
        {
            // Otherwise, emit the expression.
            if (visit(node->mArgs[field.mParameterIndex].mValue) == false)
            {
                return false;
            }
        }
    }

    return true;
}

bool CodeGenVisitor::emitNewClass(NewObjectNode* node, AggregateType* classType)
{
    TypeID typeID;
    if (getRuntimeTypeID(classType, typeID) == false)
    {
        return false;
    }

    // Create the class instance on the heap.
    emit<OpCode::cNewObject>(typeID);

    // Duplicate the address since initialization consumes one copy, and the expression of the "new" needs the other.
    emit<OpCode::cDup>();

    Symbol* constructorSymbol = node->mConstructorSymbol;
    auto* declaration = static_cast<FunctionDeclarationStatementNode*>(constructorSymbol->mDeclNode);
    if (declaration->mIsTrivialConstructor)
    {
        if (emitTrivialConstructorFields(node, declaration->mConstructorAnalysis) == false)
        {
            return false;
        }

        // If the constructor is trivial, we have all members on the stack and can directly store them.
        emit<OpCode::cStoreObjFieldN>(static_cast<FieldOffset>(0), static_cast<OpWordCount>(classType->mLayout->mSize));
    }
    else
    {
        if (emitCallArguments(node->mArgs, static_cast<FunctionType*>(constructorSymbol->mType)) == false)
        {
            return false;
        }

        // Otherwise, emit the constructor call.
        emit<OpCode::cCallMethod>(static_cast<FunctionIdx>(constructorSymbol->mIndex));
    }

    return true;
}

bool CodeGenVisitor::emitNewStruct(NewObjectNode* node, AggregateType* structType)
{
    Symbol* constructor = node->mConstructorSymbol;
    auto* declaration = static_cast<FunctionDeclarationStatementNode*>(constructor->mDeclNode);
    if (declaration->mIsTrivialConstructor)
    {
        // If the constructor is trivial, we can directly push the member values.
        return emitTrivialConstructorFields(node, declaration->mConstructorAnalysis);
    }

    if (node->mFlags.test(cExprIsConstExpr))
    {
        // Constant analysis has already validated the complete construction, including its arguments.
        ConstValue value;
        ConstEvalVisitor evaluator{mCtx.mAllocator};
        evaluator.evaluate(node, value);
        return emitConstructorConstant(value, structType);
    }

    // Push zeroes and the stack address of the first word.
    emit<OpCode::cNewStruct>(static_cast<OpWordCount>(structType->mLayout->mSize));

    // Emit all the args.
    if (emitCallArguments(node->mArgs, static_cast<FunctionType*>(constructor->mType)) == false)
    {
        return false;
    }

    // Make the call to the constructor.
    // The address will be consumed and the initialized words we pushed earlier will remain on the stack.
    emit<OpCode::cCall>(static_cast<FunctionIdx>(constructor->mIndex));

    return true;
}

} // namespace simlang
