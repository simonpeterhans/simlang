#pragma once

#include <filesystem>

#include "module/modulestage.h"

namespace simlang
{

class Scope;
struct Symbol;
struct TranslationUnitNode;

struct ModuleEntry
{
    std::filesystem::path mPath;
    TranslationUnitNode* mAST = nullptr;
    Scope* mExportScope = nullptr;
    Symbol* mModuleSymbol = nullptr;
    ModuleStage mStage = ModuleStage::cCreated;
    bool mInProgress = false;
};

} // namespace simlang
