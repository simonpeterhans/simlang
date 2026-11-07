#include <unordered_set>
#include <vector>

#include "ast/assignmentop.h"
#include "ast/nodes/astnode.h"
#include "ast/nodes/nodetypes.h"
#include "ast/nodes/stmtnodes.h"
#include "ast/nodes/typespecifiernodes.h"
#include "diag/diagnosticmanager.h"
#include "diag/diagnostictype.h"
#include "driver/compilercontext.h"
#include "sema/passes/typecheckvisitor.h"
#include "source/sourcerange.h"
#include "symbol/symbol.h"
#include "symbol/symboltype.h"
#include "symbol/symbolutils.h"
#include "type/typeformat.h"
#include "type/typekind.h"
#include "type/types.h"
#include "util/arenautils.h"
#include "util/arrayview.h"
#include "util/flags.h"
#include "util/scoping.h"
#include "util/types.h"

namespace simlang
{

struct Identifier;

bool TypeCheckVisitor::allowClassToInterfaceCast(Type* aggregateType, InterfaceType* interfaceType)
{
    if (aggregateType == nullptr || interfaceType == nullptr || aggregateType->mKind != TypeKind::cClass)
    {
        return false;
    }

    // Here, we might still have to resolve the interface types first.
    auto* classType = static_cast<AggregateType*>(aggregateType);
    auto* classDecl = static_cast<TypeDeclarationStatementNode*>(classType->mSymbol->mDeclNode);
    // This is only needed if the type has no interfaces, but the declaration has.
    if (classType->mInterfaces.empty() && classDecl->mImplementedInterfaces.empty() == false)
    {
        if (resolveImplementedInterfaces(classDecl) == false)
        {
            return false;
        }
    }

    // Find out whether the class implements the interface.
    for (const InterfaceImplementation& implementation : classType->mInterfaces)
    {
        if (implementation.mInterface == interfaceType)
        {
            return true;
        }
    }

    return false;
}

bool TypeCheckVisitor::resolveImplementedInterfaces(TypeDeclarationStatementNode* node)
{
    // Interfaces cannot inherit from other interfaces, so they are already resolved.
    if (node->isInterface())
    {
        return true;
    }

    auto* aggType = static_cast<AggregateType*>(node->mSymbol->mType);

    std::vector<InterfaceImplementation> interfaces;
    interfaces.reserve(node->mImplementedInterfaces.size());

    for (TypeSpecifierNode* implInterface : node->mImplementedInterfaces)
    {
        // Resolve the type.
        if (visit(implInterface) == false)
        {
            return false;
        }

        // If we resolved to an error, ignore it.
        Type* implInterfaceType = implInterface->mType;
        if (isMissingOrErrorType(implInterfaceType))
        {
            continue;
        }

        // If we resolved something that isn't an interface, complain.
        if (implInterfaceType->mKind != TypeKind::cInterface)
        {
            mCtx.report<cNotAnInterfaceType>(implInterface->mSourceRange, typeToString(implInterfaceType));
            continue;
        }

        // Structs currently cannot implement interfaces, so check that.
        auto* interfaceType = static_cast<InterfaceType*>(implInterfaceType);
        if (node->isClass() == false)
        {
            mCtx.report<cStructInterfaceImplementation>(implInterface->mSourceRange,
                                                        typeToString(aggType),
                                                        typeToString(interfaceType));
            continue;
        }

        // Implementing an interface twice is skipped.
        bool duplicate = false;
        for (const InterfaceImplementation& existing : interfaces)
        {
            if (existing.mInterface == interfaceType)
            {
                duplicate = true;
                break;
            }
        }

        if (duplicate)
        {
            continue;
        }

        // Otherwise, consider it implemented and store the type.
        InterfaceImplementation implementation;
        implementation.mInterface = interfaceType;
        interfaces.push_back(implementation);
    }

    // Set the resolved interface types.
    aggType->mInterfaces = makeArrayView(mCtx.mAllocator, interfaces);
    return true;
}

bool TypeCheckVisitor::checkInterface(TypeDeclarationStatementNode* node)
{
    auto* aggregateType = static_cast<AggregateType*>(node->mSymbol->mType);
    std::unordered_set<Identifier*> declaredInterfaceMethods;

    // Collect all methods declared in all interfaces that we want to implement.
    for (const InterfaceImplementation& implementation : aggregateType->mInterfaces)
    {
        for (Symbol* interfaceMember : implementation.mInterface->mSymbol->mMembers)
        {
            declaredInterfaceMethods.insert(interfaceMember->mIdentifier);
        }
    }

    // Cross-check member functions and interface methods.
    // Go through member methods to check whether any of them incorrectly try to implement an interface method.
    for (Symbol* member : node->mSymbol->mMembers)
    {
        // Ignore member variables and methods that don't want to implement an interface.
        if (member->mSymbolType != SymbolType::cMemberFunction || member->mFlags.test(SymbolFlags::cImpl) == false)
        {
            continue;
        }

        // If it's an interface method and private, this is an error.
        if (member->mFlags.test(SymbolFlags::cPrivate))
        {
            mCtx.report<cPrivateImplMethod>(member->mDeclNode->mSourceRange, member->mIdentifier);
            continue;
        }

        // If it's an interface method and not declared in any of our interfaces, this is also an error.
        if (declaredInterfaceMethods.find(member->mIdentifier) == declaredInterfaceMethods.end())
        {
            mCtx.report<cInvalidImplMethod>(member->mDeclNode->mSourceRange, member->mIdentifier);
        }
    }

    // Go through all interface methods we're implementing.
    for (const InterfaceImplementation& implementation : aggregateType->mInterfaces)
    {
        InterfaceType* interfaceType = implementation.mInterface;

        for (Symbol* interfaceMember : interfaceType->mSymbol->mMembers)
        {
            Symbol* matchingMember = nullptr;

            // Find the matching member function.
            for (Symbol* member : node->mSymbol->mMembers)
            {
                if (member == aggregateType->mConstructorSymbol)
                {
                    continue;
                }

                if (member->mSymbolType == SymbolType::cMemberFunction &&
                    member->mIdentifier == interfaceMember->mIdentifier)
                {
                    matchingMember = member;
                    break;
                }
            }

            // If we have none at all, the method is missing.
            if (matchingMember == nullptr)
            {
                auto diag = mCtx.report<cInterfaceMethodMissing>(node->mIdentifierRange,
                                                                 typeToString(node->mSymbol->mType),
                                                                 typeToString(interfaceType),
                                                                 interfaceMember->mIdentifier);
                SourceRange interfaceMethodRange = getSymbolSourceRange(interfaceMember);
                if (interfaceMethodRange.isValid())
                {
                    diag.note<cInterfaceMethodDeclaredHere>(interfaceMethodRange, interfaceMember->mIdentifier);
                }

                continue;
            }

            // If the method is private, this is an error.
            if (matchingMember->mFlags.test(SymbolFlags::cPrivate))
            {
                // The private/impl case was already reported in the first loop.
                // If we have private/no-impl but matched with an interface symbol, report that.
                if (matchingMember->mFlags.test(SymbolFlags::cImpl) == false)
                {
                    mCtx.report<cPrivateInterfaceImplementation>(matchingMember->mDeclNode->mSourceRange,
                                                                 matchingMember->mIdentifier,
                                                                 typeToString(interfaceType));
                }

                continue;
            }

            // If either of the resolutions fail(ed), bail.
            if (resolveType(interfaceMember) == false || resolveType(matchingMember) == false)
            {
                return false;
            }

            // If the types don't match, also bail.
            if (interfaceMember->mType != matchingMember->mType)
            {
                auto diag = mCtx.report<cInterfaceMethodTypeMismatch>(matchingMember->mDeclNode->mSourceRange,
                                                                      interfaceMember->mIdentifier,
                                                                      typeToString(interfaceType),
                                                                      typeToString(interfaceMember->mType),
                                                                      typeToString(matchingMember->mType));
                SourceRange interfaceMethodRange = getSymbolSourceRange(interfaceMember);
                if (interfaceMethodRange.isValid())
                {
                    diag.note<cInterfaceMethodDeclaredHere>(interfaceMethodRange, interfaceMember->mIdentifier);
                }

                continue;
            }

            // Also check for missing impl here.
            if (matchingMember->mFlags.test(SymbolFlags::cImpl) == false)
            {
                auto diag = mCtx.report<cInterfaceMethodMissingImpl>(matchingMember->mDeclNode->mSourceRange,
                                                                     matchingMember->mIdentifier,
                                                                     typeToString(interfaceType));
                SourceRange interfaceMethodRange = getSymbolSourceRange(interfaceMember);
                if (interfaceMethodRange.isValid())
                {
                    diag.note<cInterfaceMethodDeclaredHere>(interfaceMethodRange, interfaceMember->mIdentifier);
                }
                diag.hint<cAddImplSpecifierHint>(matchingMember->mDeclNode->mSourceRange);
            }
        }
    }

    return true;
}

bool TypeCheckVisitor::visitTypeDeclarationStatement(TypeDeclarationStatementNode* node)
{
    // If the type declaration is a template, do nothing.
    // Derived types will be processed separately (flagged cStmtIsTemplateInstance).
    if (node->isTemplate())
    {
        return true;
    }

    // Also import the declaring module here.
    // This is relevant if we're instantiating a template, which means we have access to the private vars.
    ScopedValueBinder moduleScope{mCurrentModule, node->mDeclModule};
    ScopedValueBinder cs{mCurrentTypeSymbol, node->mSymbol};

    // Resolve all interfaces we're implementing.
    if (resolveImplementedInterfaces(node) == false)
    {
        return false;
    }

    // Visit the member declarations.
    for (StatementNode* member : node->mMembers)
    {
        if (visit(member) == false)
        {
            return false;
        }
    }

    // If this is an interface, we're done already.
    if (node->isInterface())
    {
        return true;
    }

    // Collect all the fields.
    std::vector<Symbol*> fields;
    for (Symbol* member : node->mSymbol->mMembers)
    {
        if (member->mSymbolType == SymbolType::cMemberVariable)
        {
            fields.push_back(member);
        }
    }

    // Struct values must have at least one stored field.
    bool typeOk = true;
    if (node->isStruct() && fields.empty())
    {
        mCtx.report<cEmptyStruct>(node->mIdentifierRange, node->mIdentifier);
        typeOk = false;
    }

    // Next, we go through all statements and make sure that we initialize all fields before we do something else.
    auto* aggregateType = static_cast<AggregateType*>(node->mSymbol->mType);
    auto* constructor = static_cast<FunctionDeclarationStatementNode*>(aggregateType->mConstructorSymbol->mDeclNode);

    // Nothing is initialized to begin with.
    std::vector initialized(fields.size(), false);

    auto* body = static_cast<BlockStatementNode*>(constructor->mBody);
    for (StatementNode* statement : body->mStatements)
    {
        // If this is not an assignment, complain (for now).
        if (statement->mNodeType != NodeType::cAssignmentStatement)
        {
            break;
        }

        // If this is not a '=' assignment, complain as well.
        auto* assignment = static_cast<AssignmentStatementNode*>(statement);
        if (assignment->mOp != AssignmentOp::cAss)
        {
            break;
        }

        // Check which field we're assigning to.
        Symbol* field = getConstructorAssignedField(assignment->mLHS);
        if (field == nullptr)
        {
            break;
        }

        // Mark it as initialized.
        for (usize i = 0; i < fields.size(); ++i)
        {
            if (fields[i] == field)
            {
                initialized[i] = true;
                break;
            }
        }
    }

    // Check if we initialized all fields properly.
    for (usize i = 0; i < fields.size(); ++i)
    {
        if (initialized[i] == false)
        {
            Symbol* field = fields[i];
            mCtx.report<cMissingFieldInitializer>(constructor->mIdentifierRange, field->mIdentifier)
                .note<cFieldDeclaredHere>(getSymbolSourceRange(field), field->mIdentifier)
                .hint<cFieldInitializationHint>(constructor->mIdentifierRange);
            typeOk = false;
        }
    }

    // If something failed, bail.
    if (typeOk == false)
    {
        return true;
    }

    // If we got here, the type is valid and we can check whether we correctly implement all interfaces.
    return checkInterface(node);
}

} // namespace simlang
