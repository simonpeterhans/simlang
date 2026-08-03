#include "sema/scopes.h"

#include <memory>
#include <utility>

#include "symbol/symbol.h"
#include "util/asserts.h"
#include "util/types.h"

namespace simlang
{

static constexpr usize cInitialScopeCapacity = 64;

Scope::Scope(Scope* parent)
    : mParent(parent)
{
}

bool Scope::addSymbol(Symbol* s)
{
    SIMLANG_ASSERTM(s != nullptr, "Cannot add a null symbol to a scope.");
    if (s == nullptr)
    {
        return false;
    }

    return addSymbol(s, s->mIdentifier);
}

bool Scope::addSymbol(Symbol* s, Identifier* asName)
{
    SIMLANG_ASSERTM(s != nullptr, "Cannot add a null symbol to a scope.");
    SIMLANG_ASSERTM(asName != nullptr, "Symbol alias is null.");
    if (s == nullptr || asName == nullptr)
    {
        return false;
    }

    bool inserted = mData.emplace(asName, s).second;
    SIMLANG_ASSERTM(inserted, "Symbol already exists in scope.");

    return inserted;
}

Symbol* Scope::getSymbol(Identifier* id) const
{
    if (id == nullptr)
    {
        return nullptr;
    }

    auto it = mData.find(id);
    if (it == mData.end())
    {
        return nullptr;
    }

    return it->second;
}

Symbol* Scope::getSymbolRecursive(Identifier* id) const
{
    if (id == nullptr)
    {
        return nullptr;
    }

    // Start from the current scope, then go up to the parent until we have none left.
    for (auto sc = this; sc != nullptr; sc = sc->mParent)
    {
        if (Symbol* s = sc->getSymbol(id))
        {
            return s;
        }
    }

    return nullptr;
}

ScopeManager::ScopeManager()
{
    mScopes.reserve(cInitialScopeCapacity);

    mRoot = createScope(nullptr);
    mCurrent = mRoot;
}

Scope* ScopeManager::createScope(Scope* parent)
{
    mScopes.emplace_back(std::make_unique<Scope>(parent));
    return mScopes.back().get();
}

void ScopeManager::enterScope()
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    mCurrent = createScope(mCurrent);
}

bool ScopeManager::addSymbol(Symbol* s)
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    if (mCurrent == nullptr)
    {
        return false;
    }

    return mCurrent->addSymbol(s);
}

bool ScopeManager::addSymbol(Symbol* s, Identifier* asName)
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    if (mCurrent == nullptr)
    {
        return false;
    }

    return mCurrent->addSymbol(s, asName);
}

bool ScopeManager::hasSymbolRecursive(Identifier* id)
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    if (mCurrent == nullptr)
    {
        return false;
    }

    return mCurrent->hasSymbolRecursive(id);
}

bool ScopeManager::hasSymbol(Identifier* id)
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    if (mCurrent == nullptr)
    {
        return false;
    }

    return mCurrent->hasSymbol(id);
}

Symbol* ScopeManager::getSymbol(Identifier* id)
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    if (mCurrent == nullptr)
    {
        return nullptr;
    }

    return mCurrent->getSymbol(id);
}

Symbol* ScopeManager::getSymbolRecursive(Identifier* id)
{
    SIMLANG_ASSERTM(mCurrent != nullptr, "No current scope set.");
    if (mCurrent == nullptr)
    {
        return nullptr;
    }

    return mCurrent->getSymbolRecursive(id);
}

ScopeGuard::ScopeGuard(ScopeManager& m)
    : mManager(m)
    , mPrevious(m.getCurrentScope())
{
    mManager.enterScope();
}

ScopeGuard::ScopeGuard(ScopeManager& m, Scope* s)
    : mManager(m)
    , mPrevious(m.getCurrentScope())
{
    SIMLANG_ASSERTM(s != nullptr, "Cannot bind a null scope.");
    mManager.setCurrentScope(s);
}

ScopeGuard::~ScopeGuard()
{
    mManager.setCurrentScope(mPrevious);
}

} // namespace simlang
