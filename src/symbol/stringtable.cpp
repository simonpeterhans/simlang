#include "symbol/stringtable.h"

#include <cstring>
#include <utility>

#include "symbol/internedstring.h"
#include "util/arena.h"
#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

static constexpr usize cInitialStringCapacity = 256;

StringTable::StringTable(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
    mStringMap.reserve(cInitialStringCapacity);
    mStrings.reserve(cInitialStringCapacity);
}

const InternedString* StringTable::addString(std::string_view str)
{
    if (str.length() > cMaxInternedStringLength)
    {
        SIMLANG_BREAK("String exceeds maximum interned string length.");
        return nullptr;
    }

    // Check if it already exists.
    auto it = mStringMap.find(str);
    if (it != mStringMap.end())
    {
        return it->second;
    }

    // Check if we have too many.
    if (mStrings.size() > cMaxInternedStringIdx)
    {
        SIMLANG_BREAK("String table exhausted.");
        return nullptr;
    }

    usize stringLength = str.length();
    char* data = mAllocator.createArray<char>(stringLength);
    if (data != nullptr)
    {
        // If we have at least one char, copy stuff in.
        std::memcpy(data, str.data(), stringLength);
    }

    // Create an interned string where we can store the data.
    auto* interned = mAllocator.create<InternedString>();
    interned->mData = data;
    interned->mLength = static_cast<u16>(stringLength);
    interned->mIndex = static_cast<InternedStringIdx>(mStrings.size());

    // Add to the map so we can do a fancy lookup.
    std::string_view key = interned->toView();
    mStringMap[key] = interned;

    // Finally, add it to our interned strings.
    mStrings.push_back(interned);

    return interned;
}

} // namespace simlang
