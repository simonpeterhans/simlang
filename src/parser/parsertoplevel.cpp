#include <array>
#include <vector>

#include "ast/nodes/astnode.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "parser/parser.h"
#include "parser/parsercontext.h"
#include "parser/token.h"
#include "parser/tokenizer.h"
#include "parser/tokentype.h"
#include "source/sourcerange.h"
#include "symbol/identifiertable.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/scoping.h"

namespace simlang
{

struct Identifier;
struct TypeSpecifierNode;

ParamNode* Parser::parseFunctionParameter()
{
    Token nameToken = cErrorToken;
    if (expect(TokenType::cIdentifier, &nameToken, true) == false)
    {
        return nullptr;
    }

    bool isInOut = false;
    if (expect(TokenType::cColon, nullptr, true) == false)
    {
        return nullptr;
    }

    if (tryConsume(TokenType::cInOut))
    {
        isInOut = true;
    }

    TypeSpecifierNode* typeSpec = parseTypeSpec();
    if (typeSpec == nullptr)
    {
        return nullptr;
    }

    return mCtx.create<ParamDeclarationNode>(makeRangeToPrevious(nameToken),
                                             nameToken.getRange(),
                                             nameToken.mIdentifier,
                                             typeSpec,
                                             nullptr,
                                             isInOut);
}

bool Parser::parseTemplateParameterList(std::vector<Identifier*>& params)
{
    // The list can be something like <T1, T2<T3>>.
    // We need to tell the tokenizer that we're in a different mode.
    // This is because we want to treat ">>" and ">>=" as '>' '>' ('='), since we're not expecting >> or >>= here.
    auto splitRightAnglesScope = mTokenizer.scopedSplitRightAngles(true);

    // If we don't have a '<', this fails immediately.
    if (expect(TokenType::cLT, nullptr, true) == false)
    {
        return false;
    }

    while (true)
    {
        // Next, we require an identifier.
        Token paramToken = cErrorToken;
        if (expect(TokenType::cIdentifier, &paramToken, true) == false)
        {
            return false;
        }

        // We immediately fail if the identifier is already used for another param.
        for (const Identifier* existing : params)
        {
            if (existing == paramToken.mIdentifier)
            {
                mCtx.report<cDuplicateTemplateParameter>(paramToken.getRange(), paramToken.mIdentifier);
                return false;
            }
        }

        params.push_back(paramToken.mIdentifier);

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

ImportSelectedEntry* Parser::parseImportSelectedEntry()
{
    Token entryToken = getCurrentToken();
    if (expect(TokenType::cIdentifier, &entryToken, true) == false)
    {
        return nullptr;
    }

    Identifier* asAlias = nullptr;
    if (tryConsume(TokenType::cAs))
    {
        Token aliasToken = getCurrentToken();
        if (expect(TokenType::cIdentifier, &aliasToken, true) == false)
        {
            return nullptr;
        }

        asAlias = aliasToken.mIdentifier;
    }

    return mCtx.create<ImportSelectedEntry>(entryToken.mIdentifier, asAlias);
}

StatementNode* Parser::parseFunctionDeclaration()
{
    Token keywordToken = consume();
    TypeSpecifierNode* retType = nullptr;
    StatementNode* body = nullptr;
    Identifier* name = nullptr;
    SourceRange identifierRange = keywordToken.getRange();
    std::vector<ParamNode*> params;
    params.reserve(8);

    auto buildNode = [&]()
    {
        ArrayView<ParamNode*> paramsView = makeArrayView(mCtx.mAllocator, params);
        SourceRange range = (body != nullptr) ? makeRange(keywordToken, body) : makeRangeToPrevious(keywordToken);
        return mCtx
            .create<FunctionDeclarationStatementNode>(range, identifierRange, name, paramsView, retType, body, false);
    };

    Token nameToken = cErrorToken;
    if (expect(TokenType::cIdentifier, &nameToken, true) == false)
    {
        return nullptr;
    }

    name = nameToken.mIdentifier;
    identifierRange = nameToken.getRange();

    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    while (check(TokenType::cRightParen) == false)
    {
        if (params.empty() == false)
        {
            if (tryConsume(TokenType::cComma))
            {
                // Keep parsing.
            }
            else
            {
                mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                              std::array{TokenType::cComma, TokenType::cRightParen},
                                              getCurrentTokenText());
                return nullptr;
            }
        }

        ParamNode* param = parseFunctionParameter();
        if (param == nullptr)
        {
            return nullptr;
        }
        params.push_back(param);
    }

    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    if (expect(TokenType::cColon, nullptr, true) == false)
    {
        return nullptr;
    }

    retType = parseTypeSpec();
    if (retType == nullptr)
    {
        return nullptr;
    }

    if (mInInterface)
    {
        if (expectSemi() == false)
        {
            return nullptr;
        }

        return buildNode();
    }

    if (check(TokenType::cLeftBrace) == false)
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cLeftBrace, getCurrentTokenText());
        return nullptr;
    }

    body = parseBlock();
    if (body == nullptr)
    {
        return nullptr;
    }

    return buildNode();
}

StatementNode* Parser::parseInitializerDeclaration()
{
    Token initToken = consume();
    StatementNode* body = nullptr;
    std::vector<ParamNode*> params;
    params.reserve(8);

    auto buildNode = [&]()
    {
        ArrayView<ParamNode*> paramsView = makeArrayView(mCtx.mAllocator, params);
        SourceRange range = (body != nullptr) ? makeRange(initToken, body) : makeRangeToPrevious(initToken);
        return mCtx.create<FunctionDeclarationStatementNode>(range,
                                                             initToken.getRange(),
                                                             initToken.mIdentifier,
                                                             paramsView,
                                                             nullptr,
                                                             body,
                                                             true);
    };

    if (expect(TokenType::cLeftParen, nullptr, true) == false)
    {
        return nullptr;
    }

    while (check(TokenType::cRightParen) == false)
    {
        if (params.empty() == false)
        {
            if (tryConsume(TokenType::cComma))
            {
                // Keep parsing.
            }
            else
            {
                mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                              std::array{TokenType::cComma, TokenType::cRightParen},
                                              getCurrentTokenText());
                return nullptr;
            }
        }

        ParamNode* param = parseFunctionParameter();
        if (param == nullptr)
        {
            return nullptr;
        }
        params.push_back(param);
    }

    if (expect(TokenType::cRightParen, nullptr, true) == false)
    {
        return nullptr;
    }

    if (check(TokenType::cLeftBrace) == false)
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cLeftBrace, getCurrentTokenText());
        return nullptr;
    }

    body = parseBlock();
    if (body == nullptr)
    {
        return nullptr;
    }

    return buildNode();
}

StatementNode* Parser::parseTypeDeclaration()
{
    Token typeToken = consume();

    TypeDeclarationKind declarationKind = TypeDeclarationKind::cStruct;
    if (typeToken.mType == TokenType::cClass)
    {
        declarationKind = TypeDeclarationKind::cClass;
    }
    else if (typeToken.mType == TokenType::cInterface)
    {
        declarationKind = TypeDeclarationKind::cInterface;
    }

    bool isClass = (declarationKind == TypeDeclarationKind::cClass);
    bool isInterface = (declarationKind == TypeDeclarationKind::cInterface);

    ScopedValueBinder svb{mInTypeDeclaration, true};
    ScopedValueBinder cvb{mInClass, isClass};
    ScopedValueBinder ivb{mInInterface, isInterface};

    std::vector<StatementNode*> memberDeclarations;
    memberDeclarations.reserve(16);
    std::vector<TypeSpecifierNode*> declaredInterfaceSpecs;
    declaredInterfaceSpecs.reserve(4);

    Token nameToken = cErrorToken;
    if (expect(TokenType::cIdentifier, &nameToken, true) == false)
    {
        return nullptr;
    }

    if (isInterface == false && getCurrentTokenType() == TokenType::cImpl)
    {
        Token implToken = consume();
        const bool keepDeclaredInterfaces = isClass;
        if (isClass == false)
        {
            mCtx.report<cInvalidImplClause>(implToken.getRange());
        }

        while (true)
        {
            TypeSpecifierNode* ifaceSpec = parseTypeSpec();
            if (ifaceSpec == nullptr)
            {
                return nullptr;
            }

            if (keepDeclaredInterfaces)
            {
                declaredInterfaceSpecs.push_back(ifaceSpec);
            }

            if (tryConsume(TokenType::cComma) == false)
            {
                break;
            }
        }
    }

    if (tryConsume(TokenType::cLeftBrace) == false)
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(), TokenType::cLeftBrace, getCurrentTokenText());
        return nullptr;
    }

    while (getCurrentTokenType() != TokenType::cRightBrace && getCurrentTokenType() != TokenType::cEOF)
    {
        if (StatementNode* member = parseTypeMember())
        {
            memberDeclarations.push_back(member);
        }
    }

    if (expect(TokenType::cRightBrace, nullptr, true) == false)
    {
        return nullptr;
    }

    ArrayView<StatementNode*> membersView = makeArrayView(mCtx.mAllocator, memberDeclarations);
    return mCtx.create<TypeDeclarationStatementNode>(makeRangeToPrevious(typeToken),
                                                     nameToken.getRange(),
                                                     nameToken.mIdentifier,
                                                     membersView,
                                                     ArrayView<Identifier*>{},
                                                     declarationKind,
                                                     makeArrayView(mCtx.mAllocator, declaredInterfaceSpecs));
}

StatementNode* Parser::parseTemplateDeclaration()
{
    // This is simply a type declaration preceded by something like:
    // template<T1, T2<T3>>

    // Consume the "template" token.
    Token templateToken = consume();

    // Parse the generic parameters.
    std::vector<Identifier*> params;
    params.reserve(4);

    if (parseTemplateParameterList(params) == false)
    {
        return nullptr;
    }

    // Now this has to be followed by a type declaration keyword.
    if (getCurrentTokenType() != TokenType::cStruct && getCurrentTokenType() != TokenType::cClass &&
        getCurrentTokenType() != TokenType::cInterface)
    {
        mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                      std::array{TokenType::cStruct, TokenType::cClass, TokenType::cInterface},
                                      getCurrentTokenText());
        return nullptr;
    }

    // Parse the type declaration as usual.
    StatementNode* decl = parseTypeDeclaration();
    if (decl == nullptr || decl->mNodeType != NodeType::cTypeDeclarationStatement)
    {
        return nullptr;
    }

    // Make the type declaration node and fill in the template params.
    auto* typeDecl = static_cast<TypeDeclarationStatementNode*>(decl);
    typeDecl->mTemplateParams = makeArrayView(mCtx.mAllocator, params);
    typeDecl->mSourceRange = makeRange(templateToken, typeDecl);

    return typeDecl;
}

StatementNode* Parser::parseTypeMember()
{
    StatementNode* result = nullptr;
    Token startToken = getCurrentToken();
    FlagSet<NodeFlagType> memberFlags;
    parseMemberSpecifiers(memberFlags);

    if (getCurrentTokenType() == TokenType::cExport)
    {
        mCtx.report<cExportOnNonTopLevel>(getCurrentTokenRange());
        recover(ParseLevel::cMember, RecoveryMode::cMalformedStart);
        return nullptr;
    }

    if (mInInterface && memberFlags.test(cStmtIsPrivate))
    {
        mCtx.report<cPrivateInterfaceMember>(startToken.getRange());
        memberFlags.set(cStmtIsPrivate, false);
    }

    if (mInInterface && memberFlags.test(cStmtIsInterfaceImpl))
    {
        mCtx.report<cInvalidImplTarget>(startToken.getRange());
        memberFlags.set(cStmtIsInterfaceImpl, false);
    }

    if (mInInterface && getCurrentTokenType() != TokenType::cFun)
    {
        mCtx.report<cInvalidDeclarationStart>(getCurrentTokenRange(), getCurrentTokenText());
        recover(ParseLevel::cMember, RecoveryMode::cMalformedStart);
        return nullptr;
    }

    switch (getCurrentTokenType())
    {
        case TokenType::cVar:
        case TokenType::cConst:
        {
            result = parseVariableDeclaration(true);
            break;
        }
        case TokenType::cFun:
        {
            result = parseFunctionDeclaration();
            break;
        }
        case TokenType::cIdentifier:
        {
            if (getCurrentToken().mIdentifier == mCtx.mIdentifiers.getInitIdentifier())
            {
                result = parseInitializerDeclaration();
                break;
            }

            // If this wasn't an init identifier, this is definitely an error.
            [[fallthrough]];
        }
        default:
        {
            mCtx.report<cInvalidDeclarationStart>(getCurrentTokenRange(), getCurrentTokenText());
            recover(ParseLevel::cMember, RecoveryMode::cMalformedStart);
            return nullptr;
        }
    }

    if (result == nullptr)
    {
        recover(ParseLevel::cMember, RecoveryMode::cAfterFailure);
        return nullptr;
    }

    if (memberFlags.test(cStmtIsInterfaceImpl))
    {
        bool isMethod = false;
        if (result->mNodeType == NodeType::cFunctionDeclarationStatement)
        {
            auto* fun = static_cast<FunctionDeclarationStatementNode*>(result);
            isMethod = (fun->mIsInitMethod == false);
        }

        if (mInClass == false || isMethod == false)
        {
            mCtx.report<cInvalidImplTarget>(startToken.getRange());
            memberFlags.set(cStmtIsInterfaceImpl, false);
        }
    }

    if (memberFlags.bits() != 0)
    {
        result->mFlags.add(memberFlags);
        result->mSourceRange = makeRange(startToken, result);
    }

    return result;
}

bool Parser::parseQualifiedName(std::vector<Identifier*>& path)
{
    Token token = cErrorToken;
    if (expect(TokenType::cIdentifier, &token, true) == false)
    {
        return false;
    }

    path.push_back(token.mIdentifier);
    while (tryConsume(TokenType::cDoubleColon))
    {
        Token next = cErrorToken;
        if (expect(TokenType::cIdentifier, &next, true) == false)
        {
            return false;
        }

        path.push_back(next.mIdentifier);
    }

    return true;
}

StatementNode* Parser::parseImportDeclaration()
{
    Token importToken = consume();
    bool isRelative = tryConsume(TokenType::cPeriod);

    std::vector<Identifier*> path;
    path.reserve(8);
    std::vector<ImportSelectedEntry*> selected;
    Identifier* alias = nullptr;

    auto buildNode = [&]()
    {
        ArrayView<Identifier*> pathView = makeArrayView(mCtx.mAllocator, path);
        ArrayView<ImportSelectedEntry*> selectedView = makeArrayView(mCtx.mAllocator, selected);
        return mCtx.create<ImportDeclarationStatementNode>(makeRangeToPrevious(importToken),
                                                           pathView,
                                                           selectedView,
                                                           alias,
                                                           isRelative);
    };

    if (parseQualifiedName(path) == false)
    {
        return nullptr;
    }

    if (tryConsume(TokenType::cLeftBrace))
    {
        while (check(TokenType::cRightBrace) == false)
        {
            if (selected.empty() == false)
            {
                if (tryConsume(TokenType::cComma))
                {
                    // Keep parsing.
                }
                else
                {
                    mCtx.report<cUnexpectedToken>(getCurrentTokenRange(),
                                                  std::array{TokenType::cComma, TokenType::cRightBrace},
                                                  getCurrentTokenText());
                    return nullptr;
                }
            }

            ImportSelectedEntry* entry = parseImportSelectedEntry();
            if (entry == nullptr)
            {
                return nullptr;
            }
            selected.push_back(entry);
        }

        if (expect(TokenType::cRightBrace, nullptr, true) == false)
        {
            return nullptr;
        }
    }

    if (selected.empty() && tryConsume(TokenType::cAs))
    {
        Token aliasToken = cErrorToken;
        if (expect(TokenType::cIdentifier, &aliasToken, true) == false)
        {
            return nullptr;
        }

        alias = aliasToken.mIdentifier;
    }

    if (expectSemi() == false)
    {
        return nullptr;
    }

    return buildNode();
}

StatementNode* Parser::parseTopLevelDeclaration()
{
    Token startToken = getCurrentToken();
    FlagSet<NodeFlagType> exportFlags;
    parseExportSpecifiers(exportFlags);

    TokenType tt = getCurrentTokenType();
    StatementNode* result = nullptr;
    bool canExport = true;

    switch (tt)
    {
        case TokenType::cImport:
        {
            if (exportFlags.test(cStmtIsExported))
            {
                mCtx.report<cInvalidExportTarget>(startToken.getRange(), getCurrentTokenText());
            }
            canExport = false;
            result = parseImportDeclaration();
            break;
        }
        case TokenType::cVar:
        case TokenType::cConst:
        {
            result = parseVariableDeclaration(true);
            break;
        }
        case TokenType::cFun:
        {
            result = parseFunctionDeclaration();
            break;
        }
        case TokenType::cStruct:
        case TokenType::cClass:
        case TokenType::cInterface:
        {
            result = parseTypeDeclaration();
            break;
        }
        case TokenType::cTemplate:
        {
            result = parseTemplateDeclaration();
            break;
        }
        default:
        {
            if (exportFlags.test(cStmtIsExported))
            {
                mCtx.report<cInvalidExportTarget>(startToken.getRange(), getCurrentTokenText());
            }
            else
            {
                mCtx.report<cInvalidDeclarationStart>(getCurrentTokenRange(), getCurrentTokenText());
            }
            recover(ParseLevel::cTopLevel, RecoveryMode::cMalformedStart);
            return nullptr;
        }
    }

    if (result == nullptr)
    {
        recover(ParseLevel::cTopLevel, RecoveryMode::cAfterFailure);
        return nullptr;
    }

    if (exportFlags.test(cStmtIsExported) && canExport)
    {
        result->mFlags.add(exportFlags);
        result->mSourceRange = makeRange(startToken, result);
    }

    return result;
}

bool Parser::parseCode(TranslationUnitNode*& out)
{
    out = nullptr;
    DiagnosticCheckpoint checkpoint = mCtx.mDiag.createCheckpoint();

    std::vector<ASTNode*> nodes;
    nodes.reserve(1024);

    // Scan the first token so we can get started.
    mCurrentToken = mTokenizer.scanNextToken();

    // Keep the first token for range purposes.
    Token startToken = getCurrentToken();

    while (check(TokenType::cEOF) == false)
    {
        if (StatementNode* decl = parseTopLevelDeclaration())
        {
            nodes.push_back(decl);
        }
    }

    SourceRange range = makeRangeToPrevious(startToken);

    // Take what we have and alloc arena stuff.
    ArrayView<ASTNode*> nodesView = makeArrayView(mCtx.mAllocator, nodes);

    out = mCtx.create<TranslationUnitNode>(range, nodesView);
    return out != nullptr && mCtx.mDiag.hasNoErrorsSince(checkpoint);
}

} // namespace simlang
