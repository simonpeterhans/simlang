#include <array>
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

TypeSpecifierNode* Parser::parseFunctionTypeSpec()
{
    // Consume the "fun".
    Token funToken = consume();

    // Consume the '('.
    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Pars the params until we have a ')'.
    std::vector<FunctionTypeParameterSpecifier> params;
    while (check(TokenType::cRightParen) == false)
    {
        if (params.empty() == false)
        {
            // Consume the ',' before every param except the first.
            if (tryConsume(TokenType::cComma) == false)
            {
                mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                              std::array{TokenType::cComma, TokenType::cRightParen},
                                              getCurrentTokenText());
                return nullptr;
            }
        }

        SourceLocation paramStart = getCurrentTokenRange().getStartLoc();

        // Consume the "inout" if we have one.
        bool isInOut = tryConsume(TokenType::cInOut);

        // Parse the type specifier.
        TypeSpecifierNode* typeSpecifier = parseTypeSpec();
        if (typeSpecifier == nullptr)
        {
            return nullptr;
        }

        // Define the range and add the param.
        SourceRange paramRange{paramStart, typeSpecifier->mSourceRange.getEndLoc()};
        params.push_back(FunctionTypeParameterSpecifier{paramRange, typeSpecifier, isInOut});
    }

    // Consume the ')'.
    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    // Consume the ':'.
    if (expect(TokenType::cColon, nullptr, true) == false)
    {
        return nullptr;
    }

    // Parse the return type.
    TypeSpecifierNode* returnTypeSpecifier = parseTypeSpec();
    if (returnTypeSpecifier == nullptr)
    {
        return nullptr;
    }

    SourceRange range{funToken.getRange().getStartLoc(), returnTypeSpecifier->mSourceRange.getEndLoc()};
    return mCtx.create<FunctionTypeSpecifierNode>(range, makeArrayView(mCtx.mAllocator, params), returnTypeSpecifier);
}

TypeSpecifierNode* Parser::parseNamedTypeSpec()
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

TypeSpecifierNode* Parser::parseTypeSpec()
{
    if (check(TokenType::cFun))
    {
        return parseFunctionTypeSpec();
    }

    return parseNamedTypeSpec();
}

} // namespace simlang
