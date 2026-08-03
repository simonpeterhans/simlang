#pragma once

#include <string>
#include <vector>

#include "runtime/vmdefines.h"
#include "util/types.h"

namespace simlang
{

class BytecodeChunk;
class SourceRange;
class TextWriter;
struct BytecodeDumpOptions;
struct CompilerContext;
struct Op;
struct StringFormatTemplate;
struct Symbol;
struct TypeLayout;

class BytecodeDump
{
public:
    BytecodeDump(TextWriter& out, const CompilerContext& ctx, const BytecodeDumpOptions& options);

    void write();

private:
    static void setIndexedSymbol(std::vector<const Symbol*>& symbols, usize index, const Symbol* symbol);
    static const Symbol* getIndexedSymbol(const std::vector<const Symbol*>& symbols, usize index);

    void buildSymbolIndex();
    void indexFunctionSymbol(const Symbol* symbol);
    void indexTypeSymbol(const Symbol* symbol);
    void indexSyscallSymbol(const Symbol* symbol);
    void indexMemberOwners(const Symbol* symbol);

    const Symbol* getOwnerSymbol(const Symbol* symbol) const;
    const Symbol* getFunctionSymbol(FunctionIdx index) const;
    const Symbol* getSyscallSymbol(SyscallIdx index) const;
    const Symbol* getTypeSymbol(TypeID index) const;

    std::string getQualifiedSymbolName(const Symbol* symbol) const;
    std::string getFunctionName(FunctionIdx index) const;
    std::string getTypeName(TypeID index) const;

    void writeSymbolRef(const Symbol* symbol);
    void writeDeclLocation(const Symbol* symbol);
    void writeFunctionIndex(const char* name, FunctionIdx index);
    void writeSyscallIndex(const char* name, SyscallIdx index);
    void writeTypeIndex(const char* name, TypeID index);
    void writeStringLiteralIndex(const char* name, StringLiteralIdx index);

    void writeOperandList(const Op& op);
    void writeOpLine(u32 index, const Op& op);
    void writeSourceLineComment(SourceRange range, u32& previousSourceID, u32& previousLine);
    const SourceRange* getSourceRange(const BytecodeChunk& code, usize opIndex) const;

    void writeFunctionTable();
    void writeStringTable();
    void writeTypeTable();
    void writeProgramSection();
    void writeRawBytecode();

    void writeFormatTemplate(const StringFormatTemplate& tmpl);
    void writeRefOffsets(const TypeLayout& layout);
    void writeTypeMembers(const Symbol* symbol);

    TextWriter& mOut;
    const CompilerContext& mCtx;
    const BytecodeDumpOptions& mOptions;

    std::vector<const Symbol*> mFunctionSymbols;
    std::vector<const Symbol*> mSyscallSymbols;
    std::vector<const Symbol*> mTypeSymbols;
    std::vector<const Symbol*> mOwnerSymbols;
};

} // namespace simlang
