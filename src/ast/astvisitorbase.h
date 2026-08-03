#pragma once

#include "ast/nodes/astnode.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "ast/nodes/typespecifiernodes.h"

namespace simlang
{

template <typename Derived, typename ReturnType>
class ASTVisitorBase
{
public:
    ReturnType defaultReturn() { return ReturnType{}; }

    void enterNode(ASTNode*) {}

    void leaveNode(ASTNode*) {}

    ReturnType visit(ASTNode* n)
    {
        if (n == nullptr)
        {
            return static_cast<Derived*>(this)->defaultReturn();
        }

        static_cast<Derived*>(this)->enterNode(n);

        ReturnType result = static_cast<Derived*>(this)->defaultReturn();

        switch (n->mNodeType)
        {
#define DISPATCH_CASE(name) \
    case NodeType::c##name: \
    { \
        result = static_cast<Derived*>(this)->visit##name(static_cast<name##Node*>(n)); \
        break; \
    }

#define X(name) DISPATCH_CASE(name)

#include "ast/nodes/nodetypes.def"

#undef X

#undef DISPATCH_CASE
        }

        static_cast<Derived*>(this)->leaveNode(n);

        return result;
    }
};

} // namespace simlang
