#pragma once

#include <type_traits>
#include <utility>

#include "ast/astvisitorbase.h"
#include "util/arena.h"
#include "util/arrayview.h"

namespace simlang
{

template <typename Derived>
class ASTCloner : public ASTVisitorBase<Derived, ASTNode*>
{
public:
    explicit ASTCloner(ArenaAllocator& allocator)
        : mAllocator(allocator)
    {
    }

    ASTNode* clone(ASTNode* n) { return this->visit(n); }

    ExpressionNode* cloneExpression(ExpressionNode* n) { return static_cast<ExpressionNode*>(clone(n)); }

    StatementNode* cloneStatement(StatementNode* n) { return static_cast<StatementNode*>(clone(n)); }

    ParamNode* cloneParam(ParamNode* n) { return static_cast<ParamNode*>(clone(n)); }

    TypeSpecifierNode* cloneTypeSpecifier(TypeSpecifierNode* n) { return static_cast<TypeSpecifierNode*>(clone(n)); }

    ExpressionNode* visitImplicitCast(ImplicitCastNode* n) { return rebuild<ImplicitCastNode>(n, n->mTarget); }

    ExpressionNode* visitCast(CastNode* n) { return rebuild<CastNode>(n, n->mTypeSpecifier, n->mTarget); }

    ExpressionNode* visitIdentifier(IdentifierNode* n) { return rebuild<IdentifierNode>(n, n->mIdentifier); }

    ExpressionNode* visitThis(ThisNode* n) { return rebuild<ThisNode>(n); }

    ExpressionNode* visitIntLiteral(IntLiteralNode* n) { return rebuild<IntLiteralNode>(n, n->mInt); }

    ExpressionNode* visitFloatLiteral(FloatLiteralNode* n) { return rebuild<FloatLiteralNode>(n, n->mFloat); }

    ExpressionNode* visitBoolLiteral(BoolLiteralNode* n) { return rebuild<BoolLiteralNode>(n, n->mBool); }

    ExpressionNode* visitStringLiteral(StringLiteralNode* n) { return rebuild<StringLiteralNode>(n, n->mString); }

    ExpressionNode* visitNullLiteral(NullLiteralNode* n) { return rebuild<NullLiteralNode>(n); }

    ExpressionNode* visitFormatString(FormatStringNode* n)
    {
        return rebuild<FormatStringNode>(n, n->mLiterals, n->mArgs);
    }

    ExpressionNode* visitNewObject(NewObjectNode* n)
    {
        return rebuild<NewObjectNode>(n,
                                      n->mTypeSpecifier,
                                      n->mFieldInitializers,
                                      n->mInitializerArguments,
                                      n->mConstructionKind);
    }

    ExpressionNode* visitLambda(LambdaNode* n)
    {
        return rebuild<LambdaNode>(n, n->mParams, n->mReturnTypeSpec, n->mBody);
    }

    ExpressionNode* visitFunctionCall(FunctionCallNode* n)
    {
        return rebuild<FunctionCallNode>(n, n->mReceiver, n->mArgs);
    }

    ExpressionNode* visitIndexCall(IndexCallNode* n) { return rebuild<IndexCallNode>(n, n->mReceiver, n->mIndex); }

    ExpressionNode* visitMemberAccess(MemberAccessNode* n)
    {
        return rebuild<MemberAccessNode>(n, n->mReceiver, n->mMember);
    }

    ExpressionNode* visitModuleAccess(ModuleAccessNode* n) { return rebuild<ModuleAccessNode>(n, n->mLeft, n->mRight); }

    ExpressionNode* visitUnaryOp(UnaryOpNode* n) { return rebuild<UnaryOpNode>(n, n->mOp, n->mExpr); }

    ExpressionNode* visitBinaryOp(BinaryOpNode* n) { return rebuild<BinaryOpNode>(n, n->mOp, n->mLeft, n->mRight); }

    ExpressionNode* visitTernaryExpr(TernaryExprNode* n)
    {
        return rebuild<TernaryExprNode>(n, n->mCondition, n->mThenExpr, n->mElseExpr);
    }

    StatementNode* visitEmptyStatement(EmptyStatementNode* n) { return rebuild<EmptyStatementNode>(n); }

    StatementNode* visitBlockStatement(BlockStatementNode* n) { return rebuild<BlockStatementNode>(n, n->mStatements); }

    StatementNode* visitExpressionStatement(ExpressionStatementNode* n)
    {
        return rebuild<ExpressionStatementNode>(n, n->mExpression);
    }

    StatementNode* visitAssignmentStatement(AssignmentStatementNode* n)
    {
        return rebuild<AssignmentStatementNode>(n, n->mLHS, n->mRHS, n->mOp);
    }

    StatementNode* visitVariableDeclarationStatement(VariableDeclarationStatementNode* n)
    {
        return rebuild<VariableDeclarationStatementNode>(n,
                                                         n->mIdentifierRange,
                                                         n->mIdentifier,
                                                         n->mTypeSpec,
                                                         n->mInit);
    }

    StatementNode* visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* n)
    {
        return rebuild<FunctionDeclarationStatementNode>(n,
                                                         n->mIdentifierRange,
                                                         n->mIdentifier,
                                                         n->mParams,
                                                         n->mReturnTypeSpec,
                                                         n->mBody,
                                                         n->mIsInitMethod);
    }

    StatementNode* visitTypeDeclarationStatement(TypeDeclarationStatementNode* n)
    {
        auto* cloned = rebuild<TypeDeclarationStatementNode>(n,
                                                             n->mIdentifierRange,
                                                             n->mIdentifier,
                                                             n->mMembers,
                                                             n->mTemplateParams,
                                                             n->mKind,
                                                             n->mImplementedInterfaces);
        cloned->mDeclModule = n->mDeclModule;
        return cloned;
    }

    StatementNode* visitImportDeclarationStatement(ImportDeclarationStatementNode* n)
    {
        auto* cloned = rebuild<ImportDeclarationStatementNode>(n, n->mPath, n->mSelected, n->mAlias, n->mIsRelative);
        cloned->mResolvedModule = n->mResolvedModule;
        return cloned;
    }

    StatementNode* visitIfBranchStatement(IfBranchStatementNode* n)
    {
        return rebuild<IfBranchStatementNode>(n, n->mCondition, n->mBody);
    }

    StatementNode* visitIfStatement(IfStatementNode* n)
    {
        return rebuild<IfStatementNode>(n, n->mBranches, n->mElseBody);
    }

    StatementNode* visitForStatement(ForStatementNode* n)
    {
        return rebuild<ForStatementNode>(n, n->mInit, n->mCondition, n->mIncrement, n->mBody);
    }

    StatementNode* visitWhileStatement(WhileStatementNode* n)
    {
        return rebuild<WhileStatementNode>(n, n->mCondition, n->mBody);
    }

    StatementNode* visitSwitchSectionStatement(SwitchSectionStatementNode* n)
    {
        return rebuild<SwitchSectionStatementNode>(n, n->mCaseExpression, n->mBody);
    }

    StatementNode* visitSwitchStatement(SwitchStatementNode* n)
    {
        return rebuild<SwitchStatementNode>(n, n->mExpression, n->mSections);
    }

    StatementNode* visitReturnStatement(ReturnStatementNode* n)
    {
        return rebuild<ReturnStatementNode>(n, n->mExpression);
    }

    StatementNode* visitBreakStatement(BreakStatementNode* n) { return rebuild<BreakStatementNode>(n); }

    StatementNode* visitContinueStatement(ContinueStatementNode* n) { return rebuild<ContinueStatementNode>(n); }

    StatementNode* visitPrintStatement(PrintStatementNode* n) { return rebuild<PrintStatementNode>(n, n->mExpression); }

    // Parameters.
    ParamNode* visitParamDeclaration(ParamDeclarationNode* n)
    {
        return rebuild<ParamDeclarationNode>(n,
                                             n->mIdentifierRange,
                                             n->mIdentifier,
                                             n->mTypeSpec,
                                             n->mDefaultValue,
                                             n->mIsInOut);
    }

    TypeSpecifierNode* visitNamedTypeSpecifier(NamedTypeSpecifierNode* n)
    {
        return rebuild<NamedTypeSpecifierNode>(n, n->mNameExpression, n->mTypeArgs);
    }

    TypeSpecifierNode* visitFunctionTypeSpecifier(FunctionTypeSpecifierNode* n)
    {
        return rebuild<FunctionTypeSpecifierNode>(n, n->mParams, n->mReturnTypeSpecifier);
    }

    TypeSpecifierNode* visitSubstitutedTypeSpecifier(SubstitutedTypeSpecifierNode* n)
    {
        return rebuild<SubstitutedTypeSpecifierNode>(n, n->mType);
    }

    TranslationUnitNode* visitTranslationUnit(TranslationUnitNode* n)
    {
        return rebuild<TranslationUnitNode>(n, n->mNodes);
    }

protected:
    // Rebuilding the node creates a new node with all fields of it cloned.
    template <typename Node, typename... Args>
    Node* rebuild(const ASTNode* original, const Args&... args)
    {
        // Pass the cloned fields to the cloner as constructor args.
        return cloneNode<Node>(original, cloneField(args)...);
    }

    template <typename Node, typename... Args>
    Node* cloneNode(const ASTNode* original, Args&&... args) const
    {
        auto* cloned = mAllocator.create<Node>(original->mSourceRange, std::forward<Args>(args)...);
        // We also have to clone the flags.
        cloned->mFlags = original->mFlags;
        return cloned;
    }

    // Generic cloner.
    template <typename T>
    T cloneField(T value)
    {
        // Complain if we're not a primitive (and didn't match an overload).
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, SourceRange>,
                      "Missing clone method for field type!");
        return value;
    }

    // Trivial cloners.
    Identifier* cloneField(Identifier* value) { return value; }
    const InternedString* cloneField(const InternedString* value) { return value; }
    Type* cloneField(Type* value) { return value; }

    // Array view cloner.
    template <typename T>
    ArrayView<T> cloneField(ArrayView<T> values)
    {
        T* data = mAllocator.createArray<T>(values.size());
        for (usize i = 0; i < values.size(); ++i)
        {
            data[i] = cloneField(values[i]);
        }

        return ArrayView<T>{data, values.size()};
    }

    // Specific field cloners.
    ASTNode* cloneField(ASTNode* n) { return clone(n); }
    ExpressionNode* cloneField(ExpressionNode* n) { return cloneExpression(n); }
    StatementNode* cloneField(StatementNode* n) { return cloneStatement(n); }
    ParamNode* cloneField(ParamNode* n) { return cloneParam(n); }
    TypeSpecifierNode* cloneField(TypeSpecifierNode* n) { return cloneTypeSpecifier(n); }

    IfBranchStatementNode* cloneField(IfBranchStatementNode* n)
    {
        return static_cast<IfBranchStatementNode*>(cloneStatement(n));
    }

    SwitchSectionStatementNode* cloneField(SwitchSectionStatementNode* n)
    {
        return static_cast<SwitchSectionStatementNode*>(cloneStatement(n));
    }

    CallArgument cloneField(const CallArgument& arg)
    {
        return CallArgument{arg.mSourceRange, cloneField(arg.mValue), arg.mIsInOut};
    }

    FunctionTypeParameterSpecifier cloneField(const FunctionTypeParameterSpecifier& param)
    {
        return FunctionTypeParameterSpecifier{param.mSourceRange, cloneField(param.mTypeSpecifier), param.mIsInOut};
    }

    FieldInitializer* cloneField(FieldInitializer* field)
    {
        if (field == nullptr)
        {
            return nullptr;
        }

        return mAllocator.create<FieldInitializer>(field->mSourceRange,
                                                   field->mIdentifierRange,
                                                   field->mIdentifier,
                                                   cloneField(field->mValue));
    }

    ImportSelectedEntry* cloneField(ImportSelectedEntry* entry)
    {
        if (entry == nullptr)
        {
            return nullptr;
        }

        return mAllocator.create<ImportSelectedEntry>(entry->mName, entry->mAlias);
    }

    ArenaAllocator& mAllocator;
};

} // namespace simlang
