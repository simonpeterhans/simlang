#pragma once

#include "backend/bytecode/bytecodedumpoptions.h"

namespace simlang
{

struct CompilerContext;

class BackendDriver
{
public:
    explicit BackendDriver(CompilerContext& ctx);

    bool buildCode();
    void setOptimize(bool optimize) { mOptimize = optimize; }
    void setBytecodeDumpOptions(const BytecodeDumpOptions& options) { mBytecodeDumpOptions = options; }

private:
    bool doAggregateLayout();
    bool doSymbolLayout();
    bool doMainValidation();
    bool doConstDataLayout();
    bool doTypeLayoutMetadata();
    bool doCodeGen();
    void doOptimization();
    bool doEmitBytecode();
    void doWriteDump();

    CompilerContext& mCtx;
    BytecodeDumpOptions mBytecodeDumpOptions;
    bool mOptimize = true;
};

} // namespace simlang
