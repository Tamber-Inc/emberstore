#pragma once

#include "Collection.h"
#include "Database.h"
#include "Document.h"
#include "Query.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emberstore
{
namespace detail
{
class StagedFile
{
public:
    virtual ~StagedFile() = default;

    virtual bool hasStagedChanges() const = 0;
    virtual bool commitStagedChanges() = 0;
    virtual void discardStagedChanges() = 0;
};

template <typename T>
struct StagedCollectionFile final : StagedFile
{
    using Stored = typename Collection<T>::Map;

    explicit StagedCollectionFile(Document<Stored> documentToUse)
        : document(std::move(documentToUse))
    {
    }

    bool hasStagedChanges() const override { return !changes.empty(); }

    bool commitStagedChanges() override
    {
        const auto committed = document.mutate(
            [&](Stored& stored)
            {
                for (const auto& [id, change]: changes)
                {
                    if (change)
                        stored.insert_or_assign(id, *change);
                    else
                        stored.erase(id);
                }
            });

        if (committed)
            changes.clear();
        return committed;
    }

    void discardStagedChanges() override { changes.clear(); }

    Document<Stored> document;

    // A key mapped to nullopt is a staged remove.
    std::map<std::string, std::optional<T>> changes;
};

template <typename T>
struct StagedDocumentFile final : StagedFile
{
    using Stored = T;

    explicit StagedDocumentFile(Document<Stored> documentToUse)
        : document(std::move(documentToUse))
    {
    }

    bool hasStagedChanges() const override { return stagedValue.has_value(); }

    bool commitStagedChanges() override
    {
        const auto committed = document.write(*stagedValue);
        if (committed)
            stagedValue.reset();
        return committed;
    }

    void discardStagedChanges() override { stagedValue.reset(); }

    Document<Stored> document;
    std::optional<T> stagedValue;
};
} // namespace detail

// A Collection whose sets and removes are staged in its TransactionalDatabase
// until commit(). Reads see the staged changes layered over the file. A handle:
// it must not outlive the database that made it.
template <typename T>
class TransactionalCollection
{
public:
    class DocRef
    {
    public:
        DocRef(TransactionalCollection& ownerToUse, std::string idToUse)
            : owner(ownerToUse)
            , id(std::move(idToUse))
        {
        }

        std::optional<T> get() const { return owner.get(id); }
        void set(const T& value) { owner.set(id, value); }
        void remove() { owner.remove(id); }
        bool exists() const { return owner.contains(id); }
        const std::string& key() const { return id; }

    private:
        TransactionalCollection& owner;
        std::string id;
    };

    explicit TransactionalCollection(detail::StagedCollectionFile<T>& stagedToUse)
        : staged(stagedToUse)
    {
    }

    DocRef doc(std::string id) { return DocRef {*this, std::move(id)}; }

    std::optional<T> get(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        const auto change = staged.changes.find(id);
        if (change != staged.changes.end())
            return change->second;

        const auto& stored = staged.document.peek();
        const auto it = stored.find(id);
        if (it == stored.end())
            return std::nullopt;
        return it->second;
    }

    void set(const std::string& id, const T& value)
    {
        eacp::Threads::assertMainThread();
        staged.changes.insert_or_assign(id, value);
    }

    void remove(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        staged.changes.insert_or_assign(id, std::nullopt);
    }

    bool contains(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        const auto change = staged.changes.find(id);
        if (change != staged.changes.end())
            return change->second.has_value();
        return staged.document.peek().contains(id);
    }

    std::vector<std::string> ids()
    {
        eacp::Threads::assertMainThread();
        auto out = std::vector<std::string> {};
        visitEachVisibleDocument([&](const std::string& id, const T&)
                                 { out.push_back(id); });
        return out;
    }

    std::vector<T> values()
    {
        eacp::Threads::assertMainThread();
        auto out = std::vector<T> {};
        visitEachVisibleDocument([&](const std::string&, const T& value)
                                 { out.push_back(value); });
        return out;
    }

    Query<T> query() { return Query<T> {values()}; }

    Query<T> where(std::function<bool(const T&)> predicate)
    {
        eacp::Threads::assertMainThread();
        auto matches = std::vector<T> {};
        visitEachVisibleDocument(
            [&](const std::string&, const T& value)
            {
                if (predicate(value))
                    matches.push_back(value);
            });
        return Query<T> {std::move(matches)};
    }

    std::size_t size()
    {
        eacp::Threads::assertMainThread();
        auto count = std::size_t {0};
        visitEachVisibleDocument([&](const std::string&, const T&) { ++count; });
        return count;
    }

    bool empty() { return size() == 0; }

    const eacp::FilePath& filePath() const { return staged.document.filePath(); }

private:
    // Walks the stored and staged maps together in id order, so the result is
    // sorted like a plain Collection's without copying the stored map.
    template <typename Visitor>
    void visitEachVisibleDocument(Visitor&& visit)
    {
        const auto& stored = staged.document.peek();
        auto storedIt = stored.begin();
        auto changeIt = staged.changes.begin();

        while (storedIt != stored.end() || changeIt != staged.changes.end())
        {
            const auto isStoredNext =
                changeIt == staged.changes.end()
                || (storedIt != stored.end() && storedIt->first < changeIt->first);
            if (isStoredNext)
            {
                visit(storedIt->first, storedIt->second);
                ++storedIt;
                continue;
            }

            if (storedIt != stored.end() && storedIt->first == changeIt->first)
                ++storedIt;
            if (changeIt->second)
                visit(changeIt->first, *changeIt->second);
            ++changeIt;
        }
    }

    detail::StagedCollectionFile<T>& staged;
};

// A Document whose set() is staged in its TransactionalDatabase until commit().
// mutateImmediately() bypasses staging: it takes the inter-process lock, edits
// the file and writes it at once. A handle: it must not outlive its database.
template <typename T>
class TransactionalDocument
{
public:
    explicit TransactionalDocument(detail::StagedDocumentFile<T>& stagedToUse)
        : staged(stagedToUse)
    {
    }

    T read() { return peek(); }
    T get() { return read(); }

    const T& peek()
    {
        eacp::Threads::assertMainThread();
        if (staged.stagedValue)
            return *staged.stagedValue;
        return staged.document.peek();
    }

    void set(const T& value)
    {
        eacp::Threads::assertMainThread();
        staged.stagedValue = value;
    }

    template <typename Fn>
    bool mutateImmediately(Fn&& fn)
    {
        eacp::Threads::assertMainThread();
        return staged.document.mutate(std::forward<Fn>(fn));
    }

    const eacp::FilePath& filePath() const { return staged.document.filePath(); }

private:
    detail::StagedDocumentFile<T>& staged;
};

// A Database whose writes wait in memory until commit(). Each commit takes every
// changed file's inter-process lock, re-reads it, applies the staged sets and
// removes, and writes it once, so another process's changes to other keys
// survive. Changes still staged when it is destroyed are discarded.
//
//   auto db = TransactionalDatabase {configDir};
//   auto users = db.collection<Profile>("users");
//   users.doc("alice").set({"Alice", 1});
//   users.doc("bob").remove();
//   db.commit();
//
// Message-thread only, like Database.
class TransactionalDatabase
{
public:
    explicit TransactionalDatabase(eacp::FilePath rootToUse,
                                   Durability durabilityToUse = Durability::Durable)
        : database(std::move(rootToUse), durabilityToUse)
    {
    }

    template <typename T>
    TransactionalDocument<T> document(std::string_view name)
    {
        eacp::Threads::assertMainThread();
        return TransactionalDocument<T> {
            findOrCreateStagedFile<detail::StagedDocumentFile<T>>(name)};
    }

    template <typename T>
    TransactionalCollection<T> collection(std::string_view name)
    {
        eacp::Threads::assertMainThread();
        return TransactionalCollection<T> {
            findOrCreateStagedFile<detail::StagedCollectionFile<T>>(name)};
    }

    // Writes each changed file once. Returns false if any file could not be
    // written; that file keeps its staged changes, the others are committed.
    bool commit()
    {
        eacp::Threads::assertMainThread();
        auto isEveryFileCommitted = true;
        for (auto& entry: stagedFiles)
        {
            auto& staged = *entry.second;
            if (staged.hasStagedChanges() && !staged.commitStagedChanges())
                isEveryFileCommitted = false;
        }
        return isEveryFileCommitted;
    }

    void discardChanges()
    {
        eacp::Threads::assertMainThread();
        for (auto& entry: stagedFiles)
            entry.second->discardStagedChanges();
    }

    bool hasUncommittedChanges() const
    {
        eacp::Threads::assertMainThread();
        return std::any_of(stagedFiles.begin(),
                           stagedFiles.end(),
                           [](const auto& entry)
                           { return entry.second->hasStagedChanges(); });
    }

    const eacp::FilePath& directory() const { return database.directory(); }

private:
    template <typename Staged>
    Staged& findOrCreateStagedFile(std::string_view name)
    {
        auto& slot = stagedFiles[std::string {name}];
        if (!slot)
        {
            slot = std::make_unique<Staged>(
                database.document<typename Staged::Stored>(name));
        }

        if (auto* const typed = dynamic_cast<Staged*>(slot.get()))
            return *typed;
        throw std::logic_error {"emberstore: \"" + std::string {name}
                                + "\" is already open as a different type"};
    }

    Database database;
    std::map<std::string, std::unique_ptr<detail::StagedFile>> stagedFiles;
};
} // namespace emberstore
