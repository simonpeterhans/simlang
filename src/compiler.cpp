#include "compiler.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <ratio>
#include <sstream>
#include <string_view>
#include <utility>

#include "backend/backendstate.h"
#include "compileroptions.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/backenddriver.h"
#include "driver/compilercontext.h"
#include "driver/compilerlog.h"
#include "driver/frontenddriver.h"
#include "runtime/executableimage.h"
#include "sema/scopes.h"
#include "source/sourcerange.h"
#include "symbol/constvalue.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symbolregistry.h"
#include "symbol/symboltype.h"
#include "type/typekind.h"
#include "type/types.h"
#include "type/typetable.h"
#include "util/arena.h"
#include "util/flags.h"

namespace simlang
{

struct InternedString;

Compiler::Compiler()
    : mCtx(std::make_unique<CompilerContext>())
{
    registerPrimitives();
}

Compiler::Compiler(TextSink& output)
    : mCtx(std::make_unique<CompilerContext>(output))
{
    registerPrimitives();
}

Compiler::~Compiler() = default;

static Scope* getOrCreateHostModule(CompilerContext& ctx, std::string_view name)
{
    Identifier* id = ctx.internIdentifier(name, cInvalidSourceRange);
    if (id == nullptr)
    {
        return nullptr;
    }

    Scope* rootScope = ctx.mScopes.getRootScope();
    Symbol* existing = rootScope->getSymbol(id);
    if (existing != nullptr)
    {
        if (existing->mSymbolType == SymbolType::cModule && existing->mScope != nullptr)
        {
            return existing->mScope;
        }

        ctx.report<cSymbolAlreadyDefined>(cInvalidSourceRange, id);
        return nullptr;
    }

    Symbol* module = ctx.mSymbols.createSymbol(SymbolType::cModule);
    module->mIdentifier = id;
    module->mScope = ctx.mScopes.createScope(nullptr);

    rootScope->addSymbol(module);

    return module->mScope;
}

static Scope* getHostScope(CompilerContext& ctx, std::string_view moduleName)
{
    if (moduleName.empty())
    {
        return ctx.mScopes.getRootScope();
    }

    return getOrCreateHostModule(ctx, moduleName);
}

void Compiler::registerPrimitives()
{
    for (PrimitiveTypeKind ptk : cPrimitiveTypeKinds)
    {
        // Register the identifier.
        Identifier* id = mCtx->internIdentifier(primitiveTypeKindToString(ptk), cInvalidSourceRange);

        // Create a type for it.
        // For now we just fetch our static type directly.
        Type* t = mCtx->mTypes.getPrimitiveType(ptk);

        // Create a symbol and bind the type and identifier to it.
        Symbol* s = mCtx->mSymbols.createSymbol(SymbolType::cPrimitive);
        s->mIdentifier = id;
        s->mType = t;

        // Add to global scope.
        mCtx->mScopes.addSymbol(s);
    }
}

bool Compiler::registerConst(std::string_view moduleName, std::string_view name, NativeValue val)
{
    Scope* scope = getHostScope(*mCtx, moduleName);
    if (scope == nullptr)
    {
        return false;
    }

    Identifier* id = mCtx->internIdentifier(name, cInvalidSourceRange);
    if (id == nullptr)
    {
        return false;
    }

    if (scope->hasSymbol(id))
    {
        mCtx->report<cSymbolAlreadyDefined>(cInvalidSourceRange, id);
        return false;
    }

    Type* t = nullptr;
    ConstValue constValue;
    switch (val.mType)
    {
        case NativeType::cInt:
        {
            t = mCtx->mTypes.getPrimitiveType(PrimitiveTypeKind::cInt);
            constValue = ConstValue::makeInteger(val.as.mInteger);

            break;
        }
        case NativeType::cFloat:
        {
            t = mCtx->mTypes.getPrimitiveType(PrimitiveTypeKind::cFloat);
            constValue = ConstValue::makeFloat(val.as.mFloat);

            break;
        }
        case NativeType::cBool:
        {
            t = mCtx->mTypes.getPrimitiveType(PrimitiveTypeKind::cBool);
            constValue = ConstValue::makeBool(val.as.mBool);

            break;
        }
        case NativeType::cString:
        {
            if (val.as.mString == nullptr)
            {
                return false;
            }

            t = mCtx->mTypes.getPrimitiveType(PrimitiveTypeKind::cString);

            std::string_view stringValue{val.as.mString};
            const InternedString* internedStr = mCtx->internString(stringValue, cInvalidSourceRange);
            if (internedStr == nullptr)
            {
                return false;
            }

            constValue = ConstValue::makeString(internedStr);

            break;
        }
        default:
        {
            return false;
        }
    }

    Symbol* s = mCtx->mSymbols.createSymbol(SymbolType::cGlobalVariable);
    s->mIdentifier = id;
    s->mFlags.set(SymbolFlags::cMutable, false);
    s->mFlags.set(SymbolFlags::cConstExpr, true);
    s->mConstEvalState = ConstEvalState::cReady;
    s->mType = t;
    s->mConstValue = constValue;

    scope->addSymbol(s);

    return true;
}

bool Compiler::registerSyscall(std::string_view moduleName,
                               std::string_view name,
                               const SyscallRegistry::SyscallTemplate& syscall)
{
    Scope* scope = getHostScope(*mCtx, moduleName);
    if (scope == nullptr)
    {
        return false;
    }

    return mCtx->mSyscalls.registerSyscall(*mCtx, *scope, name, syscall);
}

void Compiler::emitDiagnostics() const
{
    mCtx->mDiag.emit();
}

std::unique_ptr<ExecutableImage> Compiler::compile(const CompilerOptions& options)
{
    auto compileStart = std::chrono::steady_clock::now();

    FrontendDriver frontendDriver{*mCtx};
    BackendDriver backendDriver{*mCtx};
    backendDriver.setOptimize(options.mOptimize);
    backendDriver.setBytecodeDumpOptions(options.mBytecodeDump);
    mCtx->mLog.setSink(options.mLogSink);
    mCtx->mLog.setEnabled(options.mPrintSummary);
    mCtx->mLog.clearPhaseTotals();

    auto emitSummary = [this, compileStart](bool success)
    {
        auto compileEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double, std::milli>(compileEnd - compileStart).count();

        mCtx->mLog.emitPhaseTotals();
        if (mCtx->mLog.isEnabled() == false)
        {
            return;
        }

        std::ostringstream arenaMessage;
        arenaMessage << "Arena memory: " << mCtx->mAllocator.getAllocatedBytes() << " bytes allocated, ";
        arenaMessage << mCtx->mAllocator.getReservedBytes() << " bytes reserved, ";
        arenaMessage << mCtx->mAllocator.getBlockCount() << " blocks.";

        mCtx->mLog.info(arenaMessage.str());

        std::ostringstream message;
        message << (success ? "Compilation finished in " : "Compilation failed in ");
        message << std::fixed << std::setprecision(3) << elapsed << " ms.";

        mCtx->mLog.info(message.str());
    };

    if (options.mRootPath.empty() == false)
    {
        frontendDriver.setRoot(options.mRootPath);
    }

    if (frontendDriver.setSource(options.mSourcePath) == false)
    {
        emitSummary(false);
        return nullptr;
    }

    if (frontendDriver.run() == false)
    {
        emitSummary(false);
        return nullptr;
    }

    if (backendDriver.buildCode() == false)
    {
        emitSummary(false);
        return nullptr;
    }

    emitSummary(true);

    // Create the executable image by stealing the data from the backend state.
    return std::make_unique<ExecutableImage>(std::move(mCtx->mBackend).toExecutableImage());
}

} // namespace simlang
