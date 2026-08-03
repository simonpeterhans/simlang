#pragma once

#include <vector>

#include "util/types.h"

namespace simlang
{

class BytecodeChunk;
class SourceRange;
struct BackendState;
struct Op;

struct OptimizerStats
{
    u32 mRewrittenOps = 0;
    u32 mRemovedOps = 0;
    i32 mBytesSaved = 0;
};

class Optimizer
{
public:
    explicit Optimizer(BackendState& ctx);

    OptimizerStats optimize();

private:
    void optimizeCode(BytecodeChunk& code);

    bool removeUnreachableBlocks();
    bool mergeConsecutiveLabels();
    bool invertConditionalSkipToNext();
    bool removeJumpToNext();
    bool dropUnusedLabels();
    bool compactLocalInc();
    bool keepStoredValue();

    isize previousLive(usize idx) const;
    usize nextLive(usize idx) const;
    bool nextLiveOp(usize from, usize& outIndex, Op& outOp) const;
    void recordRemovedOp(Op op);
    void recordRewrittenOp(Op before, Op after);
    void prepare();
    void compact();
    bool iterate();

    std::vector<isize> mLabelToOpIndex;
    std::vector<u8> mLabelUsed;
    std::vector<u8> mOpMarked;

    BackendState& mCtx;
    BytecodeChunk* mChunk = nullptr;
    std::vector<Op>* mOps = nullptr;
    std::vector<SourceRange>* mSourceRanges = nullptr;
    OptimizerStats mStats;
    u32 mLabelCount = 0;
};

} // namespace simlang
