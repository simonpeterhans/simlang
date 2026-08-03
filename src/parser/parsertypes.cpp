#include <vector>

#include "ast/nodes/exprnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnostictype.h"
#include "parser/parser.h"
#include "parser/parsercontext.h"
#include "parser/token.h"
#include "parser/tokenizer.h"
#include "parser/tokentype.h"
#include "source/sourcerange.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/scoping.h"

namespace simlang
{

bool Parser::parseTypeArgumentList(std::vector<TypeSpecifierNode*>& typeArgs)
{
    // This is similar to Parser::parseTemplateParameterList(), but we want types instead of identifiers here.
    // Enter split '>' mode.
    ScopedValueBinder splitRightAngles = mTokenizer.scopedSplitRightAngles(true);

    // If we don't have a '<', this fails immediately.
    if (expect(TokenType::cLT, nullptr, true) == false)
    {
        return false;
    }

    while (true)
    {
        // Next, we require a type.
        TypeSpecifierNode* arg = parseTypeSpec();
        if (arg == nullptr)
        {
            return false;
        }

        typeArgs.push_back(arg);

        // Then, if we have a comma, we just continue as usual.
        if (tryConsume(TokenType::cComma))
        {
            continue;
        }

        // If we don't have a comma, this has to be a '>' or an error.
        if (tryConsume(TokenType::cGT) == false)
        {
            mCtx.report<cMissingClosingDelim>(getCurrentTokenRange(), TokenType::cGT, getCurrentTokenText());
            return false;
        }

        return true;
    }
}

TypeSpecifierNode* Parser::parseTypeSpec()
{
    // Expect an identifier for the type name.
    Token lhsToken = cErrorToken;
    if (expect(TokenType::cIdentifier, &lhsToken, true) == false)
    {
        return nullptr;
    }

    // Consume the identifier and check if we have a qualified name.
    ExpressionNode* fullNode = mCtx.create<IdentifierNode>(lhsToken.getRange(), lhsToken.mIdentifier);

    while (tryConsume(TokenType::cDoubleColon))
    {
        Token rhsToken = cErrorToken;
        if (expect(TokenType::cIdentifier, &rhsToken, true) == false)
        {
            return nullptr;
        }

        auto* rhsNode = mCtx.create<IdentifierNode>(rhsToken.getRange(), rhsToken.mIdentifier);
        fullNode = mCtx.create<ModuleAccessNode>(fullNode->makeRangeTo(rhsNode), fullNode, rhsNode);
    }

    std::vector<TypeSpecifierNode*> typeArgs;
    SourceRange namedRange = fullNode->mSourceRange;

    if (check(TokenType::cLT))
    {
        if (parseTypeArgumentList(typeArgs) == false)
        {
            return nullptr;
        }

        namedRange = makeRangeToPrevious(lhsToken);
    }

    ArrayView<TypeSpecifierNode*> typeArgsView = makeArrayView(mCtx.mAllocator, typeArgs);
    return mCtx.create<NamedTypeSpecifierNode>(namedRange, fullNode, typeArgsView);
}

} // namespace simlang
