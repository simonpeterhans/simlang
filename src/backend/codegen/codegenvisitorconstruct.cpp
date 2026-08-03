#include "ast/nodes/exprnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/typeidutils.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "symbol/symbol.h"
#include "type/types.h"
#include "util/arrayview.h"

namespace simlang
{

bool CodeGenVisitor::emitNewList(NewObjectNode*, ListType* listType)
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

bool CodeGenVisitor::emitNewMap(NewObjectNode*, MapType* mapType)
{
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

bool CodeGenVisitor::emitNewClass(NewObjectNode* node, AggregateType* classType)
{
    // Find out what type we want to reserve memory for on the heap.
    TypeID typeID;
    if (getRuntimeTypeID(classType, typeID) == false)
    {
        return false;
    }

    // Create the class instance on the heap.
    emit<OpCode::cNewObject>(typeID);

    // Duplicate the address since it'll serve as arg for the init call.
    emit<OpCode::cDup>();

    // Emit the args.
    Symbol* initializerSymbol = node->mInitMethodSymbol;
    auto* funcType = static_cast<FunctionType*>(node->mInitMethodSymbol->mType);
    if (emitCallArguments(node->mInitializerArguments, funcType) == false)
    {
        return false;
    }

    // Emit the call.
    emit<OpCode::cCallMethod>(static_cast<FunctionIdx>(initializerSymbol->mIndex));

    return true;
}

bool CodeGenVisitor::emitNewStruct(NewObjectNode* node, AggregateType* structType)
{
    // Initialize all the fields.
    const AggregateLayout* layout = structType->mLayout;
    for (const FieldLayout& field : layout->mFields)
    {
        ExpressionNode* value = nullptr;
        for (FieldInitializer* init : node->mFieldInitializers)
        {
            if (init->mResolvedField == field.mSymbol)
            {
                value = init->mValue;
                break;
            }
        }

        // If the value was not found, use the default initializer.
        if (value == nullptr)
        {
            // This should never be null since we checked it before.
            auto* decl = static_cast<VariableDeclarationStatementNode*>(field.mSymbol->mDeclNode);
            value = decl->mInit;
        }

        // Directly push the value onto the stack.
        if (visit(value) == false)
        {
            return false;
        }
    }

    return true;
}

} // namespace simlang
