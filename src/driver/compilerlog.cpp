#include "driver/compilerlog.h"

#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include "driver/phaselogger.h"
#include "util/textwriter.h"

namespace simlang
{

void CompilerLog::setSink(TextSink* sink)
{
    mSink = sink;
}

void CompilerLog::setEnabled(bool enabled)
{
    mEnabled = enabled;
}

bool CompilerLog::isEnabled() const
{
    return mEnabled && mSink != nullptr;
}

void CompilerLog::info(std::string_view message)
{
    if (isEnabled() == false)
    {
        return;
    }

    TextWriter out{*mSink};
    out << cPrefix << message << '\n';
    out.flush();
}

void CompilerLog::addPhaseDetail(std::string_view phaseName, std::string detail)
{
    // Adds more information to a phase name (usually indented, on a new line).
    if (isEnabled() == false || phaseName.empty() || detail.empty())
    {
        return;
    }

    // The name has to exist for that.
    for (PhaseTotal& total : mPhaseTotals)
    {
        if (total.mName == phaseName)
        {
            total.mDetails.push_back(std::move(detail));
            return;
        }
    }
}

PhaseLogger CompilerLog::getPhaseLogger(std::string_view name)
{
    return PhaseLogger{*this, name};
}

void CompilerLog::clearPhaseTotals()
{
    mOpenPhase = nullptr;
    mPhaseTotals.clear();
}

void CompilerLog::emitPhaseTotals() const
{
    if (isEnabled() == false)
    {
        return;
    }

    // Emits the aggregated totals for each phase.
    TextWriter out{*mSink};
    for (const PhaseTotal& total : mPhaseTotals)
    {
        std::ostringstream elapsed;
        elapsed << std::fixed << std::setprecision(3) << total.mElapsedMs;

        out << cPrefix << total.mName << ": done in " << elapsed.str() << " ms";
        if (total.mFailedCount > 0)
        {
            out << " (" << total.mFailedCount << " failed)";
        }

        out << '\n';

        for (const std::string& detail : total.mDetails)
        {
            out << cPrefix << "  " << detail << '\n';
        }
    }
    out.flush();
}

void CompilerLog::recordPhase(std::string_view name, double elapsedMs, bool success)
{
    // Callback for the phase loggers to report results.
    if (name.empty())
    {
        return;
    }

    // If the phase already exists, update it.
    for (PhaseTotal& total : mPhaseTotals)
    {
        if (total.mName == name)
        {
            total.mElapsedMs += elapsedMs;
            ++total.mCount;
            if (success == false)
            {
                ++total.mFailedCount;
            }
            return;
        }
    }

    // Otherwise, create a new entry.
    PhaseTotal& total = mPhaseTotals.emplace_back();
    total.mName = name;
    total.mElapsedMs = elapsedMs;
    total.mCount = 1;
    total.mFailedCount = success ? 0 : 1;
}

} // namespace simlang
