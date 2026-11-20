// sqlitestore — the firestore demo's twin, backed by SQLite instead of
// emberstore. Same HTTP surface, same Note shape, zero auth (demo only), so
// the two servers can be curled side by side. Its /bench is the head-to-head:
// n single-document inserts through SQLite, an emberstore JSON store and an
// emberstore binary journal store, one table.
//
//   sqlitestore [port] [dataDir]         default: 8081 sqlitestore-data
//
//   GET    /                             this usage text
//   GET    /stats                        collections, doc counts, file bytes
//   GET    /d/{col}                      the whole collection as JSON
//   GET    /d/{col}/{id}                 one document
//   PUT    /d/{col}/{id}                 store the body as the document
//   DELETE /d/{col}/{id}                 remove the document
//   POST   /bench?n=500                  sqlite vs ember json vs ember binary
//                                        (&durability=durable to fsync each)
//
// Storage: one docs.sqlite with docs(col, id, body) keyed on (col, id); the
// body is the document's JSON text — the same serialized form emberstore
// writes, so both sides do the same reflection work per request. WAL mode;
// Durability maps to PRAGMA synchronous (Atomic = OFF, durable = FULL).

#include <emberstore/Emberstore.h>

#include <eacp/Network/Network.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace emberstore;
namespace fs = std::filesystem;

constexpr auto usage = R"(sqlitestore — the firestore demo on SQLite. No auth.

GET    /stats                collections, doc counts, file bytes
GET    /d/{col}              whole collection as JSON
GET    /d/{col}/{id}         one document
PUT    /d/{col}/{id}         store the body as the document
DELETE /d/{col}/{id}         remove the document
POST   /bench?n=500          sqlite vs emberstore json vs emberstore binary
                             (&durability=durable to fsync every write)

Document shape:
  { "title": "...", "body": "...", "rating": 4.5, "tags": ["..."] }
)";

struct Note
{
    std::string title;
    std::string body;
    double rating = 0.0;
    std::vector<std::string> tags;

    MIRO_REFLECT(title, body, rating, tags)
};

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start)
{
    const auto now = Clock::now();
    return std::chrono::duration<double, std::milli> {now - start}.count();
}

std::size_t fileBytes(const std::string& path)
{
    auto error = std::error_code {};
    const auto size = fs::file_size(path, error);
    return error ? 0 : static_cast<std::size_t>(size);
}

// RAII sqlite3 handle with the few operations the demo needs. Statements are
// prepared per call — good enough here, and it keeps the wrapper honest about
// what a naive integration costs. The bench pre-prepares its own.
class Sqlite
{
public:
    Sqlite(const std::string& file, Durability durability)
    {
        if (sqlite3_open(file.c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("sqlite: cannot open " + file);

        exec("PRAGMA journal_mode=WAL");
        exec(durability == Durability::Durable ? "PRAGMA synchronous=FULL"
                                               : "PRAGMA synchronous=OFF");
        exec("CREATE TABLE IF NOT EXISTS docs("
             "col TEXT NOT NULL, id TEXT NOT NULL, body TEXT NOT NULL, "
             "PRIMARY KEY(col, id))");
    }

    ~Sqlite() { sqlite3_close(db); }

    Sqlite(const Sqlite&) = delete;
    Sqlite& operator=(const Sqlite&) = delete;

    void exec(const char* sql)
    {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string {"sqlite: "} + sqlite3_errmsg(db));
    }

    // One prepared statement, bound to string params and stepped to the end.
    // Returns the first column of each result row.
    std::vector<std::string> query(const char* sql,
                                   const std::vector<std::string>& params)
    {
        auto statement = prepare(sql, params);
        auto rows = std::vector<std::string> {};

        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const auto* text = sqlite3_column_text(statement, 0);
            rows.emplace_back(text ? reinterpret_cast<const char*>(text) : "");
        }

        sqlite3_finalize(statement);
        return rows;
    }

    void run(const char* sql, const std::vector<std::string>& params)
    {
        auto statement = prepare(sql, params);
        const auto result = sqlite3_step(statement);
        sqlite3_finalize(statement);

        if (result != SQLITE_DONE && result != SQLITE_ROW)
            throw std::runtime_error(std::string {"sqlite: "} + sqlite3_errmsg(db));
    }

    sqlite3* handle() const { return db; }

private:
    sqlite3_stmt* prepare(const char* sql, const std::vector<std::string>& params)
    {
        auto statement = static_cast<sqlite3_stmt*>(nullptr);

        if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string {"sqlite: "} + sqlite3_errmsg(db));

        for (auto i = 0; i < static_cast<int>(params.size()); ++i)
            sqlite3_bind_text(statement,
                              i + 1,
                              params[static_cast<std::size_t>(i)].c_str(),
                              -1,
                              SQLITE_TRANSIENT);
        return statement;
    }

    sqlite3* db = nullptr;
};

eacp::HTTP::Response
    respond(int statusCode, const std::string& contentType, std::string content)
{
    auto res = eacp::HTTP::Response {};
    res.statusCode = statusCode;
    res.headers["Content-Type"] = contentType;
    res.content = std::move(content);
    return res;
}

eacp::HTTP::Response plain(int statusCode, std::string content)
{
    return respond(statusCode, "text/plain", std::move(content));
}

eacp::HTTP::Response json(int statusCode, std::string content)
{
    return respond(statusCode, "application/json", std::move(content));
}

std::vector<std::string> pathSegments(const std::string& path)
{
    auto out = std::vector<std::string> {};
    auto stream = std::istringstream {path};
    auto segment = std::string {};

    while (std::getline(stream, segment, '/'))
        if (!segment.empty())
            out.push_back(segment);

    return out;
}

// Collection names and ids must be [A-Za-z0-9_-] — mirrors the firestore
// app, where collection names become file names.
bool isSafeName(const std::string& name)
{
    return !name.empty()
           && std::all_of(name.begin(),
                          name.end(),
                          [](unsigned char c)
                          { return std::isalnum(c) || c == '_' || c == '-'; });
}

// Finalizes `statement` (if any) and throws with SQLite's error message.
[[noreturn]] void benchFailed(sqlite3* db, sqlite3_stmt* statement)
{
    const auto message = std::string {"sqlite: "} + sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    throw std::runtime_error(message);
}

Note benchNote(int index)
{
    auto note = Note {};
    note.title = "note-" + std::to_string(index);
    note.body =
        "The quick brown fox jumps over the lazy dog, take " + std::to_string(index);
    note.rating = static_cast<double>(index % 50) / 10.0;
    note.tags = {"bench", index % 2 == 0 ? "even" : "odd"};
    return note;
}

// The head-to-head: n single-document upserts through a fresh SQLite file
// (prepared statement, one implicit transaction per insert — each write
// standing alone, matching how the stores persist per set), a fresh
// emberstore JSON store, and a fresh emberstore binary journal store.
std::string runBench(const std::string& dataDir, int count, Durability durability)
{
    const auto dir = fs::path {dataDir} / "bench";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const auto sqlitePath = (dir / "docs.sqlite").string();
    const auto jsonPath = (dir / "docs.json").string();
    const auto binaryPath = (dir / "docs.ember").string();

    const auto sqliteMs = [&]
    {
        auto db = Sqlite {sqlitePath, durability};
        auto statement = static_cast<sqlite3_stmt*>(nullptr);
        if (sqlite3_prepare_v2(db.handle(),
                               "INSERT OR REPLACE INTO docs(col, id, body) "
                               "VALUES('bench', ?1, ?2)",
                               -1,
                               &statement,
                               nullptr)
            != SQLITE_OK)
            benchFailed(db.handle(), statement);

        const auto start = Clock::now();

        for (auto i = 0; i < count; ++i)
        {
            const auto id = "note-" + std::to_string(i);
            const auto body = Miro::toJSONString(benchNote(i));
            sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, body.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement) != SQLITE_DONE)
                benchFailed(db.handle(), statement);
            sqlite3_reset(statement);
        }

        const auto ms = elapsedMs(start);
        sqlite3_finalize(statement);
        return ms;
    }();

    const auto storeMs = [count](Collection<Note>& target)
    {
        const auto start = Clock::now();
        for (auto i = 0; i < count; ++i)
            target.set("note-" + std::to_string(i), benchNote(i));
        return elapsedMs(start);
    };

    auto jsonStore = Collection<Note> {jsonPath, Format::Json, durability};
    auto binaryStore = Collection<Note> {binaryPath, Format::Binary, durability};

    const auto jsonMs = storeMs(jsonStore);
    const auto binaryMs = storeMs(binaryStore);

    // WAL keeps recent writes beside the main file; count both for honesty.
    const auto sqliteBytes = fileBytes(sqlitePath) + fileBytes(sqlitePath + "-wal");

    const auto line =
        [count](const char* name, double ms, std::size_t bytes, const char* how)
    {
        auto out = std::ostringstream {};
        out << std::fixed << std::setprecision(1);
        out << "  " << name << "  " << ms << " ms   "
            << static_cast<int>(count / (ms / 1000.0)) << " writes/s   "
            << bytes / 1024 << " KB   (" << how << ")\n";
        return out.str();
    };

    // Point reads over the freshly written stores, ids in scattered order.
    // Both sides end with a typed Note — SQLite pays the JSON parse an app
    // needing the value would, the stores answer from the stamped cache
    // (one stat + map find + copy). Ratings are summed so nothing is
    // optimized away.
    const auto idAt = [count](int i)
    { return "note-" + std::to_string((i * 7919) % count); };

    auto readSum = 0.0;

    const auto sqliteReadMs = [&]
    {
        auto db = Sqlite {sqlitePath, durability};
        auto statement = static_cast<sqlite3_stmt*>(nullptr);
        if (sqlite3_prepare_v2(db.handle(),
                               "SELECT body FROM docs WHERE col='bench' AND id=?1",
                               -1,
                               &statement,
                               nullptr)
            != SQLITE_OK)
            benchFailed(db.handle(), statement);

        const auto start = Clock::now();

        for (auto i = 0; i < count; ++i)
        {
            const auto id = idAt(i);
            sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);

            const auto result = sqlite3_step(statement);
            if (result == SQLITE_ROW)
            {
                const auto* text = sqlite3_column_text(statement, 0);
                const auto note = Miro::createFromJSONString<Note>(
                    reinterpret_cast<const char*>(text));
                readSum += note.rating;
            }
            else if (result != SQLITE_DONE)
                benchFailed(db.handle(), statement);

            sqlite3_reset(statement);
        }

        const auto ms = elapsedMs(start);
        sqlite3_finalize(statement);
        return ms;
    }();

    const auto storeReadMs = [&](Collection<Note>& target)
    {
        const auto start = Clock::now();

        for (auto i = 0; i < count; ++i)
            if (auto note = target.get(idAt(i)))
                readSum += note->rating;

        return elapsedMs(start);
    };

    const auto jsonReadMs = storeReadMs(jsonStore);
    const auto binaryReadMs = storeReadMs(binaryStore);

    const auto readLine = [count](const char* name, double ms, const char* how)
    {
        auto out = std::ostringstream {};
        out << std::fixed << std::setprecision(1);
        out << "  " << name << "  " << ms << " ms   "
            << static_cast<int>(count / (ms / 1000.0)) << " reads/s   (" << how
            << ")\n";
        return out.str();
    };

    auto out = std::ostringstream {};
    out << count << " single-document inserts, "
        << (durability == Durability::Durable
                ? "fsync every write (sqlite synchronous=FULL)"
                : "no fsync (sqlite synchronous=OFF)")
        << ":\n"
        << line("sqlite      ",
                sqliteMs,
                sqliteBytes,
                "INSERT OR REPLACE, WAL, incl. -wal")
        << line("ember json  ",
                jsonMs,
                fileBytes(jsonPath),
                "rewrites the whole file")
        << line("ember binary", binaryMs, fileBytes(binaryPath), "appends one frame")
        << "\n"
        << count << " point reads (typed — parsed to a Note, sum "
        << static_cast<int>(readSum) << "):\n"
        << readLine("sqlite      ", sqliteReadMs, "prepared SELECT + JSON parse")
        << readLine("ember json  ", jsonReadMs, "stamped cache: stat + find")
        << readLine("ember binary", binaryReadMs, "stamped cache: stat + find");
    return out.str();
}

std::string statsFor(Sqlite& db, const std::string& file)
{
    auto out = std::ostringstream {};
    out << "db file: " << file << "  " << fileBytes(file) << " bytes ("
        << fileBytes(file + "-wal") << " in -wal)\n";

    for (const auto& name: db.query("SELECT DISTINCT col FROM docs", {}))
    {
        const auto count =
            db.query("SELECT COUNT(*) FROM docs WHERE col = ?1", {name});
        out << "  /d/" << name << "  " << (count.empty() ? "0" : count[0])
            << " docs\n";
    }

    return out.str();
}

eacp::HTTP::Response handleDocument(Sqlite& db,
                                    const eacp::HTTP::Request& req,
                                    const std::string& name,
                                    const std::string& id)
{
    if (req.type == "GET")
    {
        const auto rows =
            db.query("SELECT body FROM docs WHERE col = ?1 AND id = ?2", {name, id});

        if (rows.empty())
            return plain(404, "no such document\n");

        return json(200, rows[0]);
    }

    if (req.type == "PUT")
    {
        // Parse and re-serialize, so a PUT costs the same reflection work as
        // the emberstore twin and stores the same canonical JSON.
        const auto note = Miro::createFromJSONString<Note>(req.body);
        const auto body = Miro::toJSONString(note, 2);

        const auto start = Clock::now();
        db.run("INSERT OR REPLACE INTO docs(col, id, body) VALUES(?1, ?2, ?3)",
               {name, id, body});
        const auto micros = static_cast<int>(elapsedMs(start) * 1000.0);

        return json(200,
                    "{ \"ok\": true, \"writeMicros\": " + std::to_string(micros)
                        + " }");
    }

    if (req.type == "DELETE")
    {
        db.run("DELETE FROM docs WHERE col = ?1 AND id = ?2", {name, id});
        return json(200, "{ \"ok\": true }");
    }

    return plain(405, "use GET, PUT or DELETE\n");
}

eacp::HTTP::Response handle(Sqlite& db,
                            const std::string& dataDir,
                            const std::string& dbFile,
                            const eacp::HTTP::Request& req)
{
    const auto segments = pathSegments(req.pathWithoutQuery());

    if (segments.empty())
        return plain(200, usage);

    if (segments[0] == "stats")
        return plain(200, statsFor(db, dbFile));

    if (segments[0] == "bench" && req.type == "POST")
    {
        const auto requested =
            req.hasParam("n") ? std::atoi(req.getParam("n").c_str()) : 500;
        const auto count = std::clamp(requested, 1, 20000);
        const auto durability = req.getParam("durability") == "durable"
                                    ? Durability::Durable
                                    : Durability::Atomic;

        return plain(200, runBench(dataDir, count, durability));
    }

    if (segments[0] == "d"
        && !std::all_of(segments.begin() + 1, segments.end(), isSafeName))
        return plain(400, "names and ids must be [A-Za-z0-9_-]\n");

    if (segments[0] == "d" && segments.size() == 2 && req.type == "GET")
    {
        const auto rows =
            db.query("SELECT json_group_object(id, json(body)) FROM docs "
                     "WHERE col = ?1",
                     {segments[1]});

        return json(200, rows.empty() || rows[0].empty() ? "{}" : rows[0]);
    }

    if (segments[0] == "d" && segments.size() == 3)
        return handleDocument(db, req, segments[1], segments[2]);

    return plain(404, "not found — GET / for usage\n");
}
} // namespace

int main(int argc, char** argv)
{
    const auto port = argc > 1 ? std::atoi(argv[1]) : 8081;
    const auto dataDir = std::string {argc > 2 ? argv[2] : "sqlitestore-data"};

    fs::create_directories(dataDir);
    const auto dbFile = (fs::path {dataDir} / "docs.sqlite").string();
    auto db = Sqlite {dbFile, Durability::Atomic};

    auto options = eacp::HTTP::ServerOptions {};
    options.bindTo = eacp::BindInterface::any;

    auto server = eacp::HTTP::Server {options};
    const auto ok =
        server.listen(port,
                      [&db, &dataDir, &dbFile](const eacp::HTTP::Request& req)
                      {
                          try
                          {
                              return handle(db, dataDir, dbFile, req);
                          }
                          catch (const std::exception& e)
                          {
                              return plain(500, std::string {e.what()} + "\n");
                          }
                      });

    if (!ok)
    {
        std::cerr << "sqlitestore: failed to listen on port " << port << "\n";
        return 1;
    }

    std::cout << "sqlitestore listening on http://localhost:" << port
              << "  (all interfaces, NO AUTH — demo only)\n"
              << "db file: " << dbFile << "\n"
              << "try: curl -X POST 'localhost:" << port << "/bench?n=1000'\n";

    eacp::Threads::runEventLoop();
    return 0;
}
