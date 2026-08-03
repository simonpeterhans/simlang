#pragma once

#include "ast/assignmentop.h"
#include "ast/nodes/astnode.h"
#include "util/arrayview.h"
#include "util/types.h"

namespace simlang
{

class Scope;
struct ExpressionNode;
struct Identifier;
struct ModuleEntry;
struct ParamNode;
struct Symbol;
struct TypeSpecifierNode;

enum StmtNodeFlags : NodeFlagType
{
    cStmtIsMutable = 1 << (cNodeFlagOffset + 0),
    cStmtIsExported = 1 << (cNodeFlagOffset + 1),
    cStmtIsPrivate = 1 << (cNodeFlagOffset + 2),
    cStmtIsInterfaceImpl = 1 << (cNodeFlagOffset + 3),
    cStmtIsTemplateInstance = 1 << (cNodeFlagOffset + 4),

    cStmtFlagsOffset = cNodeFlagOffset + 5
};

struct StatementNode : ASTNode
{
    explicit StatementNode(NodeType type, SourceRange range)
        : ASTNode(type, range)
    {
    }
};

struct EmptyStatementNode : StatementNode
{
    explicit EmptyStatementNode(SourceRange range)
        : StatementNode(NodeType::cEmptyStatement, range)
    {
    }
};

struct BlockStatementNode : StatementNode
{
    explicit BlockStatementNode(SourceRange range, ArrayView<StatementNode*> statements)
        : StatementNode(NodeType::cBlockStatement, range)
        , mStatements(statements)

    {
    }

    ArrayView<StatementNode*> mStatements;

    // Resolved scope.
    Scope* mScope = nullptr;
};

struct ExpressionStatementNode : StatementNode
{
    explicit ExpressionStatementNode(SourceRange range, ExpressionNode* expression)
        : StatementNode(NodeType::cExpressionStatement, range)
        , mExpression(expression)

    {
    }

    ExpressionNode* mExpression;
};

struct AssignmentStatementNode : StatementNode
{
    explicit AssignmentStatementNode(SourceRange range, ExpressionNode* lhs, ExpressionNode* rhs, AssignmentOp op)
        : StatementNode(NodeType::cAssignmentStatement, range)
        , mOp(op)
        , mLHS(lhs)
        , mRHS(rhs)
    {
    }

    AssignmentOp mOp;
    ExpressionNode* mLHS;
    ExpressionNode* mRHS;
};

struct VariableDeclarationStatementNode : StatementNode
{
    explicit VariableDeclarationStatementNode(SourceRange range,
                                              SourceRange identifierRange,
                                              Identifier* identifier,
                                              TypeSpecifierNode* typeSpec,
                                              ExpressionNode* init)
        : StatementNode(NodeType::cVariableDeclarationStatement, range)
        , mIdentifierRange(identifierRange)
        , mIdentifier(identifier)
        , mTypeSpec(typeSpec)
        , mInit(init)
    {
    }

    SourceRange mIdentifierRange;
    Identifier* mIdentifier;
    TypeSpecifierNode* mTypeSpec;
    ExpressionNode* mInit;

    // Resolved symbol.
    Symbol* mSymbol = nullptr;
};

struct FunctionDeclarationStatementNode : StatementNode
{
    explicit FunctionDeclarationStatementNode(SourceRange range,
                                              SourceRange identifierRange,
                                              Identifier* identifier,
                                              ArrayView<ParamNode*> params,
                                              TypeSpecifierNode* returnTypeSpec,
                                              StatementNode* body,
                                              bool isInitMethod)
        : StatementNode(NodeType::cFunctionDeclarationStatement, range)
        , mIsInitMethod(isInitMethod)
        , mIdentifierRange(identifierRange)
        , mIdentifier(identifier)
        , mParams(params)
        , mReturnTypeSpec(returnTypeSpec)
        , mBody(body)
    {
    }

    bool mIsInitMethod;
    SourceRange mIdentifierRange;
    Identifier* mIdentifier;
    ArrayView<ParamNode*> mParams;
    TypeSpecifierNode* mReturnTypeSpec;
    StatementNode* mBody;

    // Resolved symbol and scope.
    Symbol* mSymbol = nullptr;
    Scope* mScope = nullptr;

    // Fields assigned in the initializer (constructor) body.
    ArrayView<Symbol*> mInitializerAssignedFields;
};

enum class TypeDeclarationKind : u8
{
    cStruct,
    cClass,
    cInterface
};

struct TypeDeclarationStatementNode : StatementNode
{
    explicit TypeDeclarationStatementNode(SourceRange range,
                                          SourceRange identifierRange,
                                          Identifier* identifier,
                                          ArrayView<StatementNode*> members,
                                          ArrayView<Identifier*> templateParams,
                                          TypeDeclarationKind kind,
                                          ArrayView<TypeSpecifierNode*> implementedInterfaces = {})
        : StatementNode(NodeType::cTypeDeclarationStatement, range)
        , mKind(kind)
        , mIdentifierRange(identifierRange)
        , mIdentifier(identifier)
        , mMembers(members)
        , mTemplateParams(templateParams)
        , mImplementedInterfaces(implementedInterfaces)
    {
    }

    bool isTemplate() const { return mTemplateParams.empty() == false; }
    bool isStruct() const { return mKind == TypeDeclarationKind::cStruct; }
    bool isClass() const { return mKind == TypeDeclarationKind::cClass; }
    bool isInterface() const { return mKind == TypeDeclarationKind::cInterface; }

    TypeDeclarationKind mKind;
    SourceRange mIdentifierRange;
    Identifier* mIdentifier;
    ArrayView<StatementNode*> mMembers;
    ArrayView<Identifier*> mTemplateParams;
    ArrayView<TypeSpecifierNode*> mImplementedInterfaces;

    // Resolved symbol, scope, and module.
    Symbol* mSymbol = nullptr;
    Scope* mScope = nullptr;
    ModuleEntry* mDeclModule = nullptr;
};

struct ImportSelectedEntry
{
    explicit ImportSelectedEntry(Identifier* name, Identifier* alias)
        : mName(name)
        , mAlias(alias)
    {
    }

    Identifier* mName;
    Identifier* mAlias;
};

struct ImportDeclarationStatementNode : StatementNode
{
    explicit ImportDeclarationStatementNode(SourceRange range,
                                            ArrayView<Identifier*> path,
                                            ArrayView<ImportSelectedEntry*> selected,
                                            Identifier* alias,
                                            bool isRelative)
        : StatementNode(NodeType::cImportDeclarationStatement, range)
        , mIsRelative(isRelative)
        , mPath(path)
        , mSelected(selected)
        , mAlias(alias)
    {
    }

    bool mIsRelative;
    ArrayView<Identifier*> mPath;
    ArrayView<ImportSelectedEntry*> mSelected;
    Identifier* mAlias;

    // Resolved module.
    ModuleEntry* mResolvedModule = nullptr;
};

struct IfBranchStatementNode : StatementNode
{
    explicit IfBranchStatementNode(SourceRange range, ExpressionNode* condition, StatementNode* body)
        : StatementNode(NodeType::cIfBranchStatement, range)
        , mCondition(condition)
        , mBody(body)
    {
    }

    ExpressionNode* mCondition;
    StatementNode* mBody;
};

struct IfStatementNode : StatementNode
{
    explicit IfStatementNode(SourceRange range, ArrayView<IfBranchStatementNode*> branches, StatementNode* elseBody)
        : StatementNode(NodeType::cIfStatement, range)
        , mBranches(branches)
        , mElseBody(elseBody)
    {
    }

    ArrayView<IfBranchStatementNode*> mBranches;
    StatementNode* mElseBody;
};

struct WhileStatementNode : StatementNode
{
    explicit WhileStatementNode(SourceRange range, ExpressionNode* condition, StatementNode* body)
        : StatementNode(NodeType::cWhileStatement, range)
        , mCondition(condition)
        , mBody(body)
    {
    }

    ExpressionNode* mCondition;
    StatementNode* mBody;
};

struct SwitchSectionStatementNode : StatementNode
{
    explicit SwitchSectionStatementNode(SourceRange range,
                                        ExpressionNode* caseExpression,
                                        ArrayView<StatementNode*> statements)
        : StatementNode(NodeType::cSwitchSectionStatement, range)
        , mCaseExpression(caseExpression)
        , mStatements(statements)
    {
    }

    // Nullptr means this is the default section.
    ExpressionNode* mCaseExpression;
    ArrayView<StatementNode*> mStatements;

    // Resolved constexpr value for case labels.
    i32 mCaseValue = 0;
};

struct SwitchStatementNode : StatementNode
{
    explicit SwitchStatementNode(SourceRange range,
                                 ExpressionNode* expression,
                                 ArrayView<SwitchSectionStatementNode*> sections)
        : StatementNode(NodeType::cSwitchStatement, range)
        , mExpression(expression)
        , mSections(sections)
    {
    }

    ExpressionNode* mExpression;
    ArrayView<SwitchSectionStatementNode*> mSections;
};

struct ForStatementNode : StatementNode
{
    explicit ForStatementNode(SourceRange range,
                              StatementNode* init,
                              ExpressionNode* condition,
                              StatementNode* increment,
                              StatementNode* body)
        : StatementNode(NodeType::cForStatement, range)
        , mInit(init)
        , mCondition(condition)
        , mIncrement(increment)
        , mBody(body)
    {
    }

    StatementNode* mInit;
    ExpressionNode* mCondition;
    StatementNode* mIncrement;
    StatementNode* mBody;

    // Resolved scope.
    Scope* mScope = nullptr;
};

struct ReturnStatementNode : StatementNode
{
    explicit ReturnStatementNode(SourceRange range, ExpressionNode* expression)
        : StatementNode(NodeType::cReturnStatement, range)
        , mExpression(expression)

    {
    }

    ExpressionNode* mExpression;
};

struct BreakStatementNode : StatementNode
{
    explicit BreakStatementNode(SourceRange range)
        : StatementNode(NodeType::cBreakStatement, range)
    {
    }
};

struct ContinueStatementNode : StatementNode
{
    explicit ContinueStatementNode(SourceRange range)
        : StatementNode(NodeType::cContinueStatement, range)
    {
    }
};

struct PrintStatementNode : StatementNode
{
    explicit PrintStatementNode(SourceRange range, ExpressionNode* expr)
        : StatementNode(NodeType::cPrintStatement, range)
        , mExpression(expr)
    {
    }

    ExpressionNode* mExpression;
};

} // namespace simlang
