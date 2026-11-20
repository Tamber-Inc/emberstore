#pragma once

#include "Collection.h"
#include "Document.h"

#include <string>
#include <string_view>
#include <utility>

namespace emberstore
{
// An embedded document database rooted at a directory: point it at a folder and
// ask for typed stores by name, each its own file under the root. Like opening
// a SQLite file, but for documents.
//
//   auto db = Database {configDir};                     // JSON files
//   auto fast = Database {configDir, Format::Binary};   // packed binary files
//   auto settings = db.document<Settings>("settings");  // <root>/settings.json
//   auto users    = db.collection<Profile>("users");    // <root>/users.json
//   users.doc("alice").set({"Alice", 1});
//
// The Format picks the extension (.json / .ember) and what writes emit. It is
// fixed per database: a store opened under the other format is a different
// file.
//
// A thin handle factory — the returned stores own the IO and, like them, it is
// message-thread only. Safe for a single app / process for now; an inter-process
// lock will come later.
class Database
{
public:
    explicit Database(eacp::FilePath rootToUse,
                      Durability durabilityToUse = Durability::Durable,
                      Format formatToUse = Format::Json)
        : root(std::move(rootToUse))
        , durability(durabilityToUse)
        , format(formatToUse)
    {
    }

    Database(eacp::FilePath rootToUse,
             Format formatToUse,
             Durability durabilityToUse = Durability::Durable)
        : Database(std::move(rootToUse), durabilityToUse, formatToUse)
    {
    }

    template <typename T>
    Document<T> document(std::string_view name) const
    {
        return Document<T> {fileFor(name), durability, format};
    }

    template <typename T>
    Collection<T> collection(std::string_view name) const
    {
        return Collection<T> {fileFor(name), durability, format};
    }

    const eacp::FilePath& directory() const { return root; }

private:
    static constexpr std::string_view extensionFor(Format formatToUse)
    {
        return formatToUse == Format::Binary ? ".ember" : ".json";
    }

    // The store file for this database's format. Pure path math, no IO.
    eacp::FilePath fileFor(std::string_view name) const
    {
        return root / (std::string {name} + std::string {extensionFor(format)});
    }

    eacp::FilePath root;
    Durability durability;
    Format format;
};
} // namespace emberstore
