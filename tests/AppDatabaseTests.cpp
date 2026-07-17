// Pins the on-disk location of every app's store. These paths are where real
// user data lives, so a change here silently strands it — that's what these
// tests exist to catch.

#include <emberstore/AppDatabase.h>

#include <NanoTest/NanoTest.h>

#include <eacp/Core/Utils/FilePath.h>

#include <string>

using namespace nano;
using namespace emberstore;

namespace
{
std::string expectedRoot(const std::string& appName)
{
    return (eacp::FilePath::appDataDirectory() / "Acme" / appName).str();
}
} // namespace

auto tRootIsUnderTheCompanyDirectory =
    test("AppDatabase/an app's store sits under <app data>/<company>/<app>") = []
{
    check(emberstore::appDataDirectory("Acme", "Notes").str()
          == expectedRoot("Notes"));
};

auto tDocumentPathsAreStable =
    test("AppDatabase/documents keep the paths their data is at") = []
{
    const auto db = emberstore::databaseForApp("Acme", "Notes");
    const auto root = expectedRoot("Notes");

    check(db.document<int>("crate-library").filePath().str()
          == root + "/crate-library.json");
    check(db.document<int>("window-position").filePath().str()
          == root + "/window-position.json");
};

auto tAppsGetSeparateRoots =
    test("AppDatabase/two apps never share a store directory") = []
{
    check(emberstore::appDataDirectory("Acme", "Notes").str()
          != emberstore::appDataDirectory("Acme", "Sketch").str());
};

auto tRootIsNotTheCompanyDirectory =
    test("AppDatabase/an app's store never collapses to the company dir") = []
{
    // If it did, every app would land on the shared company directory and
    // scribble over each other.
    const auto company = (eacp::FilePath::appDataDirectory() / "Acme").str();
    check(emberstore::appDataDirectory("Acme", "Notes").str() != company);
};
