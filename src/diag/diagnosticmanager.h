#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include "diag/diagnostic.h"
#include "diag/diagnosticformat.h"
#include "diag/diagnosticinfo.h"
#include "diag/diagnosticlevel.h"
#include "source/sourcerange.h"
#include "util/types.h"

namespace simlang
{

class DiagnosticManager;
class SourceManager;
class TextSink;
class TextWriter;
enum DiagnosticType : u8;

class DiagnosticCheckpoint
{
public:
    DiagnosticCheckpoint() = default;

private:
    friend class DiagnosticManager;

    explicit DiagnosticCheckpoint(usize errorCount)
        : mErrorCount(errorCount)
    {
    }

    usize mErrorCount = 0;
};

class DiagnosticBuilder
{
public:
    explicit DiagnosticBuilder(DiagnosticManager& manager);

    template <DiagnosticType DT, typename... Args>
    DiagnosticBuilder& note(SourceRange range, Args&&... args);

    DiagnosticBuilder& note(SourceRange range, std::string_view message);

    template <DiagnosticType DT, typename... Args>
    DiagnosticBuilder& hint(SourceRange range, Args&&... args);

    DiagnosticBuilder& hint(SourceRange range, std::string_view message);

private:
    friend class DiagnosticManager;

    DiagnosticManager& mManager;
};

class DiagnosticManager
{
public:
    explicit DiagnosticManager(const SourceManager& sm, TextSink& output);

    template <DiagnosticType DT, typename... Args>
    DiagnosticBuilder report(SourceRange range, Args&&... args)
    {
        appendDiagnostic<DT>(range, false, std::forward<Args>(args)...);
        return DiagnosticBuilder{*this};
    }

    usize getErrorCount() const { return mErrorCount; }
    DiagnosticCheckpoint createCheckpoint() const { return DiagnosticCheckpoint{getErrorCount()}; }
    bool hasErrorsSince(DiagnosticCheckpoint checkpoint) const { return getErrorCount() != checkpoint.mErrorCount; }
    bool hasNoErrorsSince(DiagnosticCheckpoint checkpoint) const { return hasErrorsSince(checkpoint) == false; }

    void emit() const;
    void emitAndClear();

private:
    friend class DiagnosticBuilder;

    template <DiagnosticType DT, typename... Args>
    void appendDiagnostic(SourceRange range, bool attached, Args&&... args)
    {
        constexpr usize N = sizeof...(Args);
        static_assert((N == getFormatForType(DT).mPlaceholderCount),
                      "Diagnostic format called with incorrect number of parameters!");

        mDiagnostics.emplace_back();

        const DiagnosticInfo& info = cDiagnosticTypeInfos[DT];
        Diagnostic& d = mDiagnostics.back();
        d.mType = DT;
        d.mSourceRange = range;
        d.mLevel = info.mLevel;
        d.mDescription = info.mDescription;
        d.mFormat = info.mFormat.mText;
        d.mAttached = attached;
        d.addParams(std::forward<Args>(args)...);

        // If this is an error, track that.
        if (d.mLevel == DiagnosticLevel::cError)
        {
            ++mErrorCount;
        }
    }

    void emit(TextWriter& out, const Diagnostic& diag, bool attached) const;

    std::vector<Diagnostic> mDiagnostics;
    const SourceManager& mSourceManager;
    TextSink& mOutput;
    usize mErrorCount = 0;
};

// We need the manager defined here, so the builder template definition follows here.

template <DiagnosticType DT, typename... Args>
DiagnosticBuilder& DiagnosticBuilder::note(SourceRange range, Args&&... args)
{
    static_assert(cDiagnosticTypeInfos[DT].mLevel == DiagnosticLevel::cNote,
                  "DiagnosticBuilder::note must be called with a note diagnostic type.");

    mManager.appendDiagnostic<DT>(range, true, std::forward<Args>(args)...);

    return *this;
}

template <DiagnosticType DT, typename... Args>
DiagnosticBuilder& DiagnosticBuilder::hint(SourceRange range, Args&&... args)
{
    static_assert(cDiagnosticTypeInfos[DT].mLevel == DiagnosticLevel::cHint,
                  "DiagnosticBuilder::hint must be called with a hint diagnostic type.");

    mManager.appendDiagnostic<DT>(range, true, std::forward<Args>(args)...);

    return *this;
}

} // namespace simlang
