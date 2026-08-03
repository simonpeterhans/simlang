#include "driver/backenddriver.h"

#include <memory>
#include <string>
#include <vector>

#include "ast/nodes/translationunitnode.h"
#include "backend/backendstate.h"
#include "backend/bytecode/bytecodedump.h"
#include "backend/bytecode/bytecodeemitter.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "backend/bytecode/optimizer.h"
#include "backend/codegen/codegenvisitor.h"
#include "backend/layout/aggregatelayoutvisitor.h"
#include "backend/layout/constdatalayoutvisitor.h"
#include "backend/layout/symbollayoutvisitor.h"
#include "backend/layout/typelayouttablevisitor.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "driver/compilerlog.h"
#include "driver/phaselogger.h"
#include "module/moduleentry.h"
#include "module/modulemanager.h"
#include "runtime/op/op.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/syscall/syscallregistry.h"
#include "source/sourcerange.h"
#include "util/textwriter.h"
#include "util/types.h"

namespace simlang
{

BackendDriver::BackendDriver(CompilerContext& ctx)
    : mCtx(ctx)
{
}

bool BackendDriver::doAggregateLayout()
{
    static constexpr const char* cPhaseName = "Laying out aggregates";

    const std::vector<std::unique_ptr<ModuleEntry>>& modules = mCtx.mModules.getModules();
    AggregateLayoutVisitor visitor{mCtx};

    for (const auto& entry : modules)
    {
        auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

        if (visitor.run(entry->mAST) == false)
        {
            return phase.finish(false);
        }

        phase.finish(true);
    }

    return true;
}

bool BackendDriver::doSymbolLayout()
{
    static constexpr const char* cPhaseName = "Laying out symbols";

    const std::vector<std::unique_ptr<ModuleEntry>>& modules = mCtx.mModules.getModules();
    SymbolLayoutVisitor visitor{mCtx};

    for (const auto& entry : modules)
    {
        auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

        if (visitor.run(entry->mAST) == false)
        {
            return phase.finish(false);
        }

        phase.finish(true);
    }

    return true;
}

bool BackendDriver::doMainValidation()
{
    // For now, always require main.
    // Later we might want to allow code without main.
    // That would be convenient for an embedding engine that wants to call coroutines instead of a main.
    if (mCtx.mBackend.hasValidMain() == false)
    {
        mCtx.report<cMissingMainFunction>(cInvalidSourceRange);
        return false;
    }

    return true;
}

bool BackendDriver::doConstDataLayout()
{
    static constexpr const char* cPhaseName = "Laying out constants";

    const std::vector<std::unique_ptr<ModuleEntry>>& modules = mCtx.mModules.getModules();
    ConstDataLayoutVisitor visitor{mCtx};

    for (const auto& entry : modules)
    {
        auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

        if (visitor.run(entry->mAST) == false)
        {
            return phase.finish(false);
        }

        phase.finish(true);
    }

    return true;
}

bool BackendDriver::doTypeLayoutMetadata()
{
    static constexpr const char* cPhaseName = "Building type layout metadata";

    const std::vector<std::unique_ptr<ModuleEntry>>& modules = mCtx.mModules.getModules();
    TypeLayoutTableVisitor visitor{mCtx};

    for (const auto& entry : modules)
    {
        auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

        if (visitor.run(entry->mAST) == false)
        {
            return phase.finish(false);
        }

        phase.finish(true);
    }

    return true;
}

bool BackendDriver::doCodeGen()
{
    static constexpr const char* cPhaseName = "Generating bytecode";

    const std::vector<std::unique_ptr<ModuleEntry>>& modules = mCtx.mModules.getModules();
    CodeGenVisitor visitor{mCtx};

    for (const auto& entry : modules)
    {
        auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

        if (visitor.visit(entry->mAST) == false)
        {
            return phase.finish(false);
        }

        phase.finish(true);
    }

    if (mCtx.mLog.isEnabled())
    {
        usize bytecodeOpCount = mCtx.mBackend.mProgramBytecode.getEntryCode().getOps().size();
        for (const FunctionBytecode& function : mCtx.mBackend.mProgramBytecode.getFunctions())
        {
            bytecodeOpCount += function.getCode().getOps().size();
        }

        mCtx.mLog.addPhaseDetail(cPhaseName, "generated " + std::to_string(bytecodeOpCount) + " ops");
    }

    return true;
}

void BackendDriver::doOptimization()
{
    if (mOptimize)
    {
        static constexpr const char* cPhaseName = "Optimizing bytecode";

        auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

        Optimizer optimizer{mCtx.mBackend};
        OptimizerStats stats = optimizer.optimize();

        phase.finish(true);

        if (mCtx.mLog.isEnabled())
        {
            mCtx.mLog.addPhaseDetail(cPhaseName, "rewrote " + std::to_string(stats.mRewrittenOps) + " ops");
            mCtx.mLog.addPhaseDetail(cPhaseName, "removed " + std::to_string(stats.mRemovedOps) + " ops");
            mCtx.mLog.addPhaseDetail(cPhaseName, "saved " + std::to_string(stats.mBytesSaved) + " bytes");
        }
    }
}

bool BackendDriver::doEmitBytecode()
{
    static constexpr const char* cPhaseName = "Emitting bytecode";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    mCtx.mBackend.mSyscallInfos = mCtx.mSyscalls.getSyscalls();

    BytecodeEmitter bytecodeEmitter{mCtx};
    if (bytecodeEmitter.emit() == false)
    {
        return phase.finish(false);
    }

    bool success = phase.finish(true);

    if (mCtx.mLog.isEnabled())
    {
        mCtx.mLog.addPhaseDetail(cPhaseName,
                                 "emitted " + std::to_string(mCtx.mBackend.mBytes.size()) + " bytecode bytes");
    }
    return success;
}

void BackendDriver::doWriteDump()
{
    if (mBytecodeDumpOptions.mSink == nullptr)
    {
        return;
    }

    static constexpr const char* cPhaseName = "Writing bytecode dump";

    auto phase = mCtx.mLog.getPhaseLogger(cPhaseName);

    TextWriter out{*mBytecodeDumpOptions.mSink};
    BytecodeDump dump{out, mCtx, mBytecodeDumpOptions};

    dump.write();

    phase.finish(true);
}

bool BackendDriver::buildCode()
{
    if (doAggregateLayout() == false)
    {
        return false;
    }

    if (doSymbolLayout() == false)
    {
        return false;
    }

    if (doMainValidation() == false)
    {
        return false;
    }

    if (doConstDataLayout() == false)
    {
        return false;
    }

    if (doTypeLayoutMetadata() == false)
    {
        return false;
    }

    if (doCodeGen() == false)
    {
        return false;
    }

    doOptimization();

    if (doEmitBytecode() == false)
    {
        return false;
    }

    doWriteDump();

    return true;
}

} // namespace simlang
