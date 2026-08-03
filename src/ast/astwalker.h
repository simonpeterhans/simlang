#pragma once

#include "ast/astvisitorbase.h"

namespace simlang
{

template <typename Derived>
class ASTWalker : public ASTVisitorBase<Derived, bool>
{
public:
    bool defaultReturn() { return true; }

    bool visitImplicitCast(ImplicitCastNode* n) { return this->visit(n->mTarget); }

    bool visitCast(CastNode* n) { return this->visit(n->mTypeSpecifier) && this->visit(n->mTarget); }

    bool visitIdentifier(IdentifierNode*) { return true; }

    bool visitThis(ThisNode*) { return true; }

    bool visitIntLiteral(IntLiteralNode*) { return true; }

    bool visitFloatLiteral(FloatLiteralNode*) { return true; }

    bool visitBoolLiteral(BoolLiteralNode*) { return true; }

    bool visitStringLiteral(StringLiteralNode*) { return true; }

    bool visitNullLiteral(NullLiteralNode*) { return true; }

    bool visitFormatString(FormatStringNode* n)
    {
        for (ExpressionNode* e : n->mArgs)
        {
            if (this->visit(e) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitNewObject(NewObjectNode* n)
    {
        if (this->visit(n->mTypeSpecifier) == false)
        {
            return false;
        }

        for (ExpressionNode* arg : n->mInitializerArguments)
        {
            if (this->visit(arg) == false)
            {
                return false;
            }
        }

        for (FieldInitializer* init : n->mFieldInitializers)
        {
            if (this->visit(init->mValue) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitFunctionCall(FunctionCallNode* n)
    {
        if (this->visit(n->mReceiver) == false)
        {
            return false;
        }

        for (ExpressionNode* arg : n->mArgs)
        {
            if (this->visit(arg) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitIndexCall(IndexCallNode* n) { return this->visit(n->mReceiver) && this->visit(n->mIndex); }

    bool visitMemberAccess(MemberAccessNode* n) { return this->visit(n->mReceiver); }

    bool visitModuleAccess(ModuleAccessNode* n) { return this->visit(n->mLeft) && this->visit(n->mRight); }

    bool visitUnaryOp(UnaryOpNode* n) { return this->visit(n->mExpr); }

    bool visitBinaryOp(BinaryOpNode* n) { return this->visit(n->mLeft) && this->visit(n->mRight); }

    bool visitTernaryExpr(TernaryExprNode* n)
    {
        return this->visit(n->mCondition) && this->visit(n->mThenExpr) && this->visit(n->mElseExpr);
    }

    bool visitEmptyStatement(EmptyStatementNode*) { return true; }

    bool visitExpressionStatement(ExpressionStatementNode* n) { return this->visit(n->mExpression); }

    bool visitBlockStatement(BlockStatementNode* n)
    {
        for (StatementNode* stmt : n->mStatements)
        {
            if (this->visit(stmt) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitVariableDeclarationStatement(VariableDeclarationStatementNode* n) { return this->visit(n->mInit); }

    bool visitFunctionDeclarationStatement(FunctionDeclarationStatementNode* n)
    {
        for (auto* p : n->mParams)
        {
            if (this->visit(p) == false)
            {
                return false;
            }
        }

        return this->visit(n->mBody);
    }

    bool visitTypeDeclarationStatement(TypeDeclarationStatementNode* n)
    {
        for (auto* iface : n->mImplementedInterfaces)
        {
            if (this->visit(iface) == false)
            {
                return false;
            }
        }

        for (auto* member : n->mMembers)
        {
            if (this->visit(member) == false)
            {
                return false;
            }
        }
        return true;
    }

    bool visitAssignmentStatement(AssignmentStatementNode* n) { return this->visit(n->mLHS) && this->visit(n->mRHS); }

    bool visitReturnStatement(ReturnStatementNode* n) { return this->visit(n->mExpression); }

    bool visitWhileStatement(WhileStatementNode* n) { return this->visit(n->mCondition) && this->visit(n->mBody); }

    bool visitSwitchSectionStatement(SwitchSectionStatementNode* n)
    {
        if (this->visit(n->mCaseExpression) == false)
        {
            return false;
        }

        for (StatementNode* stmt : n->mStatements)
        {
            if (this->visit(stmt) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitSwitchStatement(SwitchStatementNode* n)
    {
        if (this->visit(n->mExpression) == false)
        {
            return false;
        }

        for (SwitchSectionStatementNode* section : n->mSections)
        {
            if (this->visit(section) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitForStatement(ForStatementNode* n)
    {
        return this->visit(n->mInit) && this->visit(n->mCondition) && this->visit(n->mIncrement) &&
               this->visit(n->mBody);
    }

    bool visitIfBranchStatement(IfBranchStatementNode* n)
    {
        return this->visit(n->mCondition) && this->visit(n->mBody);
    }

    bool visitIfStatement(IfStatementNode* n)
    {
        for (auto* br : n->mBranches)
        {
            if (this->visit(br) == false)
            {
                return false;
            }
        }

        if (this->visit(n->mElseBody) == false)
        {
            return false;
        }

        return true;
    }

    bool visitBreakStatement(BreakStatementNode*) { return true; }

    bool visitContinueStatement(ContinueStatementNode*) { return true; }

    bool visitPrintStatement(PrintStatementNode* n) { return this->visit(n->mExpression); }

    bool visitImportDeclarationStatement(ImportDeclarationStatementNode*) { return true; }

    bool visitParamDeclaration(ParamDeclarationNode*) { return true; }

    // Types.
    bool visitNamedTypeSpecifier(NamedTypeSpecifierNode* n)
    {
        for (TypeSpecifierNode* arg : n->mTypeArgs)
        {
            if (this->visit(arg) == false)
            {
                return false;
            }
        }

        return true;
    }

    bool visitSubstitutedTypeSpecifier(SubstitutedTypeSpecifierNode*) { return true; }

    bool visitTranslationUnit(TranslationUnitNode* n)
    {
        for (auto* d : n->mNodes)
        {
            if (this->visit(d) == false)
            {
                return false;
            }
        }

        return true;
    }
};

} // namespace simlang
