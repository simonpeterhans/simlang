#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace simlang
{

class CompilerLog;

class PhaseLogger
{
public:
    explicit PhaseLogger(CompilerLog& log, std::string_view name);
    ~PhaseLogger();

    // Don't allow any copies or moves.
    PhaseLogger(const PhaseLogger&) = delete;
    PhaseLogger& operator=(const PhaseLogger&) = delete;
    PhaseLogger(PhaseLogger&&) = delete;
    PhaseLogger& operator=(PhaseLogger&&) = delete;

    bool finish(bool success);

private:
    friend class CompilerLog;

    CompilerLog& mLog;
    std::string mName;
    std::chrono::steady_clock::time_point mStart;
    PhaseLogger* mPreviousOpenPhase = nullptr;
    double mNestedElapsedMs = 0.0;
    bool mRecordsPhase = false;
    bool mFinished = false;
};

} // namespace simlang
