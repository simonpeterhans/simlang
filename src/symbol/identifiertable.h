#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

namespace simlang
{

class ArenaAllocator;
struct Identifier;

inline constexpr std::string_view cInitName = "init";
inline constexpr std::string_view cMainName = "main";

class IdentifierTable
{
public:
    explicit IdentifierTable(ArenaAllocator& allocator);

    Identifier* addIdentifier(std::string_view name);
    Identifier* getInitIdentifier() const { return mInit; }
    Identifier* getMainIdentifier() const { return mMain; }

private:
    ArenaAllocator& mAllocator;
    std::unordered_map<std::string_view, Identifier*> mData;
    std::vector<Identifier*> mIdentifiers;

    Identifier* mInit = nullptr;
    Identifier* mMain = nullptr;
};

} // namespace simlang
