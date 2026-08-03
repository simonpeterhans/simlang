#pragma once

#include <utility>
#include <vector>

namespace simlang
{

struct Symbol;
struct Type;
struct TypeDeclarationStatementNode;

struct TemplateInstanceEntry
{
    Symbol* mTemplateSymbol = nullptr;
    TypeDeclarationStatementNode* mNode = nullptr;
    std::vector<Type*> mArgs;
};

class TemplateInstantiationCache
{
public:
    TypeDeclarationStatementNode* find(Symbol* templateSymbol, const std::vector<Type*>& args) const
    {
        for (const TemplateInstanceEntry& entry : mInstances)
        {
            if (entry.mTemplateSymbol == templateSymbol && entry.mArgs == args)
            {
                return entry.mNode;
            }
        }

        return nullptr;
    }

    void add(Symbol* templateSymbol, TypeDeclarationStatementNode* node, std::vector<Type*> args)
    {
        mInstances.push_back(TemplateInstanceEntry{templateSymbol, node, std::move(args)});
    }

private:
    std::vector<TemplateInstanceEntry> mInstances;
};

} // namespace simlang
