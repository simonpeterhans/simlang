#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "runtime/stringdata.h"
#include "runtime/vmdefines.h"
#include "symbol/internedstring.h"
#include "util/types.h"

namespace simlang
{

class StringFormatTemplateBuilder
{
public:
    void appendLiteral(StringLiteralIdx index, u16 byteLength);
    void appendArgKind(StringFormatArgKind kind);

    // Gets the runtime representation of the string format template.
    StringFormatTemplate build() &&;

private:
    StringFormatTemplate mTemplate;
};

struct StringFormatTemplateHash
{
    std::size_t operator()(const StringFormatTemplate& tmpl) const;
};

struct StringFormatTemplateEqual
{
    bool operator()(const StringFormatTemplate& lhs, const StringFormatTemplate& rhs) const;
};

class StringFormatTableBuilder
{
public:
    void getOrAddTemplate(StringFormatTemplate tmpl, StringFormatIdx& out);
    bool getTemplateIndex(const StringFormatTemplate& tmpl, StringFormatIdx& out) const;

    const std::vector<StringFormatTemplate>& geTemplates() const { return mTemplates; }

    // Builds the runtime string formats.
    std::vector<StringFormatTemplate> build() &&;

private:
    std::unordered_map<StringFormatTemplate, StringFormatIdx, StringFormatTemplateHash, StringFormatTemplateEqual>
        mTemplateIdxMap;
    std::vector<StringFormatTemplate> mTemplates;
};

class StringPoolBuilder
{
public:
    StringLiteralIdx getOrAddLiteral(const InternedString* str);
    bool getLiteralIndex(const InternedString* str, StringLiteralIdx& out) const;

    const std::vector<StringLiteral>& getLiterals() const { return mLiterals; }
    const std::vector<u8>& getBytes() const { return mBytes; }

    // Builds the runtime string pool representation.
    StringPool build() &&;

private:
    std::unordered_map<InternedStringIdx, StringLiteralIdx> mLiteralIdxMap;
    std::vector<StringLiteral> mLiterals;
    std::vector<u8> mBytes;
};

} // namespace simlang
