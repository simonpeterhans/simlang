#pragma once

namespace simlang
{

class TextSink;

struct BytecodeDumpOptions
{
    TextSink* mSink = nullptr;
    bool mIncludeFunctionTable = true;
    bool mIncludeStringTable = true;
    bool mIncludeTypeTable = true;
    bool mIncludeProgram = true;
    bool mIncludeRawBytecode = false;
    bool mIncludeSourceLines = true;
    bool mIncludeAnnotations = true;
};

} // namespace simlang
