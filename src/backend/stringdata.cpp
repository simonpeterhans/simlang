#include "backend/stringdata.h"

#include <utility>

#include "util/hash.h"

namespace simlang
{

void StringFormatTemplateBuilder::appendLiteral(StringLiteralIdx index, u16 byteLength)
{
    mTemplate.mLiteralIndices.push_back(index);
    mTemplate.mLiteralByteCount += byteLength;
}

void StringFormatTemplateBuilder::appendArgKind(StringFormatArgKind kind)
{
    mTemplate.mArgKinds.push_back(kind);
}

StringFormatTemplate StringFormatTemplateBuilder::build() &&
{
    return std::move(mTemplate);
}

std::size_t StringFormatTemplateHash::operator()(const StringFormatTemplate& tmpl) const
{
    std::size_t seed = 0;

    hashCombine(seed, tmpl.mLiteralIndices.size());
    for (StringLiteralIdx index : tmpl.mLiteralIndices)
    {
        hashCombine(seed, index);
    }

    hashCombine(seed, tmpl.mArgKinds.size());
    for (StringFormatArgKind kind : tmpl.mArgKinds)
    {
        hashCombine(seed, static_cast<u8>(kind));
    }

    hashCombine(seed, tmpl.mLiteralByteCount);

    return seed;
}

bool StringFormatTemplateEqual::operator()(const StringFormatTemplate& lhs, const StringFormatTemplate& rhs) const
{
    return lhs.mLiteralIndices == rhs.mLiteralIndices && lhs.mArgKinds == rhs.mArgKinds &&
           lhs.mLiteralByteCount == rhs.mLiteralByteCount;
}

void StringFormatTableBuilder::getOrAddTemplate(StringFormatTemplate tmpl, StringFormatIdx& out)
{
    auto it = mTemplateIdxMap.find(tmpl);
    if (it != mTemplateIdxMap.end())
    {
        out = it->second;
        return;
    }

    // We could warn or assert here if we have too many, but 4 billion capacity should be enough.
    StringFormatIdx formatIndex = static_cast<StringFormatIdx>(mTemplates.size());
    mTemplateIdxMap.emplace(tmpl, formatIndex);
    mTemplates.push_back(std::move(tmpl));

    out = formatIndex;
}

bool StringFormatTableBuilder::getTemplateIndex(const StringFormatTemplate& tmpl, StringFormatIdx& out) const
{
    auto it = mTemplateIdxMap.find(tmpl);
    if (it == mTemplateIdxMap.end())
    {
        return false;
    }

    out = it->second;

    return true;
}

std::vector<StringFormatTemplate> StringFormatTableBuilder::build() &&
{
    // Building moves the data out from this into the runtime string format table.
    return std::move(mTemplates);
}

StringLiteralIdx StringPoolBuilder::getOrAddLiteral(const InternedString* str)
{
    InternedStringIdx internedStringIndex = str->mIndex;

    auto it = mLiteralIdxMap.find(internedStringIndex);
    if (it != mLiteralIdxMap.end())
    {
        return it->second;
    }

    // We could warn or assert here if we have too many, but 4 billion capacity should be enough.
    StringLiteralIdx literalIndex = static_cast<StringLiteralIdx>(mLiterals.size());

    StringLiteral lit;
    lit.mOffset = static_cast<u32>(mBytes.size());
    lit.mLength = str->mLength;

    // Append the string to the bytes.
    if (str->mLength > 0)
    {
        auto begin = reinterpret_cast<const u8*>(str->mData);
        const u8* end = begin + str->mLength;
        mBytes.insert(mBytes.end(), begin, end);
    }
    // Null-terminate the string.
    mBytes.push_back('\0');

    mLiterals.push_back(lit);
    mLiteralIdxMap[internedStringIndex] = literalIndex;

    return literalIndex;
}

bool StringPoolBuilder::getLiteralIndex(const InternedString* str, StringLiteralIdx& out) const
{
    InternedStringIdx internedStringIndex = str->mIndex;

    auto it = mLiteralIdxMap.find(internedStringIndex);
    if (it == mLiteralIdxMap.end())
    {
        return false;
    }

    out = it->second;

    return true;
}

StringPool StringPoolBuilder::build() &&
{
    StringPool pool;
    pool.mLiterals = std::move(mLiterals);
    pool.mBytes = std::move(mBytes);
    return pool;
}

} // namespace simlang
