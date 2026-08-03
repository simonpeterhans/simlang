#include "symbol/identifiertable.h"

#include <cstring>
#include <limits>
#include <utility>

#include "symbol/identifier.h"
#include "util/arena.h"
#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

static constexpr usize cInitialIdentifierCapacity = 2048;

IdentifierTable::IdentifierTable(ArenaAllocator& allocator)
    : mAllocator(allocator)
{
    mData.reserve(cInitialIdentifierCapacity);
    mIdentifiers.reserve(cInitialIdentifierCapacity);

    mInit = addIdentifier(cInitName);
    SIMLANG_ASSERT(mInit != nullptr);

    mMain = addIdentifier(cMainName);
    SIMLANG_ASSERT(mMain != nullptr);
}

Identifier* IdentifierTable::addIdentifier(std::string_view name)
{
    if (name.empty())
    {
        SIMLANG_BREAK("Cannot intern an empty identifier.");
        return nullptr;
    }

    if (name.length() > cMaxIdentifierLen)
    {
        SIMLANG_BREAK("Identifier exceeds maximum length.");
        return nullptr;
    }

    // If it already exists, don't add it.
    auto it = mData.find(name);
    if (it != mData.end())
    {
        return it->second;
    }

    if (mIdentifiers.size() > std::numeric_limits<u32>::max())
    {
        SIMLANG_BREAK("Identifier table exhausted.");
        return nullptr;
    }

    // Otherwise, create a new one.
    auto* identifier = mAllocator.create<Identifier>();

    identifier->mName = mAllocator.createArray<char>(name.length() + 1);
    std::memcpy(identifier->mName, name.data(), name.length());
    identifier->mName[name.length()] = '\0'; // Null-terminate the string.

    identifier->mLength = static_cast<u8>(name.length());
    identifier->mID = static_cast<u32>(mIdentifiers.size());

    // Create a sv from the identifier that we just created.
    // That way, we know the sv will be valid as long as the allocator holds the identifiers.
    std::string_view key{identifier->mName, identifier->mLength};
    mData[key] = identifier;

    mIdentifiers.push_back(identifier);

    return identifier;
}

} // namespace simlang
