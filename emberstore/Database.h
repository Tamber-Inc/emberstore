#pragma once

#include "Collection.h"
#include "Document.h"

#include <string>
#include <string_view>
#include <utility>

namespace emberstore
{
// An embedded document database rooted at a directory: point it at a folder and
// ask for typed stores by name, each its own JSON file under the root. Like
// opening a SQLite file, but for documents.
//
//   auto db = Database {configDir};
//   auto settings = db.document<Settings>("settings");  // <root>/settings.json
//   auto users    = db.collection<Profile>("users");    // <root>/users.json
//   users.doc("alice").set({"Alice", 1});
//
// A thin handle factory — the returned stores own the IO and, like them, it is
// message-thread only. Safe for a single app / process for now; an inter-process
// lock will come later.
class Database
{
public:
    explicit Database(eacp::FilePath rootToUse,
                      Durability durabilityToUse = Durability::Durable)
        : root(std::move(rootToUse))
        , durability(durabilityToUse)
    {
    }

    template <typename T>
    Document<T> document(std::string_view name) const
    {
        return Document<T> {fileFor(name), durability};
    }

    template <typename T>
    Collection<T> collection(std::string_view name) const
    {
        return Collection<T> {fileFor(name), durability};
    }

    const eacp::FilePath& directory() const { return root; }

private:
    eacp::FilePath fileFor(std::string_view name) const
    {
        return root / (std::string {name} + ".json");
    }

    eacp::FilePath root;
    Durability durability;
};
} // namespace emberstore
