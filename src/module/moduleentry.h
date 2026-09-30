#pragma once

#include <filesystem>

#include "module/modulestage.h"

namespace simlang
{

struct Symbol;
struct TranslationUnitNode;

struct ModuleEntry
{
    std::filesystem::path mPath;
    TranslationUnitNode* mAST = nullptr;
    Symbol* mModuleSymbol = nullptr;
    ModuleStage mStage = ModuleStage::cCreated;
    bool mInProgress = false;
};

} // namespace simlang
