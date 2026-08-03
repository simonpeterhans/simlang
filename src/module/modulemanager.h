#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "module/moduledata.h"

namespace simlang
{

template <typename T>
class ArrayView;
class DiagnosticManager;
class IdentifierTable;
class SourceRange;
class SymbolRegistry;
struct Identifier;
struct ModuleEntry;

class ModuleManager
{
public:
    explicit ModuleManager(DiagnosticManager& diag, IdentifierTable& identifiers, SymbolRegistry& symbols);

    ModuleEntry* getOrRegisterModule(std::filesystem::path path, SourceRange importRange);
    ModuleEntry* getOrRegisterModule(ArrayView<Identifier*> pathIdentifiers, SourceRange importRange);
    ModuleEntry* getOrRegisterModuleRelative(ArrayView<Identifier*> pathIdentifiers,
                                             ModuleEntry* current,
                                             SourceRange importRange);

    std::filesystem::path getPathRelativeToRoot(const std::filesystem::path& path) const;

    void setRoot(const std::filesystem::path& root);
    const std::filesystem::path& getRoot() const;

    const std::vector<std::unique_ptr<ModuleEntry>>& getModules() const;

private:
    std::filesystem::path getPathFromRoot(ArrayView<Identifier*> pathIdentifiers) const;

    DiagnosticManager& mDiag;
    IdentifierTable& mIdentifiers;
    SymbolRegistry& mSymbols;
    ModuleData mModules;
};

} // namespace simlang
