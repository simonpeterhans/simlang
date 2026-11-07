#include "ast/nodes/exprnodes.h"
#include "backend/backendstate.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/typeidutils.h"
#include "driver/compilercontext.h"
#include "runtime/op/opcode.h"
#include "runtime/vmdefines.h"
#include "symbol/symbol.h"
#include "type/types.h"

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

bool CodeGenVisitor::emitNewClass(NewObjectNode* node, AggregateType* classType)
{
    TypeID typeID;
    if (getRuntimeTypeID(classType, typeID) == false)
    {
        return false;
    }

    // Create the class instance on the heap.
    emit<OpCode::cNewObject>(typeID);

    // Duplicate the address since we need it as arg for the constructor call, and then for the expression of the "new".
    emit<OpCode::cDup>();

    Symbol* constructorSymbol = node->mConstructorSymbol;
    if (emitCallArguments(node->mArgs, static_cast<FunctionType*>(constructorSymbol->mType)) == false)
    {
        return false;
    }

    // Emit the call.
    emit<OpCode::cCallMethod>(static_cast<FunctionIdx>(constructorSymbol->mIndex));

    return true;
}

bool CodeGenVisitor::emitNewStruct(NewObjectNode* node, AggregateType* structType)
{
    // Push zeroes and the stack address of the first word.
    emit<OpCode::cNewStruct>(static_cast<OpWordCount>(structType->mLayout->mSize));

    // Emit all the args.
    Symbol* constructor = node->mConstructorSymbol;
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
