#pragma once

#include "Collection.h"
#include "Database.h"
#include "Document.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace emberstore
{
namespace detail
{
class StagedStore
{
public:
    virtual ~StagedStore() = default;

    virtual bool hasStagedChanges() const = 0;
    virtual bool commitStagedChanges() = 0;
    virtual void discardStagedChanges() = 0;
};

template <typename Store>
class TransactionalStore final : public StagedStore
{
public:
    explicit TransactionalStore(Store storeToUse)
        : store(std::move(storeToUse))
    {
        store.stageWritesUntilCommit();
    }

    bool hasStagedChanges() const override { return store.hasStagedChanges(); }
    bool commitStagedChanges() override { return store.commitStagedChanges(); }
    void discardStagedChanges() override { store.discardStagedChanges(); }

    Store store;
};
} // namespace detail

// A Database whose writes wait in memory until commit(). The stores it hands
// out are the same Document and Collection a Database makes, except that their
// writes are staged; reads through them see the staged changes over the file.
// Each commit takes every changed file's inter-process lock, re-reads it,
// replays the staged changes and writes it once, so another process's changes
// to other keys survive. Changes still staged when it is destroyed are dropped.
//
//   auto db = TransactionalDatabase {configDir};
//   auto& users = db.collection<Profile>("users");
//   users.doc("alice").set({"Alice", 1});
//   users.doc("bob").remove();
//   db.commit();
//
// The stores are owned by the database: hold them by reference. Message-thread
// only, like Database.
class TransactionalDatabase
{
public:
    explicit TransactionalDatabase(eacp::FilePath rootToUse,
                                   Durability durabilityToUse = Durability::Durable)
        : database(std::move(rootToUse), durabilityToUse)
    {
    }

    template <typename T>
    Document<T>& document(std::string_view name)
    {
        eacp::Threads::assertMainThread();
        return findOrCreateStore(name, [&] { return database.document<T>(name); });
    }

    template <typename T>
    Collection<T>& collection(std::string_view name)
    {
        eacp::Threads::assertMainThread();
        return findOrCreateStore(name, [&] { return database.collection<T>(name); });
    }

    // Writes each changed file once. Returns false if any file could not be
    // written; that file keeps its staged changes, the others are committed.
    bool commit()
    {
        eacp::Threads::assertMainThread();
        auto isEveryFileCommitted = true;
        for (auto& entry: stores)
        {
            if (!entry.second->commitStagedChanges())
                isEveryFileCommitted = false;
        }
        return isEveryFileCommitted;
    }

    void discardChanges()
    {
        eacp::Threads::assertMainThread();
        for (auto& entry: stores)
            entry.second->discardStagedChanges();
    }

    bool hasUncommittedChanges() const
    {
        eacp::Threads::assertMainThread();
        return std::any_of(stores.begin(),
                           stores.end(),
                           [](const auto& entry)
                           { return entry.second->hasStagedChanges(); });
    }

    const eacp::FilePath& directory() const { return database.directory(); }

private:
    template <typename MakeStore>
    auto& findOrCreateStore(std::string_view name, MakeStore&& makeStore)
    {
        using Store = std::invoke_result_t<MakeStore&>;
        using Entry = detail::TransactionalStore<Store>;

        auto& slot = stores[std::string {name}];
        if (!slot)
            slot = std::make_unique<Entry>(makeStore());

        if (auto* const entry = dynamic_cast<Entry*>(slot.get()))
            return entry->store;
        throw std::logic_error {"emberstore: \"" + std::string {name}
                                + "\" is already open as a different type"};
    }

    Database database;
    std::map<std::string, std::unique_ptr<detail::StagedStore>> stores;
};
} // namespace emberstore
