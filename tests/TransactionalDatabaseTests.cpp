#include "ContestFile.h"
#include <emberstore/Emberstore.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

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
               / ("emberstore-transactional-test-"
                  + std::to_string(counter.fetch_add(1)));
        fs::remove_all(root);
    }

    ~TempDir() { fs::remove_all(root); }

    std::string path() const { return root.string(); }
};

struct Profile
{
    std::string name;
    int level = 0;

    MIRO_REFLECT(name, level)
};

struct IdCounter
{
    int next = 0;

    MIRO_REFLECT(next)
};

template <typename DatabaseType>
concept CanCommit = requires(DatabaseType& database) { database.commit(); };

static_assert(CanCommit<TransactionalDatabase>);
static_assert(!CanCommit<emberstore::Database>);

std::optional<Profile> readUserFromDisk(const TempDir& dir, const std::string& id)
{
    return emberstore::Database {dir.path()}.collection<Profile>("users").get(id);
}

void writeUserToDisk(const TempDir& dir, const std::string& id, const Profile& user)
{
    emberstore::Database {dir.path()}.collection<Profile>("users").set(id, user);
}
} // namespace

auto tStagedSetIsReadButNotWritten = test(
    "TransactionalDatabase/a staged set is visible to reads but not on disk") = []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");

    users.doc("alice").set({"Alice", 1});

    check(users.doc("alice").get()->name == "Alice");
    check(users.contains("alice"));
    check(users.size() == 1);
    check(!readUserFromDisk(dir, "alice").has_value());
    check(!fs::exists(dir.root / "users.json"));
};

auto tCommitWritesStagedSets =
    test("TransactionalDatabase/commit writes staged sets to disk") = []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");
    users.doc("alice").set({"Alice", 1});
    users.doc("bob").set({"Bob", 2});

    check(db.commit());

    check(readUserFromDisk(dir, "alice")->level == 1);
    check(readUserFromDisk(dir, "bob")->level == 2);
    check(users.size() == 2);
};

auto tDiscardChangesFallsBackToDisk = test(
    "TransactionalDatabase/discardChanges drops staged changes and reads use disk") =
    []
{
    auto dir = TempDir {};
    writeUserToDisk(dir, "alice", {"Alice", 1});

    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");
    users.doc("alice").set({"Changed", 9});
    users.doc("bob").set({"Bob", 2});

    db.discardChanges();

    check(!db.hasUncommittedChanges());
    check(users.doc("alice").get()->name == "Alice");
    check(!users.doc("bob").exists());
    check(db.commit());
    check(readUserFromDisk(dir, "alice")->name == "Alice");
    check(!readUserFromDisk(dir, "bob").has_value());
};

auto tStagedRemoveIsCommitted = test(
    "TransactionalDatabase/a staged remove hides the key and commit deletes it") = []
{
    auto dir = TempDir {};
    writeUserToDisk(dir, "alice", {"Alice", 1});

    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");
    users.doc("alice").remove();

    check(!users.doc("alice").exists());
    check(users.empty());
    check(readUserFromDisk(dir, "alice").has_value());

    check(db.commit());
    check(!readUserFromDisk(dir, "alice").has_value());
};

auto tHasUncommittedChanges = test(
    "TransactionalDatabase/hasUncommittedChanges is true only until commit") = []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");

    check(!db.hasUncommittedChanges());
    users.doc("alice").set({"Alice", 1});
    check(db.hasUncommittedChanges());
    check(db.commit());
    check(!db.hasUncommittedChanges());
};

auto tDestroyedWithStagedChangesWritesNothing = test(
    "TransactionalDatabase/changes staged when it is destroyed are not written") = []
{
    auto dir = TempDir {};
    {
        auto db = TransactionalDatabase {dir.path()};
        db.collection<Profile>("users").doc("alice").set({"Alice", 1});
        db.document<Profile>("me").set({"ada", 3});
    }

    check(!fs::exists(dir.root / "users.json"));
    check(!fs::exists(dir.root / "me.json"));
};

auto tReadsMergeStagedOverStored = test(
    "TransactionalDatabase/ids, values and where merge staged changes in id order") =
    []
{
    auto dir = TempDir {};
    auto stored = emberstore::Database {dir.path()}.collection<Profile>("users");
    stored.set("a", {"Ann", 1});
    stored.set("c", {"Cy", 3});
    stored.set("d", {"Di", 4});

    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");
    users.set("b", {"Bo", 2});
    users.set("c", {"Cy II", 30});
    users.remove("d");

    const auto ids = users.ids();
    check(ids.size() == 3);
    check(ids[0] == "a");
    check(ids[1] == "b");
    check(ids[2] == "c");
    check(users.values()[2].name == "Cy II");
    check(users.where([](const Profile& p) { return p.level > 1; }).count() == 2);
};

auto tStagedDocumentValue = test(
    "TransactionalDatabase/a staged document value is read back and committed") = []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& me = db.document<Profile>("me");

    me.set({"ada", 3});

    check(me.get().name == "ada");
    check(emberstore::Database {dir.path()}
              .document<Profile>("me")
              .get()
              .name.empty());
    check(db.commit());
    check(emberstore::Database {dir.path()}.document<Profile>("me").get().level
          == 3);
};

auto tPlainDatabaseWritesWhileChangesWait = test(
    "TransactionalDatabase/a plain Database on the same root allocates ids at once") =
    []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& users = db.collection<Profile>("users");
    auto counter = emberstore::Database {dir.path()}.document<IdCounter>("ids");

    auto allocatedId = 0;
    check(counter.mutate([&](IdCounter& ids) { allocatedId = ids.next++; }));
    users.set(std::to_string(allocatedId), {"Ann", 1});

    check(db.document<IdCounter>("ids").get().next == 1);
    check(!readUserFromDisk(dir, "0").has_value());
    check(db.hasUncommittedChanges());
};

auto tStagedUpdateRunsOnLaterCommittedContent = test(
    "TransactionalDatabase/a staged update runs on what another database committed "
    "after it") = []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& counter = db.document<IdCounter>("ids");
    check(counter.mutate([](IdCounter& ids) { ids.next += 10; }));
    check(counter.get().next == 10);

    auto other = TransactionalDatabase {dir.path()};
    other.document<IdCounter>("ids").set({5});
    check(other.commit());
    check(counter.get().next == 15);

    check(db.commit());
    check(emberstore::Database {dir.path()}.document<IdCounter>("ids").get().next
          == 15);
};

auto tTwoDatabasesKeepEachOthersKeys = test(
    "TransactionalDatabase/two databases on one file commit different keys") = []
{
    auto dir = TempDir {};
    auto first = TransactionalDatabase {dir.path()};
    auto second = TransactionalDatabase {dir.path()};

    first.collection<Profile>("users").set("alice", {"Alice", 1});
    second.collection<Profile>("users").set("bob", {"Bob", 2});
    check(first.commit());
    check(second.commit());

    check(readUserFromDisk(dir, "alice").has_value());
    check(readUserFromDisk(dir, "bob").has_value());
};

auto tLastCommitWinsForOneKey =
    test("TransactionalDatabase/the last commit wins for the same key") = []
{
    auto dir = TempDir {};
    auto first = TransactionalDatabase {dir.path()};
    auto second = TransactionalDatabase {dir.path()};

    second.collection<Profile>("users").set("alice", {"Second", 2});
    first.collection<Profile>("users").set("alice", {"First", 1});
    check(second.commit());
    check(first.commit());

    check(readUserFromDisk(dir, "alice")->name == "First");
};

auto tRealProcessCommitSurvivesOurs = test(
    "TransactionalDatabase/a real process's commit survives a later commit") = []
{
    auto dir = TempDir {};
    auto db = TransactionalDatabase {dir.path()};
    auto& counts = db.collection<int>("counts");
    counts.set("parent", 1);

    const auto output = testing::runContender("commit", dir.path(), "child");
    check(output.find("committed") != std::string::npos);

    check(db.commit());
    check(counts.contains("child"));
    check(counts.contains("parent"));
};
