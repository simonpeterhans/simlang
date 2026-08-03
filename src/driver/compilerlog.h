#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "util/types.h"

namespace simlang
{

class PhaseLogger;
class TextSink;

class CompilerLog
{
public:
    static constexpr std::string_view cPrefix = "[simlang] ";

    void setSink(TextSink* sink);
    void setEnabled(bool enabled);

    bool isEnabled() const;

    void info(std::string_view message);
    void addPhaseDetail(std::string_view phaseName, std::string detail);
    PhaseLogger getPhaseLogger(std::string_view name);

    void clearPhaseTotals();
    void emitPhaseTotals() const;

private:
    friend class PhaseLogger;

    void recordPhase(std::string_view name, double elapsedMs, bool success);

    struct PhaseTotal
    {
        std::string mName;
        double mElapsedMs = 0.0;
        usize mCount = 0;
        usize mFailedCount = 0;
        std::vector<std::string> mDetails;
    };

    TextSink* mSink = nullptr;
    PhaseLogger* mOpenPhase = nullptr;
    bool mEnabled = true;
    std::vector<PhaseTotal> mPhaseTotals;
};

} // namespace simlang
