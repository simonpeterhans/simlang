#pragma once

#include "ast/nodes/nodetypes.h"
#include "source/sourcerange.h"
#include "util/flags.h"
#include "util/types.h"

namespace simlang
{

using NodeFlagType = u8;

enum NodeFlags : NodeFlagType
{
    cNodeFlagOffset = 0
};

struct ASTNode
{
    explicit ASTNode(NodeType type, SourceRange range)
        : mSourceRange(range)
        , mNodeType(type)
    {
    }

    // Never ever add virtual stuff here for release.
    // virtual void dummy() const {}

    SourceRange makeRangeTo(const ASTNode* other) const
    {
        return SourceRange{mSourceRange.getStartLoc(), other->mSourceRange.getEndLoc()};
    }

    SourceRange mSourceRange;
    const NodeType mNodeType;
    FlagSet<NodeFlagType> mFlags;
};

} // namespace simlang
