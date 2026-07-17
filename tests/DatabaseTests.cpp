// Unit tests for the Firebase-style surface — Database, its single-value
// documents, and its keyed collections. Drives the real on-disk path.

#include <emberstore/Emberstore.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

using namespace nano;
using namespace emberstore;

namespace
{
namespace fs = std::filesystem;

struct TempDir
{
    fs::path root;

    TempDir()
    {
        static std::atomic<int> counter {0};
        root = fs::temp_directory_path()
               / ("emberstore-db-test-" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string path() const { return root.string(); }
};

// No `id` field: a collection keys documents externally, like a Firestore doc
// id, so the stored type carries no key of its own.
struct Profile
{
    std::string name;
    int level = 0;

    MIRO_REFLECT(name, level)
};
} // namespace

// --- Database.document — a single value, get/set --------------------------

auto tDocumentSetThenGet = test("Database/document set then get round-trips") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};

    auto me = db.document<Profile>("me");
    check(me.set({"ada", 3}));

    const auto p = me.get();
    check(p.name == "ada");
    check(p.level == 3);
};

auto tDocumentMissingIsDefault =
    test("Database/an unset document reads as a default value") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};

    const auto p = db.document<Profile>("missing").get();
    check(p.name.empty());
    check(p.level == 0);
};

auto tDocumentLandsUnderRoot =
    test("Database/a document is a <name>.json file under the root") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    db.document<Profile>("me").set({"ada", 1});

    check(fs::exists(dir.root / "me.json"));
};

auto tDocumentSetReplaces =
    test("Database/setting a document replaces the previous value") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto me = db.document<Profile>("me");

    me.set({"ada", 1});
    me.set({"grace", 9});

    const auto p = me.get();
    check(p.name == "grace");
    check(p.level == 9);
};

// --- Database.collection — keyed documents, Firestore-style ---------------

auto tCollectionDocSetThenGet =
    test("Collection/doc set then get returns the document") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};

    auto users = db.collection<Profile>("users");
    check(users.doc("alice").set({"Alice", 1}));

    const auto got = users.doc("alice").get();
    check(got.has_value());
    check(got->name == "Alice");
    check(got->level == 1);
};

auto tCollectionGetMissingIsEmpty =
    test("Collection/get of an unknown id is empty") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto users = db.collection<Profile>("users");
    users.doc("alice").set({"Alice", 1});

    check(!users.doc("nobody").get().has_value());
};

auto tCollectionSetOverwrites =
    test("Collection/setting an existing id overwrites it") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto users = db.collection<Profile>("users");

    users.doc("alice").set({"Alice", 1});
    users.doc("alice").set({"Alice II", 2});

    check(users.size() == 1);
    check(users.doc("alice").get()->name == "Alice II");
};

auto tCollectionExistsAndRemove =
    test("Collection/exists reports presence and remove deletes") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto users = db.collection<Profile>("users");
    users.doc("alice").set({"Alice", 1});
    users.doc("bob").set({"Bob", 2});

    check(users.doc("alice").exists());
    check(users.doc("alice").remove());
    check(!users.doc("alice").exists());
    check(users.doc("bob").exists());
    check(users.size() == 1);
};

auto tCollectionEmptyAndSize = test("Collection/empty and size report state") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto users = db.collection<Profile>("users");

    check(users.empty());
    users.doc("alice").set({"Alice", 1});
    check(!users.empty());
    check(users.size() == 1);
};

auto tCollectionIdsAndValues =
    test("Collection/ids and values list every document") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};
    auto users = db.collection<Profile>("users");
    users.doc("alice").set({"Alice", 1});
    users.doc("bob").set({"Bob", 2});

    const auto ids = users.ids();
    check(ids.size() == 2);
    check(ids[0] == "alice"); // std::map order — sorted by id
    check(ids[1] == "bob");

    const auto values = users.values();
    check(values.size() == 2);
    check(values[0].name == "Alice");
    check(values[1].name == "Bob");
};

auto tCollectionPersistsAcrossReopen =
    test("Collection/documents survive reopening the database") = []
{
    auto dir = TempDir {};
    {
        auto db = emberstore::Database {dir.path()};
        db.collection<Profile>("users").doc("bob").set({"Bob", 2});
    }
    auto reopened = emberstore::Database {dir.path()};
    const auto got = reopened.collection<Profile>("users").doc("bob").get();
    check(got.has_value());
    check(got->level == 2);
};

auto tDatabaseKeepsStoresSeparate =
    test("Database/documents and collections are independent files") = []
{
    auto dir = TempDir {};
    auto db = emberstore::Database {dir.path()};

    db.document<Profile>("me").set({"ada", 1});
    db.collection<Profile>("users").doc("bob").set({"Bob", 2});

    check(fs::exists(dir.root / "me.json"));
    check(fs::exists(dir.root / "users.json"));
    // The document is untouched by the collection write and vice versa.
    check(db.document<Profile>("me").get().name == "ada");
    check(db.collection<Profile>("users").size() == 1);
};

// --- Collection search — where / orderBy / limit --------------------------

namespace
{
emberstore::Collection<Profile> threeLevels(const TempDir& dir)
{
    auto users = emberstore::Database {dir.path()}.collection<Profile>("users");
    users.doc("a").set({"Ann", 1});
    users.doc("b").set({"Bo", 5});
    users.doc("c").set({"Cy", 9});
    return users;
}
} // namespace

auto tQueryWhereFilters = test("Query/where keeps only matching documents") = []
{
    auto dir = TempDir {};
    auto users = threeLevels(dir);

    const auto high =
        users.where([](const Profile& p) { return p.level >= 5; }).get();
    check(high.size() == 2);
};

auto tQueryOrderByLimit =
    test("Query/orderBy descending with limit sorts and caps") = []
{
    auto dir = TempDir {};
    auto users = threeLevels(dir);

    const auto top = users.query()
                         .orderBy([](const Profile& p) { return p.level; }, false)
                         .limit(2)
                         .get();
    check(top.size() == 2);
    check(top[0].level == 9);
    check(top[1].level == 5);
};

auto tQueryOrderByMemberPointer = test("Query/orderBy accepts a member pointer") = []
{
    auto dir = TempDir {};
    auto users = threeLevels(dir);

    const auto rows = users.query().orderBy(&Profile::level, false).get();
    check(rows.size() == 3);
    check(rows[0].level == 9);
    check(rows[2].level == 1);
};

auto tQueryChainedWhereOrderBy = test("Query/where then orderBy composes") = []
{
    auto dir = TempDir {};
    auto users = threeLevels(dir);

    const auto rows = users.where([](const Profile& p) { return p.level >= 5; })
                          .orderBy([](const Profile& p) { return p.level; })
                          .get();
    check(rows.size() == 2);
    check(rows[0].level == 5); // ascending
    check(rows[1].level == 9);
};

auto tQueryFirstAndCount = test("Query/first and count report the result set") = []
{
    auto dir = TempDir {};
    auto users = threeLevels(dir);

    check(users.where([](const Profile& p) { return p.level >= 5; }).count() == 2);

    const auto lowest =
        users.query().orderBy([](const Profile& p) { return p.level; }).first();
    check(lowest.has_value());
    check(lowest->level == 1);

    check(!users.where([](const Profile& p) { return p.level > 100; })
               .first()
               .has_value());
};
