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
        const auto& all = document.peek();
        const auto it = all.find(id);
        if (it == all.end())
            return std::nullopt;
        return it->second;
    }

    // Stores `value` under `id`, replacing any document already there.
    bool set(const std::string& id, const T& value)
    {
        eacp::Threads::assertMainThread();
        return document.mutate([&](Map& all) { all[id] = value; });
    }

    bool remove(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        return document.mutate([&](Map& all) { all.erase(id); });
    }

    bool contains(const std::string& id)
    {
        eacp::Threads::assertMainThread();
        const auto& all = document.peek();
        return all.find(id) != all.end();
    }

    std::vector<std::string> ids()
    {
        eacp::Threads::assertMainThread();
        auto out = std::vector<std::string> {};
        for (const auto& [id, value]: document.peek())
            out.push_back(id);
        return out;
    }

    std::vector<T> values()
    {
        eacp::Threads::assertMainThread();
        auto out = std::vector<T> {};
        for (const auto& [id, value]: document.peek())
            out.push_back(value);
        return out;
    }

    // A query over every document — chain where / orderBy / limit, then get().
    Query<T> query() { return Query<T> {values()}; }

    // query().where(predicate), but filtered straight off the stored documents
    // so only the matches are copied rather than the whole collection.
    Query<T> where(std::function<bool(const T&)> predicate)
    {
        eacp::Threads::assertMainThread();
        auto matches = std::vector<T> {};
        for (const auto& [id, value]: document.peek())
            if (predicate(value))
                matches.push_back(value);
        return Query<T> {std::move(matches)};
    }

    std::size_t size()
    {
        eacp::Threads::assertMainThread();
        return document.peek().size();
    }

    bool empty()
    {
        eacp::Threads::assertMainThread();
        return document.peek().empty();
    }

private:
    Document<Map> document;
};
} // namespace emberstore
