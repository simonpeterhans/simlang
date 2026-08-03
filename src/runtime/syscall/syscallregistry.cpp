#include "runtime/syscall/syscallregistry.h"

#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "runtime/vmdefines.h"
#include "sema/scopes.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symbolregistry.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "type/typetable.h"

namespace simlang
{

static u32 getSyscallStackWords(SyscallRegistry::SyscallType type)
{
    return type.mType == NativeType::cVoid ? 0U : 1U;
}

Type* SyscallRegistry::getSyscallType(CompilerContext& ctx, SyscallType type)
{
    switch (type.mType)
    {
        case NativeType::cVoid: return ctx.mTypes.getPrimitiveType(PrimitiveTypeKind::cVoid);
        case NativeType::cInt: return ctx.mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
        case NativeType::cFloat: return ctx.mTypes.getPrimitiveType(PrimitiveTypeKind::cFloat);
        case NativeType::cBool: return ctx.mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
        case NativeType::cString: return ctx.mTypes.getPrimitiveType(PrimitiveTypeKind::cString);
        case NativeType::cListRef:
        case NativeType::cListResult:
        {
            Type* elementType = getSyscallType(ctx, SyscallType{type.mElementType, NativeType::cInvalid});
            if (elementType == nullptr)
            {
                return nullptr;
            }
            return ctx.mTypes.getOrAddList(elementType);
        }
        case NativeType::cMapRef:
        case NativeType::cMapResult:
        {
            Type* keyType = getSyscallType(ctx, SyscallType{type.mKeyType, NativeType::cInvalid});
            Type* valueType = getSyscallType(ctx, SyscallType{type.mValueType, NativeType::cInvalid});
            if (keyType == nullptr || valueType == nullptr)
            {
                return nullptr;
            }
            return ctx.mTypes.getOrAddMap(keyType, valueType);
        }
        default:
        {
            return nullptr;
        }
    }
}

bool SyscallRegistry::registerSyscall(CompilerContext& ctx,
                                      Scope& targetScope,
                                      std::string_view name,
                                      const SyscallTemplate& templateData)
{
    // Make an identifier if it doesn't exist already.
    Identifier* identifier = ctx.internIdentifier(name, cInvalidSourceRange);
    if (identifier == nullptr)
    {
        return false;
    }

    if (targetScope.hasSymbol(identifier))
    {
        ctx.report<cSymbolAlreadyDefined>(cInvalidSourceRange, identifier);
        return false;
    }

    // We have a new function, look up the type and create it if it doesn't exist yet.
    TypeTable& tt = ctx.mTypes;

    Type* returnType = getSyscallType(ctx, templateData.mReturnType);
    if (returnType == nullptr)
    {
        return false;
    }

    std::vector<FunctionParam> paramTypes;
    paramTypes.reserve(templateData.mParamTypes.size());

    u32 argWords = 0;
    for (const SyscallType& pt : templateData.mParamTypes)
    {
        Type* paramType = getSyscallType(ctx, pt);
        if (paramType == nullptr)
        {
            return false;
        }

        argWords += getSyscallStackWords(pt);
        paramTypes.push_back(FunctionParam{paramType, false});
    }

    if (argWords > cMaxOpWordCount)
    {
        ctx.report<cFunctionFrameTooLarge>(cInvalidSourceRange, argWords, "syscall argument", cMaxOpWordCount);
        return false;
    }

    u32 returnWords = getSyscallStackWords(templateData.mReturnType);
    if (returnWords > cMaxReturnWords)
    {
        ctx.report<cFunctionFrameTooLarge>(cInvalidSourceRange, returnWords, "syscall return", cMaxReturnWords);
        return false;
    }

    FunctionType* functionType = tt.getOrAddFunction(returnType, paramTypes);

    // ID.
    usize rawID = mSyscalls.size();

    SyscallIdx id = static_cast<SyscallIdx>(rawID);

    SyscallEntry entry;
    entry.mID = id;
    entry.mFunction = templateData.mFunction;
    entry.mCaller = templateData.mCaller;
    entry.mArgWords = static_cast<OpWordCount>(argWords);
    entry.mReturnWords = static_cast<ReturnWordCount>(returnWords);

    mSyscalls.push_back(entry);

    // Create the syscall symbol.
    Symbol* s = ctx.mSymbols.createSymbol(SymbolType::cSyscall);
    s->mIdentifier = identifier;
    s->mType = functionType;
    s->mIndex = static_cast<i32>(id);

    targetScope.addSymbol(s);

    return true;
}

} // namespace simlang
