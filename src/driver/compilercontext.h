#pragma once

#include <string_view>
#include <utility>

#include "backend/backendstate.h"
#include "diag/diagnosticmanager.h"
#include "driver/compilerlog.h"
#include "module/modulemanager.h"
#include "parser/parsercontext.h"
#include "runtime/syscall/syscallregistry.h"
#include "sema/scopes.h"
#include "sema/templateinstantiationcache.h"
#include "source/sourcemanager.h"
#include "symbol/identifiertable.h"
#include "symbol/interning.h"
#include "symbol/stringtable.h"
#include "symbol/symbolregistry.h"
#include "type/typetable.h"
#include "util/arena.h"
#include "util/textsinks.h"

namespace simlang
{

struct CompilerContext
{
    // Make sure the order here aligns with the declaration order.
    CompilerContext()
        : mSources()
        , mDefaultOutput(stdout)
        , mDiag(mSources, mDefaultOutput)
        , mIdentifiers(mAllocator)
        , mStrings(mAllocator)
        , mSymbols(mAllocator)
        , mTypes(mAllocator)
        , mModules(mDiag, mIdentifiers, mSymbols)
    {
    }

    explicit CompilerContext(TextSink& output)
        : mSources()
        , mDefaultOutput(stdout)
        , mDiag(mSources, output)
        , mIdentifiers(mAllocator)
        , mStrings(mAllocator)
        , mSymbols(mAllocator)
        , mTypes(mAllocator)
        , mModules(mDiag, mIdentifiers, mSymbols)
    {
    }

    template <typename T, typename... Args>
    T* allocate(Args&&... args)
    {
        return mAllocator.create<T>(std::forward<Args>(args)...);
    }

    template <DiagnosticType DT, typename... Args>
    DiagnosticBuilder report(SourceRange range, Args&&... args)
    {
        return mDiag.report<DT>(range, std::forward<Args>(args)...);
    }

    Identifier* internIdentifier(std::string_view name, SourceRange range)
    {
        return intern::addIdentifier(mDiag, mIdentifiers, name, range);
    }

    const InternedString* internString(std::string_view value, SourceRange range)
    {
        return intern::addString(mDiag, mStrings, value, range);
    }

    ParserContext makeParserContext() { return ParserContext{mAllocator, mDiag, mIdentifiers, mStrings}; }

    ArenaAllocator mAllocator;

    // Make sure the order here aligns with the constructor order.
    // Shared stuff.
    SourceManager mSources;
    FileTextSink mDefaultOutput;
    CompilerLog mLog;
    DiagnosticManager mDiag;

    IdentifierTable mIdentifiers;
    StringTable mStrings;
    SymbolRegistry mSymbols;
    TypeTable mTypes;

    // Front-end stuff.
    ScopeManager mScopes;
    ModuleManager mModules;
    TemplateInstantiationCache mTemplateCache;

    // Shared host/runtime binding stuff.
    SyscallRegistry mSyscalls;

    // Back-end stuff.
    BackendState mBackend;
};

} // namespace simlang
