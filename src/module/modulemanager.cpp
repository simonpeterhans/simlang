#include "module/modulemanager.h"

#include <cstddef>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "module/moduleentry.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/interning.h"
#include "symbol/symbol.h"
#include "symbol/symbolregistry.h"
#include "symbol/symboltype.h"
#include "util/arrayview.h"

namespace simlang
{

static constexpr std::size_t cInitialModuleCapacity = 64;

static bool pathExists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::filesystem::path canonicalizeModulePath(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path canonicalPath = std::filesystem::canonical(path, ec);
    if (ec == std::error_code())
    {
        return canonicalPath;
    }

    return std::filesystem::absolute(path).lexically_normal();
}

ModuleManager::ModuleManager(DiagnosticManager& diag, IdentifierTable& identifiers, SymbolRegistry& symbols)
    : mDiag(diag)
    , mIdentifiers(identifiers)
    , mSymbols(symbols)
{
    mModules.mModuleList.reserve(cInitialModuleCapacity);
    mModules.mModuleMap.reserve(cInitialModuleCapacity);
    mModules.mModuleLookupMap.reserve(cInitialModuleCapacity);
}

void ModuleManager::setRoot(const std::filesystem::path& root)
{
    mModules.mRoot = root;
}

const std::filesystem::path& ModuleManager::getRoot() const
{
    return mModules.mRoot;
}

const std::vector<std::unique_ptr<ModuleEntry>>& ModuleManager::getModules() const
{
    return mModules.mModuleList;
}

std::filesystem::path ModuleManager::getPathRelativeToRoot(const std::filesystem::path& path) const
{
    std::filesystem::path root = mModules.mRoot.empty() ? std::filesystem::current_path() : mModules.mRoot;
    std::filesystem::path absoluteRoot = canonicalizeModulePath(root);
    std::filesystem::path absolutePath = canonicalizeModulePath(path);
    std::filesystem::path relativePath = absolutePath.lexically_relative(absoluteRoot);

    auto it = relativePath.begin();
    if (relativePath.empty() || (it != relativePath.end() && *it == ".."))
    {
        return absolutePath;
    }

    return relativePath;
}

static std::filesystem::path appendModulePath(std::filesystem::path base, ArrayView<Identifier*> pathIdentifiers)
{
    for (Identifier* identifier : pathIdentifiers)
    {
        base /= identifier->mName;
    }

    return base;
}

static std::filesystem::path getPathRelativeToCurrent(ArrayView<Identifier*> pathIdentifiers,
                                                      const ModuleEntry* current)
{
    // Get the path of the current module and drop the file name.
    std::filesystem::path path = current->mPath;
    path.remove_filename();

    return appendModulePath(std::move(path), pathIdentifiers);
}

std::filesystem::path ModuleManager::getPathFromRoot(ArrayView<Identifier*> pathIdentifiers) const
{
    return appendModulePath(mModules.mRoot, pathIdentifiers);
}

ModuleEntry* ModuleManager::getOrRegisterModuleRelative(ArrayView<Identifier*> pathIdentifiers,
                                                        ModuleEntry* current,
                                                        SourceRange importRange)
{
    return getOrRegisterModule(getPathRelativeToCurrent(pathIdentifiers, current), importRange);
}

ModuleEntry* ModuleManager::getOrRegisterModule(ArrayView<Identifier*> pathIdentifiers, SourceRange importRange)
{
    return getOrRegisterModule(getPathFromRoot(pathIdentifiers), importRange);
}

ModuleEntry* ModuleManager::getOrRegisterModule(std::filesystem::path path, SourceRange importRange)
{
    std::filesystem::path lookupPath = path;
    lookupPath.replace_extension("");
    const std::string lookupKey = lookupPath.lexically_normal().generic_string();
    auto lookupIt = mModules.mModuleLookupMap.find(lookupKey);
    if (lookupIt != mModules.mModuleLookupMap.end())
    {
        return lookupIt->second;
    }

    std::filesystem::path singleFilePath = path;
    singleFilePath.replace_extension(".sim");
    bool singleFileExists = pathExists(singleFilePath);

    std::filesystem::path moduleRootPath = path;
    moduleRootPath.replace_extension("");

    std::filesystem::path modFilePath = moduleRootPath;
    modFilePath /= "modules.sim";
    bool modFileExists = pathExists(modFilePath);

    if (singleFileExists == false && modFileExists == false)
    {
        mDiag.report<cModuleNotFound>(importRange, moduleRootPath.generic_string());
        return nullptr;
    }

    if (singleFileExists && modFileExists)
    {
        mDiag.report<cAmbiguousModuleRoot>(importRange,
                                           singleFilePath.generic_string(),
                                           modFilePath.generic_string(),
                                           moduleRootPath.generic_string());
        return nullptr;
    }

    std::filesystem::path modulePath = singleFileExists ? singleFilePath : modFilePath;
    modulePath = canonicalizeModulePath(modulePath);
    const std::string key = modulePath.generic_string();

    auto it = mModules.mModuleMap.find(key);
    if (it != mModules.mModuleMap.end())
    {
        mModules.mModuleLookupMap.emplace(lookupKey, it->second);
        return it->second;
    }

    std::string moduleIdentifierName = modulePath.filename().string();
    Identifier* moduleIdentifier = intern::addIdentifier(mDiag, mIdentifiers, moduleIdentifierName, importRange);
    if (moduleIdentifier == nullptr)
    {
        return nullptr;
    }

    // Create a new module entry and get it so we can fill it.
    auto& module = mModules.mModuleList.emplace_back(std::make_unique<ModuleEntry>());
    ModuleEntry* ret = module.get();
    ret->mPath = std::move(modulePath);

    // Create a symbol so imports can be resolved using that.
    Symbol* symbol = mSymbols.createSymbol(SymbolType::cModule);
    symbol->mIdentifier = moduleIdentifier;
    ret->mModuleSymbol = symbol;

    // Finally, add it to the map.
    mModules.mModuleMap.emplace(key, ret);
    mModules.mModuleLookupMap.emplace(lookupKey, ret);

    return ret;
}

} // namespace simlang
