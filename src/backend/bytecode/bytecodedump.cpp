#include "backend/bytecode/bytecodedump.h"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "ast/nodes/astnode.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "backend/backendstate.h"
#include "backend/bytecode/bytecodedumpoptions.h"
#include "backend/bytecode/bytecodeprogram.h"
#include "backend/layout/layout.h"
#include "backend/layout/typelayouttablebuilder.h"
#include "backend/stringdata.h"
#include "backend/typeidutils.h"
#include "driver/compilercontext.h"
#include "runtime/callinfo.h"
#include "runtime/memory/typelayout.h"
#include "runtime/op/op.h"
#include "runtime/op/opcode.h"
#include "runtime/op/opmacros.h"
#include "runtime/op/oputils.h"
#include "runtime/stringdata.h"
#include "runtime/syscall/syscallentry.h"
#include "runtime/vmdefines.h"
#include "source/linecolumninfo.h"
#include "source/source.h"
#include "source/sourcemanager.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "symbol/symbol.h"
#include "symbol/symbolregistry.h"
#include "symbol/symboltype.h"
#include "symbol/symbolutils.h"
#include "type/typeformat.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arrayview.h"
#include "util/bitutils.h"
#include "util/escaping.h"
#include "util/textwriter.h"
#include "util/types.h"

namespace simlang
{

namespace
{

std::string_view getIdentifierName(const Identifier* identifier)
{
    if (identifier == nullptr || identifier->mName == nullptr)
    {
        return "<anonymous>";
    }

    return std::string_view{identifier->mName, identifier->mLength};
}

std::string_view getSymbolName(const Symbol* symbol)
{
    if (symbol == nullptr)
    {
        return "<none>";
    }

    return getIdentifierName(symbol->mIdentifier);
}

bool isTypeSymbol(const Symbol* symbol)
{
    if (symbol == nullptr)
    {
        return false;
    }

    return symbol->mSymbolType == SymbolType::cStruct || symbol->mSymbolType == SymbolType::cClass ||
           symbol->mSymbolType == SymbolType::cInterface;
}

bool isFunctionSymbol(const Symbol* symbol)
{
    if (symbol == nullptr)
    {
        return false;
    }

    return symbol->mSymbolType == SymbolType::cFunction || symbol->mSymbolType == SymbolType::cMemberFunction;
}

bool hasFunctionBody(const Symbol* symbol)
{
    if (symbol == nullptr || symbol->mDeclNode == nullptr ||
        symbol->mDeclNode->mNodeType != NodeType::cFunctionDeclarationStatement)
    {
        return false;
    }

    auto* decl = static_cast<FunctionDeclarationStatementNode*>(symbol->mDeclNode);
    return decl->mBody != nullptr;
}

const char* getTypeLayoutKindName(TypeLayout::Kind kind)
{
    switch (kind)
    {
        case TypeLayout::Kind::cInline: return "inline";
        case TypeLayout::Kind::cReference: return "reference";
        default: return "???";
    }
}

std::string_view getStringLiteralText(const BackendState& backend, const StringLiteral& literal)
{
    const std::vector<u8>& bytes = backend.mStrings.getBytes();
    if (literal.mOffset >= bytes.size())
    {
        return {};
    }

    usize available = bytes.size() - literal.mOffset;
    usize length = literal.mLength;
    if (length > available)
    {
        length = available;
    }

    const auto* data = reinterpret_cast<const char*>(bytes.data() + literal.mOffset);
    return std::string_view{data, length};
}

template <typename T>
bool readBytecodeValue(const std::vector<u8>& bytes, usize& offset, T& out)
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    {
        return false;
    }

    out = bits::readUnaligned<T>(bytes.data() + offset);
    offset += sizeof(T);
    return true;
}

bool readOp(const std::vector<u8>& bytes, usize& offset, Op& out)
{
    OpCode opCode;
    if (readBytecodeValue(bytes, offset, opCode) == false)
    {
        return false;
    }

    out.mOpCode = opCode;

    switch (opCode)
    {
#define X(NAME, COUNT, T1, N1, T2, N2) \
    case OpCode::c##NAME: \
    { \
        SIMLANG_OP_IFGE1(COUNT, if (readBytecodeValue(bytes, offset, out.as.m##NAME.N1) == false) { return false; }) \
        SIMLANG_OP_IF2(COUNT, if (readBytecodeValue(bytes, offset, out.as.m##NAME.N2) == false) { return false; }) \
        return true; \
    }
#include "runtime/op/opcodes.def"

#undef X
        default: return false;
    }
}

} // namespace

BytecodeDump::BytecodeDump(TextWriter& out, const CompilerContext& ctx, const BytecodeDumpOptions& options)
    : mOut(out)
    , mCtx(ctx)
    , mOptions(options)
{
    buildSymbolIndex();
}

void BytecodeDump::write()
{
    // Write the requested dump sections in a stable order.
    if (mOptions.mIncludeFunctionTable)
    {
        writeFunctionTable();
        mOut << "\n";
    }

    if (mOptions.mIncludeStringTable)
    {
        writeStringTable();
        mOut << "\n";
    }

    if (mOptions.mIncludeTypeTable)
    {
        writeTypeTable();
        mOut << "\n";
    }

    if (mOptions.mIncludeProgram)
    {
        writeProgramSection();
        mOut << "\n";
    }

    if (mOptions.mIncludeRawBytecode)
    {
        writeRawBytecode();
    }
}

void BytecodeDump::setIndexedSymbol(std::vector<const Symbol*>& symbols, usize index, const Symbol* symbol)
{
    if (index >= symbols.size())
    {
        symbols.resize(index + 1U);
    }

    symbols[index] = symbol;
}

const Symbol* BytecodeDump::getIndexedSymbol(const std::vector<const Symbol*>& symbols, usize index)
{
    if (index >= symbols.size())
    {
        return nullptr;
    }

    return symbols[index];
}

void BytecodeDump::buildSymbolIndex()
{
    // TODO We basically build a symbol table here because our symbol registry is just a vector.
    // If we ever get a proper debug symbol lookup table, use that instead here.
    mFunctionSymbols.resize(mCtx.mBackend.mFunctionInfos.size());
    mSyscallSymbols.resize(mCtx.mBackend.mSyscallInfos.size());
    mTypeSymbols.resize(mCtx.mBackend.mNextTypeID);
    mOwnerSymbols.resize(mCtx.mSymbols.getSymbols().size());

    // Index every symbol by the runtime/backend index that is relevant for its kind.
    for (const Symbol* symbol : mCtx.mSymbols.getSymbols())
    {
        if (symbol == nullptr)
        {
            continue;
        }

        indexFunctionSymbol(symbol);
        indexSyscallSymbol(symbol);
        indexTypeSymbol(symbol);
        indexMemberOwners(symbol);
    }
}

void BytecodeDump::indexFunctionSymbol(const Symbol* symbol)
{
    if (isFunctionSymbol(symbol) == false || symbol->mIndex < 0)
    {
        return;
    }

    // If multiple symbols share the same function index, prefer the declaration that has bytecode.
    usize index = static_cast<usize>(symbol->mIndex);
    if (index >= mFunctionSymbols.size())
    {
        mFunctionSymbols.resize(index + 1U);
    }

    const Symbol* existing = mFunctionSymbols[index];
    if (hasFunctionBody(symbol))
    {
        if (hasFunctionBody(existing) == false)
        {
            mFunctionSymbols[index] = symbol;
        }
        return;
    }

    // Otherwise keep the first non-body symbol as a fallback.
    if (hasFunctionBody(existing) == false)
    {
        mFunctionSymbols[index] = symbol;
    }
}

void BytecodeDump::indexTypeSymbol(const Symbol* symbol)
{
    if (isTypeSymbol(symbol) == false || symbol->mIndex < 0)
    {
        return;
    }

    setIndexedSymbol(mTypeSymbols, static_cast<usize>(symbol->mIndex), symbol);
}

void BytecodeDump::indexSyscallSymbol(const Symbol* symbol)
{
    if (symbol->mSymbolType != SymbolType::cSyscall || symbol->mIndex < 0)
    {
        return;
    }

    setIndexedSymbol(mSyscallSymbols, static_cast<usize>(symbol->mIndex), symbol);
}

void BytecodeDump::indexMemberOwners(const Symbol* symbol)
{
    if (isTypeSymbol(symbol) == false)
    {
        return;
    }

    // Record the aggregate/interface owner for every member symbol.
    for (const Symbol* member : symbol->mMembers)
    {
        if (member != nullptr && member->mID >= 0)
        {
            setIndexedSymbol(mOwnerSymbols, static_cast<usize>(member->mID), symbol);
        }
    }
}

const Symbol* BytecodeDump::getOwnerSymbol(const Symbol* symbol) const
{
    if (symbol == nullptr || symbol->mID < 0)
    {
        return nullptr;
    }

    return getIndexedSymbol(mOwnerSymbols, static_cast<usize>(symbol->mID));
}

const Symbol* BytecodeDump::getFunctionSymbol(FunctionIdx index) const
{
    return getIndexedSymbol(mFunctionSymbols, index);
}

const Symbol* BytecodeDump::getSyscallSymbol(SyscallIdx index) const
{
    return getIndexedSymbol(mSyscallSymbols, index);
}

const Symbol* BytecodeDump::getTypeSymbol(TypeID index) const
{
    return getIndexedSymbol(mTypeSymbols, index);
}

std::string BytecodeDump::getQualifiedSymbolName(const Symbol* symbol) const
{
    if (symbol == nullptr)
    {
        return "<unknown>";
    }

    const Symbol* owner = getOwnerSymbol(symbol);
    if (owner == nullptr)
    {
        return std::string{getSymbolName(symbol)};
    }

    // Member functions are printed as Owner.member.
    std::string result{getSymbolName(owner)};
    result += ".";
    result += getSymbolName(symbol);
    return result;
}

std::string BytecodeDump::getFunctionName(FunctionIdx index) const
{
    const Symbol* symbol = getFunctionSymbol(index);
    if (symbol == nullptr)
    {
        return "<unknown>";
    }

    return getQualifiedSymbolName(symbol);
}

std::string BytecodeDump::getTypeName(TypeID index) const
{
    if (const char* builtinName = getRuntimeBuiltinTypeIDName(index))
    {
        return builtinName;
    }

    const Symbol* symbol = getTypeSymbol(index);
    if (symbol == nullptr)
    {
        return "<unknown>";
    }

    return std::string{getSymbolName(symbol)};
}

void BytecodeDump::writeSymbolRef(const Symbol* symbol)
{
    if (symbol == nullptr)
    {
        mOut << "<none>";
        return;
    }

    mOut << "#" << symbol->mID << "(" << getSymbolName(symbol) << ")";
}

void BytecodeDump::writeDeclLocation(const Symbol* symbol)
{
    // Resolve the declaration range to a user-facing file/line/column.
    SourceRange range = getSymbolSourceRange(symbol);
    if (range.isValid() == false)
    {
        return;
    }

    ResolvedSourceLocation resolved = mCtx.mSources.resolveLocation(range.getStartLoc());
    if (resolved.isValid() == false)
    {
        return;
    }

    // Offset by 1 for 1-based line and column indices.
    LineColumnInfo info = resolved.mSource->getLineAndColumnFromOffset(resolved.mLocalOffset);
    mOut << "  decl: " << resolved.mSource->getFilename() << ":" << (info.mLineIndex + 1) << ":"
         << (info.mColumnIndex + 1) << "\n";
}

void BytecodeDump::writeFunctionIndex(const char* name, FunctionIdx index)
{
    mOut << name << "=" << index;
    const Symbol* symbol = getFunctionSymbol(index);
    if (symbol != nullptr)
    {
        mOut << "(" << getQualifiedSymbolName(symbol) << ")";
    }
}

void BytecodeDump::writeSyscallIndex(const char* name, SyscallIdx index)
{
    mOut << name << "=" << index;
    const Symbol* symbol = getSyscallSymbol(index);
    if (symbol != nullptr)
    {
        mOut << "(" << getSymbolName(symbol) << ")";
    }
}

void BytecodeDump::writeTypeIndex(const char* name, TypeID index)
{
    mOut << name << "=" << index << "(" << getTypeName(index) << ")";
}

void BytecodeDump::writeStringLiteralIndex(const char* name, StringLiteralIdx index)
{
    mOut << name << "=" << index;

    // Add the string contents as an annotation if the literal index is valid.
    const std::vector<StringLiteral>& literals = mCtx.mBackend.mStrings.getLiterals();
    if (index >= literals.size())
    {
        return;
    }

    std::string_view text = getStringLiteralText(mCtx.mBackend, literals[index]);
    mOut << "(\"" << escape::formatEscapedText(text) << "\")";
}

void BytecodeDump::writeOperandList(const Op& op)
{
    if (mOptions.mIncludeAnnotations)
    {
        // Some operands are more useful when printed with resolved symbol/type/string annotations.
        switch (op.mOpCode)
        {
            case OpCode::cCall:
            {
                writeFunctionIndex("mIndex", op.as.mCall.mIndex);
                return;
            }
            case OpCode::cCallMethod:
            {
                writeFunctionIndex("mIndex", op.as.mCallMethod.mIndex);
                return;
            }
            case OpCode::cSyscall:
            {
                writeSyscallIndex("mIndex", op.as.mSyscall.mIndex);
                return;
            }
            case OpCode::cNewObject:
            {
                writeTypeIndex("mTypeID", op.as.mNewObject.mTypeID);
                return;
            }
            case OpCode::cNewList:
            {
                writeTypeIndex("mTypeID", op.as.mNewList.mTypeID);
                return;
            }
            case OpCode::cNewMap:
            {
                writeTypeIndex("mKeyTypeID", op.as.mNewMap.mKeyTypeID);
                mOut << " ";
                writeTypeIndex("mValueTypeID", op.as.mNewMap.mValueTypeID);
                return;
            }
            case OpCode::cCheckCast:
            {
                writeTypeIndex("mTypeID", op.as.mCheckCast.mTypeID);
                return;
            }
            case OpCode::cPushString:
            {
                writeStringLiteralIndex("mIndex", op.as.mPushString.mIndex);
                return;
            }
            case OpCode::cFormatString:
            {
                mOut << "mIndex=" << op.as.mFormatString.mIndex;
                return;
            }
            case OpCode::cPrint:
            {
                auto kind = static_cast<StringFormatArgKind>(op.as.mPrint.mKind);
                mOut << "mKind=" << op.as.mPrint.mKind << "(" << stringFormatArgKindToString(kind) << ")";
                return;
            }
            default:
            {
                break;
            }
        }
    }

    // Fall back to the raw opcode operand names from opcodes.def.
    switch (op.mOpCode)
    {
#define X(NAME, COUNT, T1, N1, T2, N2) \
    case OpCode::c##NAME: \
    { \
        SIMLANG_OP_IFGE1(COUNT, mOut << #N1 << "=" << op.as.m##NAME.N1;) \
        SIMLANG_OP_IF2(COUNT, mOut << " " << #N2 << "=" << op.as.m##NAME.N2;) \
        break; \
    }
#include "runtime/op/opcodes.def"

#undef X
    }
}

void BytecodeDump::writeOpLine(u32 index, const Op& op)
{
    mOut << "[";
    mOut.writePaddedValue(index, 4, TextAlignment::cRight);
    mOut << "] ";
    mOut.writePadded(getOpCodeName(op.mOpCode), 15, TextAlignment::cLeft);

    if (hasOpOperands(op.mOpCode))
    {
        mOut << " ";
        writeOperandList(op);
    }

    mOut << "\n";
}

void BytecodeDump::writeSourceLineComment(SourceRange range, u32& previousSourceID, u32& previousLine)
{
    // Only emit source comments when we can resolve the bytecode op to a source line.
    if (range.isValid() == false)
    {
        return;
    }

    ResolvedSourceLocation resolved = mCtx.mSources.resolveLocation(range.getStartLoc());
    if (resolved.isValid() == false)
    {
        return;
    }

    LineColumnInfo info = resolved.mSource->getLineAndColumnFromOffset(resolved.mLocalOffset);

    // Avoid repeating the same source line before consecutive ops.
    if (resolved.mSourceID == previousSourceID && info.mLineIndex == previousLine)
    {
        return;
    }

    previousSourceID = resolved.mSourceID;
    previousLine = info.mLineIndex;
    mOut << "\n// Line " << (info.mLineIndex + 1) << ": " << info.mLine << "\n";
}

const SourceRange* BytecodeDump::getSourceRange(const BytecodeChunk& code, usize opIndex) const
{
    const std::vector<SourceRange>& sourceRanges = code.getSourceRanges();
    if (opIndex >= sourceRanges.size())
    {
        return nullptr;
    }

    return &sourceRanges[opIndex];
}

void BytecodeDump::writeFunctionTable()
{
    mOut << "== functions ==\n";

    const std::vector<FunctionInfo>& functions = mCtx.mBackend.mFunctionInfos;
    for (usize i = 0; i < functions.size(); ++i)
    {
        FunctionIdx functionIdx = static_cast<FunctionIdx>(i);
        const Symbol* symbol = getFunctionSymbol(functionIdx);
        const FunctionInfo& info = functions[i];

        mOut << "\n[function=" << functionIdx << "] " << getFunctionName(functionIdx) << "\n";
        mOut << "  symbol: ";
        writeSymbolRef(symbol);
        mOut << "\n";

        if (symbol != nullptr && symbol->mType != nullptr)
        {
            mOut << "  signature: " << typeToString(symbol->mType) << "\n";
        }

        mOut << "  frame: arg-words=" << info.mArgWords << ", local-words=" << info.mLocalWords
             << ", return-words=" << info.mReturnWords << ", max-stack=" << info.mMaxStackWords << "\n";
        mOut << "  bytecode-entry: " << info.mEntryAddress << "\n";
        writeDeclLocation(symbol);
    }
}

void BytecodeDump::writeStringTable()
{
    const std::vector<StringLiteral>& literals = mCtx.mBackend.mStrings.getLiterals();
    const std::vector<u8>& bytes = mCtx.mBackend.mStrings.getBytes();
    const std::vector<StringFormatTemplate>& formats = mCtx.mBackend.mStringFormats.geTemplates();

    mOut << "== strings ==\n";
    mOut << "literals: " << literals.size() << "\n";
    mOut << "bytes: " << bytes.size() << "\n";
    mOut << "formats: " << formats.size() << "\n";

    mOut << "\n== string literals ==\n";

    // Dump the literal table with escaped contents.
    for (usize i = 0; i < literals.size(); ++i)
    {
        const StringLiteral& literal = literals[i];
        mOut << "[" << i << "] offset=" << literal.mOffset << " length=" << literal.mLength << " \""
             << escape::formatEscapedText(getStringLiteralText(mCtx.mBackend, literal)) << "\"\n";
    }

    mOut << "\n== string formats ==\n";

    // Dump format templates separately so their literal/argument layout is visible.
    for (usize i = 0; i < formats.size(); ++i)
    {
        mOut << "[" << i << "] ";
        writeFormatTemplate(formats[i]);
        mOut << "\n";
    }
}

void BytecodeDump::writeTypeTable()
{
    mOut << "== types ==\n";

    // mTypeSymbols is indexed by type ID, so iteration naturally prints type-ID order.
    for (const Symbol* symbol : mTypeSymbols)
    {
        if (symbol == nullptr || isTypeSymbol(symbol) == false || symbol->mIndex < 0)
        {
            continue;
        }

        TypeID typeID = static_cast<TypeID>(symbol->mIndex);
        mOut << "\n[type=" << typeID << "] " << getSymbolName(symbol) << " " << symbolTypeToString(symbol->mSymbolType)
             << "\n";
        mOut << "  symbol: ";
        writeSymbolRef(symbol);
        mOut << "\n";

        if (symbol->mType != nullptr)
        {
            mOut << "  type: " << typeToString(symbol->mType) << "\n";
            mOut << "  value-words: " << layout::getWordSizeForType(symbol->mType) << "\n";
        }

        writeDeclLocation(symbol);

        if (mCtx.mBackend.mTypeLayoutTable.hasLayout(typeID))
        {
            const TypeLayout& layout = mCtx.mBackend.mTypeLayoutTable.getLayout(typeID);
            mOut << "  layout: kind=" << getTypeLayoutKindName(layout.getKind()) << ", words=" << layout.getWordCount()
                 << ", refs=" << layout.getRefCount() << ", ref-offset-start=" << layout.getRefOffsetStartIndex()
                 << "\n";
            writeRefOffsets(layout);
        }

        writeTypeMembers(symbol);
    }
}

void BytecodeDump::writeProgramSection()
{
    mOut << "== entry ==\n";

    // Dump the entry chunk before all function chunks.
    const BytecodeChunk& entry = mCtx.mBackend.mProgramBytecode.getEntryCode();
    for (usize i = 0; i < entry.getOps().size(); ++i)
    {
        writeOpLine(static_cast<u32>(i), entry.getOps()[i]);
    }

    const std::vector<FunctionBytecode>& functions = mCtx.mBackend.mProgramBytecode.getFunctions();

    // Dump each function chunk with optional frame metadata and source-line comments.
    for (usize i = 0; i < functions.size(); ++i)
    {
        const FunctionBytecode& function = functions[i];
        const BytecodeChunk& code = function.getCode();
        FunctionIdx functionIdx = function.getFunctionIdx();
        mOut << "\n== function " << functionIdx << ": " << getFunctionName(functionIdx) << " ==\n";

        if (functionIdx < mCtx.mBackend.mFunctionInfos.size())
        {
            const FunctionInfo& info = mCtx.mBackend.mFunctionInfos[functionIdx];
            mOut << "; frame arg-words=" << info.mArgWords << ", local-words=" << info.mLocalWords
                 << ", return-words=" << info.mReturnWords << ", max-stack=" << info.mMaxStackWords
                 << ", entry=" << info.mEntryAddress << "\n";
        }

        SourceID previousSourceID = cInvalidSourceID;
        u32 previousLine = std::numeric_limits<u32>::max();

        for (usize opIdx = 0; opIdx < code.getOps().size(); ++opIdx)
        {
            const SourceRange* sourceRange = getSourceRange(code, opIdx);
            if (mOptions.mIncludeSourceLines && sourceRange != nullptr)
            {
                writeSourceLineComment(*sourceRange, previousSourceID, previousLine);
            }

            writeOpLine(static_cast<u32>(opIdx), code.getOps()[opIdx]);
        }
    }
}

void BytecodeDump::writeRawBytecode()
{
    const std::vector<u8>& bytes = mCtx.mBackend.mBytes;

    mOut << "== raw bytecode ==\n";
    mOut << "bytes: " << bytes.size() << "\n";
    mOut << "functions: " << mCtx.mBackend.mFunctionInfos.size() << "\n";
    mOut << "main: " << mCtx.mBackend.mMainIndex << "\n\n";

    // Walk the final byte stream and decode one op at a time.
    usize offset = 0;
    while (offset < bytes.size())
    {
        usize opOffset = offset;
        Op op;
        if (readOp(bytes, offset, op) == false)
        {
            mOut << "[" << opOffset << "] <invalid>\n";
            return;
        }

        mOut << "[";
        mOut.writePaddedValue(opOffset, 5, TextAlignment::cRight);
        mOut << "] ";
        mOut.writePadded(getOpCodeName(op.mOpCode), 15, TextAlignment::cLeft);
        if (hasOpOperands(op.mOpCode))
        {
            mOut << " ";
            writeOperandList(op);
        }
        mOut << "\n";
    }
}

void BytecodeDump::writeFormatTemplate(const StringFormatTemplate& tmpl)
{
    mOut << "literal-bytes=" << tmpl.mLiteralByteCount << " literals=[";

    // Print the literal indices and resolved literal text.
    for (usize i = 0; i < tmpl.mLiteralIndices.size(); ++i)
    {
        if (i > 0)
        {
            mOut << ", ";
        }

        StringLiteralIdx literalIdx = tmpl.mLiteralIndices[i];
        mOut << literalIdx << ":";
        const std::vector<StringLiteral>& literals = mCtx.mBackend.mStrings.getLiterals();
        if (literalIdx < literals.size())
        {
            mOut << "\"" << escape::formatEscapedText(getStringLiteralText(mCtx.mBackend, literals[literalIdx]))
                 << "\"";
        }
        else
        {
            mOut << "<invalid>";
        }
    }

    mOut << "] args=[";

    // Print the argument kinds in template order.
    for (usize i = 0; i < tmpl.mArgKinds.size(); ++i)
    {
        if (i > 0)
        {
            mOut << ", ";
        }

        mOut << stringFormatArgKindToString(tmpl.mArgKinds[i]);
    }
    mOut << "]";
}

void BytecodeDump::writeRefOffsets(const TypeLayout& layout)
{
    TypeLayoutRefCount count = layout.getRefCount();
    if (count == 0)
    {
        mOut << "  ref-offsets: <none>\n";
        return;
    }

    mOut << "  ref-offsets: ";
    TypeLayoutRefOffsetIndex start = layout.getRefOffsetStartIndex();

    for (u32 i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            mOut << ", ";
        }

        mOut << mCtx.mBackend.mTypeLayoutTable.getRefOffset(start + i);
    }
    mOut << "\n";
}

void BytecodeDump::writeTypeMembers(const Symbol* symbol)
{
    Type* type = symbol->mType;
    if (type == nullptr)
    {
        return;
    }

    if (type->mKind == TypeKind::cClass || type->mKind == TypeKind::cStruct)
    {
        auto* aggregate = static_cast<AggregateType*>(type);
        if (aggregate->mLayout != nullptr)
        {
            mOut << "  fields:\n";

            if (aggregate->mLayout->mFields.empty())
            {
                mOut << "    <none>\n";
            }

            // Print fields in layout order so offsets and word counts are easy to inspect.
            for (usize i = 0; i < aggregate->mLayout->mFields.size(); ++i)
            {
                const FieldLayout& field = aggregate->mLayout->mFields[i];
                Symbol* fieldSymbol = field.mSymbol;

                mOut << "    [" << i << "] " << getSymbolName(fieldSymbol) << " : ";

                if (fieldSymbol != nullptr && fieldSymbol->mType != nullptr)
                {
                    mOut << typeToString(fieldSymbol->mType);
                    mOut << " words=" << layout::getStorageWordSizeForType(fieldSymbol->mType);
                }
                else
                {
                    mOut << "<null>";
                }

                mOut << " offset=" << field.mOffset << " symbol=";
                writeSymbolRef(fieldSymbol);
                mOut << "\n";
            }
        }
    }

    mOut << "  methods:\n";

    // Methods are stored on the symbol rather than the aggregate layout.
    bool hasMethods = false;

    for (Symbol* member : symbol->mMembers)
    {
        if (member == nullptr || member->mSymbolType != SymbolType::cMemberFunction)
        {
            continue;
        }

        hasMethods = true;
        mOut << "    ";

        if (member->mIndex >= 0)
        {
            mOut << "[" << member->mIndex << "] ";
        }

        mOut << getSymbolName(member);

        if (member->mType != nullptr)
        {
            mOut << " : " << typeToString(member->mType);
        }

        mOut << " symbol=";
        writeSymbolRef(member);
        mOut << "\n";
    }

    if (hasMethods == false)
    {
        mOut << "    <none>\n";
    }
}

} // namespace simlang
