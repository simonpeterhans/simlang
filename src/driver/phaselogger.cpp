#include "driver/phaselogger.h"

#include <chrono>
#include <ratio>

#include "driver/compilerlog.h"

namespace simlang
{

PhaseLogger::PhaseLogger(CompilerLog& log, std::string_view name)
    : mLog(log)
{
    // Find out if logging is enabled at all.
    mRecordsPhase = mLog.isEnabled();
    if (mRecordsPhase == false)
    {
        return;
    }

    mName = name;

    // If this is a nested phase, we need to track that.
    // Store the currently active open phase.
    mPreviousOpenPhase = mLog.mOpenPhase;
    // Register ourselves as the new open phase.
    mLog.mOpenPhase = this;

    // Finally start.
    mStart = std::chrono::steady_clock::now();
}

PhaseLogger::~PhaseLogger()
{
    if (mFinished == false)
    {
        finish(false);
    }
}

bool PhaseLogger::finish(bool success)
{
    // Only finish once.
    if (mFinished)
    {
        return success;
    }

    mFinished = true;

    // If we're supposed to do nothing, bail.
    if (mRecordsPhase == false)
    {
        return success;
    }

    // Get the end time.
    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - mStart).count();

    // Now subtract any nested time.
    double exclusiveElapsed = elapsed - mNestedElapsedMs;
    if (exclusiveElapsed < 0.0)
    {
        exclusiveElapsed = 0.0;
    }

    // Tell the logger about our result.
    mLog.recordPhase(mName, exclusiveElapsed, success);

    if (mPreviousOpenPhase != nullptr)
    {
        // If we're a nested phase, subtract our time from the previous open phase.
        // That way, our time here does not get added on top of the parent phase.
        mPreviousOpenPhase->mNestedElapsedMs += elapsed;
    }

    // Since we're done, set the previous open phase as the new open phase.
    mLog.mOpenPhase = mPreviousOpenPhase;

    return success;
}

} // namespace simlang
