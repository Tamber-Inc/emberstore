#pragma once

#include "Document.h"
#include "Query.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace emberstore
{
// A collection of Miro-reflected documents keyed by a string id, in the shape
// of a Firestore collection: the id lives outside the document, so the stored
// type carries no key of its own. Persisted as one JSON object under the id
// keys, written atomically (see Document); message-thread only.
//
//   auto users = db.collection<Profile>("users");
//   users.doc("alice").set({"Alice", 1});
//   auto alice = users.doc("alice").get();   // std::optional<Profile>
//   users.doc("alice").remove();
//
// Suited to modest collections — each call reads or writes the whole file.
template <typename T>
class Collection
{
public:
    using Map = std::map<std::string, T>;

    // A handle to one id — present or not — mirroring Firestore's doc ref.
    class DocRef
    {
    public:
        DocRef(Collection& ownerToUse, std::string idToUse)
            : owner(ownerToUse)
            , id(std::move(idToUse))
        {
        }

        std::optional<T> get() const { return owner.get(id); }
        bool set(const T& value) { return owner.set(id, value); }
        bool remove() { return owner.remove(id); }
        bool exists() const { return owner.contains(id); }
        const std::string& key() const { return id; }

    private:
        Collection& owner;
        std::string id;
    };

    explicit Collection(eacp::FilePath path,
                        Durability durability = Durability::Durable)
        : document(std::move(path), durability)
    {
    }

    DocRef doc(std::string id) { return DocRef {*this, std::move(id)}; }

    std::optional<T> get(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        const auto change = stagedChanges.find(id);
        if (change != stagedChanges.end())
            return change->second;

        const auto& all = document.peek();
        const auto it = all.find(id);
        if (it == all.end())
            return std::nullopt;
        return it->second;
    }

    // Stores `value` under `id`, replacing any document already there. In a
    // TransactionalDatabase it is staged instead and this returns true.
    bool set(const std::string& id, const T& value)
    {
        eacp::Threads::assertMainThread();
        if (writeMode == detail::WriteMode::StageUntilCommit)
        {
            stagedChanges.insert_or_assign(id, value);
            return true;
        }
        return document.mutate([&](Map& all) { all[id] = value; });
    }

    bool remove(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        if (writeMode == detail::WriteMode::StageUntilCommit)
        {
            stagedChanges.insert_or_assign(id, std::nullopt);
            return true;
        }
        return document.mutate([&](Map& all) { all.erase(id); });
    }

    bool contains(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        const auto change = stagedChanges.find(id);
        if (change != stagedChanges.end())
            return change->second.has_value();
        const auto& all = document.peek();
        return all.find(id) != all.end();
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

    // A query over every document — chain where / orderBy / limit, then get().
    Query<T> query() { return Query<T> {values()}; }

    // query().where(predicate), but filtered straight off the stored documents
    // so only the matches are copied rather than the whole collection.
    Query<T> where(const std::function<bool(const T&)>& predicate)
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
        if (stagedChanges.empty())
            return document.peek().size();

        auto count = std::size_t {0};
        visitEachVisibleDocument([&](const std::string&, const T&) { ++count; });
        return count;
    }

    bool empty()
    {
        eacp::Threads::assertMainThread();
        if (stagedChanges.empty())
            return document.peek().empty();
        return size() == 0;
    }

private:
    template <typename Store>
    friend class detail::TransactionalStore;

    // A key mapped to nullopt is a staged remove.
    using StagedChanges = std::map<std::string, std::optional<T>>;
    using StagedChange = typename StagedChanges::value_type;
    using StagedChangeIterator = typename StagedChanges::const_iterator;

    bool commitStagedChanges()
    {
        if (stagedChanges.empty())
            return true;

        const auto committed = document.mutate(
            [this](Map& all)
            {
                for (const auto& [id, change]: stagedChanges)
                {
                    if (change)
                        all.insert_or_assign(id, *change);
                    else
                        all.erase(id);
                }
            });
        if (committed)
            stagedChanges.clear();
        return committed;
    }

    void discardStagedChanges() { stagedChanges.clear(); }
    bool hasStagedChanges() const { return !stagedChanges.empty(); }
    void stageWritesUntilCommit()
    {
        writeMode = detail::WriteMode::StageUntilCommit;
    }

    // Walks the stored and staged maps together in id order, so the result is
    // sorted by id without copying the stored map.
    template <typename Visitor>
    void visitEachVisibleDocument(Visitor&& visit)
    {
        auto change = stagedChanges.cbegin();
        for (const auto& [id, value]: document.peek())
        {
            change = visitStagedChangesBefore(id, change, visit);
            if (change != stagedChanges.cend() && change->first == id)
                visitStagedChange(*change++, visit);
            else
                visit(id, value);
        }

        for (; change != stagedChanges.cend(); ++change)
            visitStagedChange(*change, visit);
    }

    template <typename Visitor>
    StagedChangeIterator visitStagedChangesBefore(const std::string& id,
                                                  StagedChangeIterator change,
                                                  Visitor& visit) const
    {
        for (; change != stagedChanges.cend() && change->first < id; ++change)
            visitStagedChange(*change, visit);
        return change;
    }

    template <typename Visitor>
    static void visitStagedChange(const StagedChange& change, Visitor& visit)
    {
        if (change.second)
            visit(change.first, *change.second);
    }

    Document<Map> document;
    StagedChanges stagedChanges;
    detail::WriteMode writeMode = detail::WriteMode::CommitEachChange;
};
} // namespace emberstore
