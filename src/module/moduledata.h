#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "module/moduleentry.h"

namespace simlang
{

struct ModuleData
{
    std::filesystem::path mRoot = "";
    std::vector<std::unique_ptr<ModuleEntry>> mModuleList;
    std::unordered_map<std::string, ModuleEntry*> mModuleMap;
    std::unordered_map<std::string, ModuleEntry*> mModuleLookupMap;
};

} // namespace simlang
