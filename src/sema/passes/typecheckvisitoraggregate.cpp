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
        if (isErrorType(implInterfaceType))
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
    // Interfaces cannot inherit from other interfaces, so we're already done.
    if (node->isInterface())
    {
        return true;
    }

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
                if (member == aggregateType->mInitMethodSymbol)
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

    // Struct values must have at least one stored field.
    bool typeOk = true;
    if (node->isStruct())
    {
        bool hasStoredField = false;
        for (StatementNode* member : node->mMembers)
        {
            if (member->mNodeType == NodeType::cVariableDeclarationStatement)
            {
                hasStoredField = true;
                break;
            }
        }

        if (hasStoredField == false)
        {
            mCtx.report<cEmptyStruct>(node->mIdentifierRange, node->mIdentifier);
            typeOk = false;
        }
    }

    // Check initializer rules.
    bool initializersOk = true;
    if (node->isClass() == false)
    {
        // Structs cannot declare class initializers.
        for (StatementNode* member : node->mMembers)
        {
            if (member->mNodeType != NodeType::cFunctionDeclarationStatement)
            {
                continue;
            }

            auto* fun = static_cast<FunctionDeclarationStatementNode*>(member);
            if (fun->mIsInitMethod)
            {
                mCtx.report<cInitOnNonClass>(fun->mIdentifierRange);
                initializersOk = false;
            }
        }
    }
    else
    {
        // Classes must declare an initializer.
        auto* classType = static_cast<AggregateType*>(node->mSymbol->mType);
        Symbol* initializerSymbol = classType->mInitMethodSymbol;
        if (initializerSymbol == nullptr)
        {
            mCtx.report<cMissingClassInitializer>(node->mIdentifierRange, typeToString(classType));
            initializersOk = false;
        }
        else
        {
            auto* init = static_cast<FunctionDeclarationStatementNode*>(initializerSymbol->mDeclNode);

            // Collect all fields.
            std::vector<Symbol*> fields;
            fields.reserve(classType->mSymbol->mMembers.size());
            for (Symbol* fieldMember : classType->mSymbol->mMembers)
            {
                if (fieldMember->mSymbolType == SymbolType::cMemberVariable)
                {
                    fields.push_back(fieldMember);
                }
            }

            // Track whether fields are initialized by declaration defaults.
            std::vector<bool> initialized(fields.size(), false);
            for (usize i = 0; i < fields.size(); ++i)
            {
                auto* decl = static_cast<VariableDeclarationStatementNode*>(fields[i]->mDeclNode);
                // That is the case if we have an initializer expression.
                initialized[i] = decl->mInit != nullptr;
            }

            // Now go through the init statements and fill in the explicitly assigned fields.
            // We break on the first non-assignment statement and assess whether the initializer is complete.
            std::vector<bool> explicitlyAssigned(fields.size(), false);
            std::vector<Symbol*> assignedFields;
            assignedFields.reserve(fields.size());

            auto* body = static_cast<BlockStatementNode*>(init->mBody);
            for (StatementNode* stmt : body->mStatements)
            {
                // If the statement is not an assignment, we're done.
                if (stmt->mNodeType != NodeType::cAssignmentStatement)
                {
                    break;
                }

                auto* assignment = static_cast<AssignmentStatementNode*>(stmt);
                // If the assignment is not a pure assignment, we're done as well.
                if (assignment->mOp != AssignmentOp::cAss)
                {
                    break;
                }

                // We're checking whether the assignment is to a member variable.
                Symbol* assignedField = getInitializerAssignedField(assignment->mLHS);

                // If we're not assigning to a member variable, we're done.
                if (assignedField == nullptr)
                {
                    break;
                }

                // Find out to which field we're assigning.
                usize fieldIndex = fields.size();
                for (usize i = 0; i < fields.size(); ++i)
                {
                    if (fields[i] == assignedField)
                    {
                        fieldIndex = i;
                        break;
                    }
                }

                if (fieldIndex == fields.size())
                {
                    break;
                }

                // If we already assigned to that field, we can skip the rest.
                if (explicitlyAssigned[fieldIndex])
                {
                    continue;
                }

                // Otherwise, we register the field as initialized and explicitly assigned.
                initialized[fieldIndex] = true;
                explicitlyAssigned[fieldIndex] = true;
                assignedFields.push_back(assignedField);
            }

            // Track the fields that were initialized in the init function in the node.
            init->mInitializerAssignedFields = makeArrayView(mCtx.mAllocator, assignedFields);

            // If a field is not initialized by a declaration default or init assignment, this is an error.
            for (usize i = 0; i < fields.size(); ++i)
            {
                if (initialized[i])
                {
                    continue;
                }

                Symbol* field = fields[i];

                auto diag = mCtx.report<cMissingFieldInitializer>(init->mIdentifierRange, field->mIdentifier);
                SourceRange fieldRange = getSymbolSourceRange(field);
                if (fieldRange.isValid())
                {
                    diag.note<cFieldDeclaredHere>(fieldRange, field->mIdentifier);
                }
                diag.hint<cClassFieldInitializationHint>(init->mIdentifierRange);

                initializersOk = false;
            }
        }
    }

    if (typeOk == false || initializersOk == false)
    {
        return true;
    }

    // If we got here, the type is valid and we can check whether we correctly implement all interfaces.
    return checkInterface(node);
}

} // namespace simlang
