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
// The Format picks the extension (.json / .ember) and what writes emit.
// Opening a database under a different format than last time just works: the
// old store file is adopted (renamed) the first time a store handle is made,
// reads sniff the actual content, and each store converts on its next write.
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

    // Resolves the store file for this database's format, first adopting a
    // file the other format left behind so a format switch keeps the data.
    eacp::FilePath fileFor(std::string_view name) const
    {
        const auto other = format == Format::Binary ? Format::Json : Format::Binary;
        const auto target =
            root / (std::string {name} + std::string {extensionFor(format)});
        const auto legacy =
            root / (std::string {name} + std::string {extensionFor(other)});

        adoptLegacyFile(legacy, target);
        return target;
    }

    eacp::FilePath root;
    Durability durability;
    Format format;
};
} // namespace emberstore
