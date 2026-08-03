#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

namespace simlang
{

class ArenaAllocator;
struct InternedString;

class StringTable
{
public:
    explicit StringTable(ArenaAllocator& allocator);

    const InternedString* addString(std::string_view str);

private:
    ArenaAllocator& mAllocator;
    std::unordered_map<std::string_view, const InternedString*> mStringMap;
    std::vector<const InternedString*> mStrings;
};

} // namespace simlang
