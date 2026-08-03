#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ratio>
#include <string>
#include <string_view>

#include "backend/bytecode/bytecodedumpoptions.h"
#include "compiler.h"
#include "compileroptions.h"
#include "runtime/executableimage.h"
#include "runtime/runtimeerror.h"
#include "runtime/runtimeerrorsink.h"
#include "runtime/vm/vm.h"
#include "util/textsink.h"
#include "util/textsinks.h"
#include "util/textwriter.h"

namespace
{

struct CLIOptions
{
    std::filesystem::path mSourcePath;
    std::filesystem::path mRootPath;
    std::filesystem::path mDumpPath;
    simlang::RuntimeErrorMode mRuntimeErrorMode = simlang::RuntimeErrorMode::cLogAndContinue;

    bool mRun = true;
    bool mOptimize = true;
    bool mPrintSummary = true;
    bool mPrintTime = false;
    bool mWriteDump = false;
    bool mShowHelp = false;
};

class FileOutputSink : public simlang::TextSink
{
public:
    explicit FileOutputSink(const std::filesystem::path& path)
        : mStream(path, std::ios::out | std::ios::trunc)
    {
    }

    bool isOpen() const { return mStream.is_open(); }

    void write(std::string_view text) override
    {
        if (text.empty())
        {
            return;
        }

        mStream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    void flush() override { mStream.flush(); }

private:
    std::ofstream mStream;
};

bool startsWith(std::string_view text, std::string_view prefix)
{
    return (text.size() >= prefix.size()) && (text.substr(0, prefix.size()) == prefix);
}

std::filesystem::path pathFromArg(std::string_view value)
{
    return std::filesystem::path{std::string{value}};
}

bool readOptionValue(int& index, int argc, char** argv, std::string_view& outValue)
{
    if (index + 1 >= argc)
    {
        return false;
    }

    ++index;
    outValue = argv[index];

    return true;
}

bool parseRuntimeErrorMode(std::string_view value, simlang::RuntimeErrorMode& outMode)
{
    if (value == "halt" || value == "halt-on-error")
    {
        outMode = simlang::RuntimeErrorMode::cHaltOnError;
        return true;
    }

    if (value == "continue" || value == "log-and-continue")
    {
        outMode = simlang::RuntimeErrorMode::cLogAndContinue;
        return true;
    }

    return false;
}

void printUsage(simlang::TextWriter& out)
{
    out.writeLine("Usage:");
    out.writeLine("  simlang-cli [options] <source.sim>");
    out.writeLine();
    out.writeLine("Options:");
    out.writeLine("  -h, --help                 Show this help.");
    out.writeLine("  --root PATH                Resolve imports relative to PATH.");
    out.writeLine("  --dump PATH                Write a bytecode dump using the default dump options.");
    out.writeLine("  --check                    Compile only; do not run the program.");
    out.writeLine("  --run                      Run the program after compiling. This is the default.");
    out.writeLine("  --no-optimize              Disable bytecode optimization.");
    out.writeLine("  --quiet                    Suppress the compilation summary.");
    out.writeLine("  --time                     Print VM execution time to stderr.");
    out.writeLine("  --runtime-errors MODE      Runtime error mode: halt or continue. Default: continue.");
}

bool parseArgs(int argc, char** argv, CLIOptions& options, simlang::TextWriter& err)
{
    bool stopOptions = false;

    for (int i = 1; i < argc; ++i)
    {
        // Check if we have reached the delimiter for positional input.
        std::string_view arg{argv[i]};
        if (stopOptions == false && arg == "--")
        {
            stopOptions = true;
            continue;
        }

        if (stopOptions == false && (arg == "-h" || arg == "--help"))
        {
            options.mShowHelp = true;
            return true;
        }

        // Compile without running.
        if (stopOptions == false && arg == "--check")
        {
            options.mRun = false;
            continue;
        }

        if (stopOptions == false && arg == "--run")
        {
            options.mRun = true;
            continue;
        }

        if (stopOptions == false && arg == "--no-optimize")
        {
            options.mOptimize = false;
            continue;
        }

        // No compilation summary.
        if (stopOptions == false && arg == "--quiet")
        {
            options.mPrintSummary = false;
            continue;
        }

        // Print the total execution time of the script.
        if (stopOptions == false && arg == "--time")
        {
            options.mPrintTime = true;
            continue;
        }

        // Set the root path.
        std::string_view value;
        if (stopOptions == false && (arg == "--root" || startsWith(arg, "--root=")))
        {
            if (arg == "--root")
            {
                if (readOptionValue(i, argc, argv, value) == false)
                {
                    err.writeLine("error: --root needs a path.");
                    return false;
                }
            }
            else
            {
                value = arg.substr(std::string_view{"--root="}.size());
            }

            options.mRootPath = pathFromArg(value);
            continue;
        }

        // Whether (and where) to dump the compiler data at the end of compilation.
        if (stopOptions == false && (arg == "--dump" || startsWith(arg, "--dump=")))
        {
            if (arg == "--dump")
            {
                if (readOptionValue(i, argc, argv, value) == false)
                {
                    err.writeLine("error: --dump needs a path.");
                    return false;
                }
            }
            else
            {
                value = arg.substr(std::string_view{"--dump="}.size());
            }

            options.mDumpPath = pathFromArg(value);
            options.mWriteDump = true;
            continue;
        }

        // What runtime error mode to use.
        if (stopOptions == false && (arg == "--runtime-errors" || startsWith(arg, "--runtime-errors=")))
        {
            if (arg == "--runtime-errors")
            {
                if (readOptionValue(i, argc, argv, value) == false)
                {
                    err.writeLine("error: --runtime-errors needs a mode.");
                    return false;
                }
            }
            else
            {
                value = arg.substr(std::string_view{"--runtime-errors="}.size());
            }

            if (parseRuntimeErrorMode(value, options.mRuntimeErrorMode) == false)
            {
                err.writeLine("error: --runtime-errors must be 'halt' or 'continue'.");
                return false;
            }

            continue;
        }

        if (stopOptions == false && arg.empty() == false && arg[0] == '-')
        {
            err << "error: unknown option '" << arg << "'.\n";
            return false;
        }

        // Positional stuff (only 1 source file is allowed).
        if (options.mSourcePath.empty() == false)
        {
            err.writeLine("error: only one source file can be compiled at a time.");
            return false;
        }

        options.mSourcePath = pathFromArg(arg);
    }

    // If we only show help, we can bail.
    if (options.mShowHelp)
    {
        return true;
    }

    // Otherwise, expect a source.
    if (options.mSourcePath.empty())
    {
        err.writeLine("error: no source file specified.");
        return false;
    }

    return true;
}

std::unique_ptr<simlang::ExecutableImage> compileSource(const CLIOptions& cliOptions,
                                                        simlang::TextSink& diagnostics,
                                                        simlang::TextSink* dumpSink)
{
    simlang::Compiler compiler{diagnostics};

    simlang::CompilerOptions compilerOptions;
    compilerOptions.mSourcePath = cliOptions.mSourcePath;
    compilerOptions.mRootPath = cliOptions.mRootPath;
    compilerOptions.mOptimize = cliOptions.mOptimize;
    compilerOptions.mPrintSummary = cliOptions.mPrintSummary;
    compilerOptions.mLogSink = cliOptions.mPrintSummary ? &diagnostics : nullptr;
    compilerOptions.mBytecodeDump.mSink = dumpSink;

    // Compile and print diag.
    std::unique_ptr<simlang::ExecutableImage> executable = compiler.compile(compilerOptions);
    compiler.emitDiagnostics();

    return executable;
}

bool runExecutable(const simlang::ExecutableImage& executable,
                   const CLIOptions& options,
                   simlang::TextSink& output,
                   simlang::TextSink& diagnostics)
{
    simlang::TextRuntimeErrorSink runtimeErrors{diagnostics};

    simlang::VM vm{executable};
    vm.setOutput(&output);
    vm.setRuntimeErrorSink(&runtimeErrors);
    vm.setRuntimeErrorMode(options.mRuntimeErrorMode);

    auto start = std::chrono::steady_clock::now();
    bool success = vm.run();
    auto end = std::chrono::steady_clock::now();

    if (options.mPrintTime)
    {
        simlang::TextWriter err{diagnostics};
        auto elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
        err << "Execution finished in " << elapsedMs << " ms.\n";
    }

    return success && vm.hasRuntimeErrors() == false;
}

} // namespace

int main(int argc, char** argv)
{
    simlang::FileTextSink stdoutSink{stdout};
    simlang::FileTextSink stderrSink{stderr};
    simlang::TextWriter out{stdoutSink};
    simlang::TextWriter err{stderrSink};

    CLIOptions options;
    if (parseArgs(argc, argv, options, err) == false)
    {
        err.writeLine();
        printUsage(err);
        return 1;
    }

    if (options.mShowHelp)
    {
        printUsage(out);
        return 0;
    }

    std::unique_ptr<FileOutputSink> dumpSink;
    if (options.mWriteDump)
    {
        dumpSink = std::make_unique<FileOutputSink>(options.mDumpPath);
        if (dumpSink->isOpen() == false)
        {
            err << "error: failed to open bytecode dump path '" << options.mDumpPath.string() << "'.\n";
            return 1;
        }
    }

    std::unique_ptr<simlang::ExecutableImage> executable = compileSource(options, stderrSink, dumpSink.get());
    if (executable == nullptr)
    {
        return 1;
    }

    if (options.mRun == false)
    {
        return 0;
    }

    if (runExecutable(*executable, options, stdoutSink, stderrSink) == false)
    {
        return 1;
    }

    return 0;
}
