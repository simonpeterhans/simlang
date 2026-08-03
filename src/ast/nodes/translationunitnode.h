#pragma once

#include "ast/nodes/astnode.h"
#include "util/arrayview.h"

namespace simlang
{

class Scope;
struct ModuleEntry;

struct TranslationUnitNode : ASTNode
{
    explicit TranslationUnitNode(SourceRange range, ArrayView<ASTNode*> nodes)
        : ASTNode(NodeType::cTranslationUnit, range)
        , mNodes(nodes)
    {
    }

    ArrayView<ASTNode*> mNodes;

    // Resolved scope and module entry.
    Scope* mScope = nullptr;
    ModuleEntry* mModuleEntry = nullptr;
};

} // namespace simlang
