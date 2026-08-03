#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

namespace simlang
{

struct Identifier;
struct Symbol;

class Scope
{
public:
    explicit Scope(Scope* parent);

    bool addSymbol(Symbol* s);
    bool addSymbol(Symbol* s, Identifier* asName);

    Symbol* getSymbol(Identifier* id) const;
    Symbol* getSymbolRecursive(Identifier* id) const;

    bool hasSymbol(Identifier* id) const { return getSymbol(id) != nullptr; }
    bool hasSymbolRecursive(Identifier* id) const { return getSymbolRecursive(id) != nullptr; }

private:
    Scope* mParent = nullptr;
    std::unordered_map<Identifier*, Symbol*> mData;
};

class ScopeManager
{
public:
    ScopeManager();

    Scope* createScope(Scope* parent);

    void enterScope();

    bool addSymbol(Symbol* s);
    bool addSymbol(Symbol* s, Identifier* asName);

    bool hasSymbolRecursive(Identifier* id);
    bool hasSymbol(Identifier* id);

    Symbol* getSymbol(Identifier* id);
    Symbol* getSymbolRecursive(Identifier* id);

    Scope* getRootScope() const { return mRoot; }
    Scope* getCurrentScope() const { return mCurrent; }
    void setCurrentScope(Scope* scope) { mCurrent = scope; }
    const std::vector<std::unique_ptr<Scope>>& scopes() const { return mScopes; }

private:
    std::vector<std::unique_ptr<Scope>> mScopes;
    Scope* mRoot = nullptr;
    Scope* mCurrent = nullptr;
};

class ScopeGuard
{
public:
    explicit ScopeGuard(ScopeManager& m);
    explicit ScopeGuard(ScopeManager& m, Scope* s);
    ~ScopeGuard();

private:
    ScopeManager& mManager;
    Scope* mPrevious = nullptr;
};

} // namespace simlang
