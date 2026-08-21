#pragma once

#include <utility>
#include <vector>

#include "ast/nodes/astnode.h"
#include "ast/nodes/exprnodes.h"
#include "ast/nodes/paramnodes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/translationunitnode.h"
#include "ast/nodes/typespecifiernodes.h"
#include "util/arena.h"
#include "util/arenautils.h"

namespace simlang
{

template <typename Derived>
class ASTCloner
{
public:
    explicit ASTCloner(ArenaAllocator& allocator)
        : mAllocator(allocator)
    {
    }

    ASTNode* clone(ASTNode* n)
    {
        if (n == nullptr)
        {
            return nullptr;
        }

        switch (n->mNodeType)
        {
#define DISPATCH_CASE(name) \
    case NodeType::c##name: return static_cast<Derived*>(this)->clone##name(static_cast<name##Node*>(n));

#define X(name) DISPATCH_CASE(name)

#include "ast/nodes/nodetypes.def"

#undef X
#undef DISPATCH_CASE
        }

        return nullptr;
    }

    ExpressionNode* cloneExpression(ExpressionNode* n) { return static_cast<ExpressionNode*>(clone(n)); }

    StatementNode* cloneStatement(StatementNode* n) { return static_cast<StatementNode*>(clone(n)); }

    ParamNode* cloneParam(ParamNode* n) { return static_cast<ParamNode*>(clone(n)); }

    TypeSpecifierNode* cloneTypeSpecifier(TypeSpecifierNode* n) { return static_cast<TypeSpecifierNode*>(clone(n)); }

    ExpressionNode* cloneImplicitCast(ImplicitCastNode* n)
    {
        ExpressionNode* child = cloneExpression(n->mTarget);
        return cloneNode<ImplicitCastNode>(n, child);
    }

    ExpressionNode* cloneCast(CastNode* n)
    {
        TypeSpecifierNode* typeSpecifier = cloneTypeSpecifier(n->mTypeSpecifier);
        ExpressionNode* child = cloneExpression(n->mTarget);
        return cloneNode<CastNode>(n, typeSpecifier, child);
    }

    ExpressionNode* cloneIdentifier(IdentifierNode* n) { return cloneNode<IdentifierNode>(n, n->mIdentifier); }

    ExpressionNode* cloneThis(ThisNode* n) { return cloneNode<ThisNode>(n); }

    ExpressionNode* cloneIntLiteral(IntLiteralNode* n) { return cloneNode<IntLiteralNode>(n, n->mInt); }

    ExpressionNode* cloneFloatLiteral(FloatLiteralNode* n) { return cloneNode<FloatLiteralNode>(n, n->mFloat); }

    ExpressionNode* cloneBoolLiteral(BoolLiteralNode* n) { return cloneNode<BoolLiteralNode>(n, n->mBool); }

    ExpressionNode* cloneStringLiteral(StringLiteralNode* n) { return cloneNode<StringLiteralNode>(n, n->mString); }

    ExpressionNode* cloneNullLiteral(NullLiteralNode* n) { return cloneNode<NullLiteralNode>(n); }

    ExpressionNode* cloneFormatString(FormatStringNode* n)
    {
        std::vector<ExpressionNode*> args;
        args.reserve(n->mArgs.size());
        for (ExpressionNode* arg : n->mArgs)
        {
            ExpressionNode* clonedArg = cloneExpression(arg);
            args.push_back(clonedArg);
        }

        return cloneNode<FormatStringNode>(n, n->mLiterals, makeArrayView(mAllocator, args));
    }

    ExpressionNode* cloneNewObject(NewObjectNode* n)
    {
        std::vector<CallArgument> args;
        args.reserve(n->mInitializerArguments.size());
        for (const CallArgument& arg : n->mInitializerArguments)
        {
            ExpressionNode* clonedValue = cloneExpression(arg.mValue);
            args.push_back(CallArgument{arg.mSourceRange, clonedValue, arg.mIsInOut});
        }

        std::vector<FieldInitializer*> fields;
        fields.reserve(n->mFieldInitializers.size());
        for (FieldInitializer* init : n->mFieldInitializers)
        {
            ExpressionNode* value = cloneExpression(init->mValue);
            fields.push_back(mAllocator.create<FieldInitializer>(init->mSourceRange,
                                                                 init->mIdentifierRange,
                                                                 init->mIdentifier,
                                                                 value));
        }

        TypeSpecifierNode* typeSpecifier = cloneTypeSpecifier(n->mTypeSpecifier);
        return cloneNode<NewObjectNode>(n,
                                        typeSpecifier,
                                        makeArrayView(mAllocator, fields),
                                        makeArrayView(mAllocator, args),
                                        n->mConstructionKind);
    }

    ExpressionNode* cloneFunctionCall(FunctionCallNode* n)
    {
        std::vector<CallArgument> args;
        args.reserve(n->mArgs.size());
        for (const CallArgument& arg : n->mArgs)
        {
            ExpressionNode* clonedValue = cloneExpression(arg.mValue);
            args.push_back(CallArgument{arg.mSourceRange, clonedValue, arg.mIsInOut});
        }

        ExpressionNode* receiver = cloneExpression(n->mReceiver);
        return cloneNode<FunctionCallNode>(n, receiver, makeArrayView(mAllocator, args));
    }

    ExpressionNode* cloneIndexCall(IndexCallNode* n)
    {
        ExpressionNode* receiver = cloneExpression(n->mReceiver);
        ExpressionNode* index = cloneExpression(n->mIndex);
        return cloneNode<IndexCallNode>(n, receiver, index);
    }

    ExpressionNode* cloneMemberAccess(MemberAccessNode* n)
    {
        ExpressionNode* receiver = cloneExpression(n->mReceiver);
        return cloneNode<MemberAccessNode>(n, receiver, n->mMember);
    }

    ExpressionNode* cloneModuleAccess(ModuleAccessNode* n)
    {
        ExpressionNode* left = cloneExpression(n->mLeft);
        ExpressionNode* right = cloneExpression(n->mRight);
        return cloneNode<ModuleAccessNode>(n, left, right);
    }

    ExpressionNode* cloneUnaryOp(UnaryOpNode* n)
    {
        ExpressionNode* expr = cloneExpression(n->mExpr);
        return cloneNode<UnaryOpNode>(n, n->mOp, expr);
    }

    ExpressionNode* cloneBinaryOp(BinaryOpNode* n)
    {
        ExpressionNode* left = cloneExpression(n->mLeft);
        ExpressionNode* right = cloneExpression(n->mRight);
        return cloneNode<BinaryOpNode>(n, n->mOp, left, right);
    }

    ExpressionNode* cloneTernaryExpr(TernaryExprNode* n)
    {
        ExpressionNode* condition = cloneExpression(n->mCondition);
        ExpressionNode* thenExpr = cloneExpression(n->mThenExpr);
        ExpressionNode* elseExpr = cloneExpression(n->mElseExpr);
        return cloneNode<TernaryExprNode>(n, condition, thenExpr, elseExpr);
    }

    StatementNode* cloneEmptyStatement(EmptyStatementNode* n) { return cloneNode<EmptyStatementNode>(n); }

    StatementNode* cloneBlockStatement(BlockStatementNode* n)
    {
        std::vector<StatementNode*> statements;
        statements.reserve(n->mStatements.size());
        for (StatementNode* statement : n->mStatements)
        {
            StatementNode* clonedStatement = cloneStatement(statement);
            statements.push_back(clonedStatement);
        }

        return cloneNode<BlockStatementNode>(n, makeArrayView(mAllocator, statements));
    }

    StatementNode* cloneExpressionStatement(ExpressionStatementNode* n)
    {
        ExpressionNode* expression = cloneExpression(n->mExpression);
        return cloneNode<ExpressionStatementNode>(n, expression);
    }

    StatementNode* cloneAssignmentStatement(AssignmentStatementNode* n)
    {
        ExpressionNode* lhs = cloneExpression(n->mLHS);
        ExpressionNode* rhs = cloneExpression(n->mRHS);
        return cloneNode<AssignmentStatementNode>(n, lhs, rhs, n->mOp);
    }

    StatementNode* cloneVariableDeclarationStatement(VariableDeclarationStatementNode* n)
    {
        TypeSpecifierNode* typeSpec = cloneTypeSpecifier(n->mTypeSpec);
        ExpressionNode* init = cloneExpression(n->mInit);
        return cloneNode<VariableDeclarationStatementNode>(n, n->mIdentifierRange, n->mIdentifier, typeSpec, init);
    }

    StatementNode* cloneFunctionDeclarationStatement(FunctionDeclarationStatementNode* n)
    {
        std::vector<ParamNode*> params;
        params.reserve(n->mParams.size());
        for (ParamNode* param : n->mParams)
        {
            ParamNode* clonedParam = cloneParam(param);
            params.push_back(clonedParam);
        }

        TypeSpecifierNode* returnTypeSpec = cloneTypeSpecifier(n->mReturnTypeSpec);
        StatementNode* body = cloneStatement(n->mBody);
        return cloneNode<FunctionDeclarationStatementNode>(n,
                                                           n->mIdentifierRange,
                                                           n->mIdentifier,
                                                           makeArrayView(mAllocator, params),
                                                           returnTypeSpec,
                                                           body,
                                                           n->mIsInitMethod);
    }

    StatementNode* cloneTypeDeclarationStatement(TypeDeclarationStatementNode* n)
    {
        std::vector<StatementNode*> members;
        members.reserve(n->mMembers.size());
        for (StatementNode* member : n->mMembers)
        {
            StatementNode* clonedMember = cloneStatement(member);
            members.push_back(clonedMember);
        }

        std::vector<TypeSpecifierNode*> implementedInterfaces;
        implementedInterfaces.reserve(n->mImplementedInterfaces.size());
        for (TypeSpecifierNode* iface : n->mImplementedInterfaces)
        {
            TypeSpecifierNode* clonedIface = cloneTypeSpecifier(iface);
            implementedInterfaces.push_back(clonedIface);
        }

        auto* cloned = cloneNode<TypeDeclarationStatementNode>(n,
                                                               n->mIdentifierRange,
                                                               n->mIdentifier,
                                                               makeArrayView(mAllocator, members),
                                                               n->mTemplateParams,
                                                               n->mKind,
                                                               makeArrayView(mAllocator, implementedInterfaces));
        cloned->mDeclModule = n->mDeclModule;
        return cloned;
    }

    StatementNode* cloneImportDeclarationStatement(ImportDeclarationStatementNode* n)
    {
        std::vector<ImportSelectedEntry*> selected;
        selected.reserve(n->mSelected.size());
        for (ImportSelectedEntry* entry : n->mSelected)
        {
            if (entry == nullptr)
            {
                selected.push_back(nullptr);
                continue;
            }

            selected.push_back(mAllocator.create<ImportSelectedEntry>(entry->mName, entry->mAlias));
        }

        auto* cloned = cloneNode<ImportDeclarationStatementNode>(n,
                                                                 n->mPath,
                                                                 makeArrayView(mAllocator, selected),
                                                                 n->mAlias,
                                                                 n->mIsRelative);
        cloned->mResolvedModule = n->mResolvedModule;
        return cloned;
    }

    StatementNode* cloneIfBranchStatement(IfBranchStatementNode* n)
    {
        ExpressionNode* condition = cloneExpression(n->mCondition);
        StatementNode* body = cloneStatement(n->mBody);
        return cloneNode<IfBranchStatementNode>(n, condition, body);
    }

    StatementNode* cloneIfStatement(IfStatementNode* n)
    {
        std::vector<IfBranchStatementNode*> branches;
        branches.reserve(n->mBranches.size());
        for (IfBranchStatementNode* branch : n->mBranches)
        {
            StatementNode* clonedStatement = cloneStatement(branch);
            auto* clonedBranch = static_cast<IfBranchStatementNode*>(clonedStatement);
            branches.push_back(clonedBranch);
        }

        StatementNode* elseBody = cloneStatement(n->mElseBody);
        return cloneNode<IfStatementNode>(n, makeArrayView(mAllocator, branches), elseBody);
    }

    StatementNode* cloneForStatement(ForStatementNode* n)
    {
        StatementNode* init = cloneStatement(n->mInit);
        ExpressionNode* condition = cloneExpression(n->mCondition);
        StatementNode* increment = cloneStatement(n->mIncrement);
        StatementNode* body = cloneStatement(n->mBody);
        return cloneNode<ForStatementNode>(n, init, condition, increment, body);
    }

    StatementNode* cloneWhileStatement(WhileStatementNode* n)
    {
        ExpressionNode* condition = cloneExpression(n->mCondition);
        StatementNode* body = cloneStatement(n->mBody);
        return cloneNode<WhileStatementNode>(n, condition, body);
    }

    StatementNode* cloneSwitchSectionStatement(SwitchSectionStatementNode* n)
    {
        ExpressionNode* caseExpression = cloneExpression(n->mCaseExpression);
        StatementNode* body = cloneStatement(n->mBody);
        return cloneNode<SwitchSectionStatementNode>(n, caseExpression, body);
    }

    StatementNode* cloneSwitchStatement(SwitchStatementNode* n)
    {
        std::vector<SwitchSectionStatementNode*> sections;
        sections.reserve(n->mSections.size());
        for (SwitchSectionStatementNode* section : n->mSections)
        {
            StatementNode* clonedStatement = cloneStatement(section);
            auto* clonedSection = static_cast<SwitchSectionStatementNode*>(clonedStatement);
            sections.push_back(clonedSection);
        }

        ExpressionNode* expression = cloneExpression(n->mExpression);
        return cloneNode<SwitchStatementNode>(n, expression, makeArrayView(mAllocator, sections));
    }

    StatementNode* cloneReturnStatement(ReturnStatementNode* n)
    {
        ExpressionNode* expression = cloneExpression(n->mExpression);
        return cloneNode<ReturnStatementNode>(n, expression);
    }

    StatementNode* cloneBreakStatement(BreakStatementNode* n) { return cloneNode<BreakStatementNode>(n); }

    StatementNode* cloneContinueStatement(ContinueStatementNode* n) { return cloneNode<ContinueStatementNode>(n); }

    StatementNode* clonePrintStatement(PrintStatementNode* n)
    {
        ExpressionNode* expression = cloneExpression(n->mExpression);
        return cloneNode<PrintStatementNode>(n, expression);
    }

    // Parameters.
    ParamNode* cloneParamDeclaration(ParamDeclarationNode* n)
    {
        TypeSpecifierNode* typeSpec = cloneTypeSpecifier(n->mTypeSpec);
        ExpressionNode* defaultValue = cloneExpression(n->mDefaultValue);
        return cloneNode<ParamDeclarationNode>(n,
                                               n->mIdentifierRange,
                                               n->mIdentifier,
                                               typeSpec,
                                               defaultValue,
                                               n->mIsInOut);
    }

    TypeSpecifierNode* cloneNamedTypeSpecifier(NamedTypeSpecifierNode* n)
    {
        std::vector<TypeSpecifierNode*> args;
        args.reserve(n->mTypeArgs.size());
        for (TypeSpecifierNode* arg : n->mTypeArgs)
        {
            TypeSpecifierNode* clonedArg = cloneTypeSpecifier(arg);
            args.push_back(clonedArg);
        }

        ExpressionNode* nameExpression = cloneExpression(n->mNameExpression);
        return cloneNode<NamedTypeSpecifierNode>(n, nameExpression, makeArrayView(mAllocator, args));
    }

    TypeSpecifierNode* cloneSubstitutedTypeSpecifier(SubstitutedTypeSpecifierNode* n)
    {
        return cloneNode<SubstitutedTypeSpecifierNode>(n, n->mType);
    }

    TranslationUnitNode* cloneTranslationUnit(TranslationUnitNode* n)
    {
        std::vector<ASTNode*> nodes;
        nodes.reserve(n->mNodes.size());
        for (ASTNode* node : n->mNodes)
        {
            ASTNode* clonedNode = clone(node);
            nodes.push_back(clonedNode);
        }

        return cloneNode<TranslationUnitNode>(n, makeArrayView(mAllocator, nodes));
    }

protected:
    template <typename Node, typename... Args>
    Node* cloneNode(const ASTNode* original, Args&&... args) const
    {
        auto* cloned = mAllocator.create<Node>(original->mSourceRange, std::forward<Args>(args)...);
        cloned->mFlags = original->mFlags;
        return cloned;
    }

    ArenaAllocator& mAllocator;
};

} // namespace simlang
