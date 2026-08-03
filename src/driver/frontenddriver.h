#pragma once

#include <filesystem>

#include "util/types.h"

namespace simlang
{

struct CompilerContext;
struct ModuleEntry;
enum class ModuleStage : u8;

class FrontendDriver
{
public:
    explicit FrontendDriver(CompilerContext& ctx);

    bool setSource(std::filesystem::path path) const;
    void setRoot(const std::filesystem::path& root) const;

    bool run();

private:
    bool processToStage(ModuleEntry* module, ModuleStage target);
    bool processAllToStage(ModuleStage target);

    bool doParse(ModuleEntry* module);
    bool doDeclarations(ModuleEntry* module);
    bool doImports(ModuleEntry* module);
    bool doResolve(ModuleEntry* module);
    bool doThisRewrite(ModuleEntry* module);
    bool doTypeCheck(ModuleEntry* module);
    bool doConstFolding(ModuleEntry* module);

    void emitStats();

    struct Stats
    {
        usize mFilesLoaded = 0;
        usize mSourceBytes = 0;
        usize mTokensScanned = 0;
        usize mASTNodesCreated = 0;
    };

    CompilerContext& mCtx;
    Stats mStats;
};

} // namespace simlang
