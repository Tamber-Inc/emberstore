// firestore — emberstore over plain HTTP, with zero auth (unsecured on
// purpose; keep it off networks you don't trust). A demo for feeling the
// write throughput of the binary journal format: every write reports its
// server-side cost, and /bench races the JSON store against the binary one.
//
//   firestore [port] [dataDir]           default: 8080 firestore-data
//
//   GET    /                             this usage text
//   GET    /stats                        collections, doc counts, file bytes
//   GET    /d/{col}                      the whole collection as JSON
//   GET    /d/{col}/{id}                 one document
//   PUT    /d/{col}/{id}                 store the body as the document
//   DELETE /d/{col}/{id}                 remove the document
//   POST   /bench?n=500                  n inserts, JSON store vs binary
//                                        (&durability=durable to fsync each)
//
// Documents share one shape:
//   { "title": "...", "body": "...", "rating": 4.5, "tags": ["..."] }
//
// Try:
//   curl -X PUT localhost:8080/d/notes/a -d '{"title":"hi","rating":5}'
//   curl localhost:8080/d/notes
//   curl -X POST 'localhost:8080/bench?n=1000'

#include <emberstore/Emberstore.h>

#include <eacp/Network/Network.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using namespace emberstore;
namespace fs = std::filesystem;

constexpr auto usage = R"(firestore — emberstore over HTTP. No auth. Demo only.

GET    /stats                collections, doc counts, file bytes
GET    /d/{col}              whole collection as JSON
GET    /d/{col}/{id}         one document
PUT    /d/{col}/{id}         store the body as the document
DELETE /d/{col}/{id}         remove the document
POST   /bench?n=500          n inserts, JSON store vs binary store
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

// The live stores. Handles are cached so repeated requests hit the in-memory
// value and each PUT costs only its own append.
struct Store
{
    Collection<Note>& collection(const std::string& name)
    {
        auto it = collections.find(name);
        if (it == collections.end())
            it = collections.try_emplace(name, db.collection<Note>(name)).first;
        return it->second;
    }

    std::string filePathFor(const std::string& name) const
    {
        return (fs::path {dir} / (name + ".ember")).string();
    }

    std::string dir;
    Database db {dir, Format::Binary, Durability::Atomic};
    std::map<std::string, Collection<Note>> collections;
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

// Collection names become file names under the store root, so anything
// beyond [A-Za-z0-9_-] (a separator, "..") is refused. Ids get the same rule
// for symmetry.
bool isSafeName(const std::string& name)
{
    return !name.empty()
           && std::all_of(name.begin(),
                          name.end(),
                          [](unsigned char c)
                          { return std::isalnum(c) || c == '_' || c == '-'; });
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

// Races n single-document inserts through a fresh JSON store and a fresh
// binary store. The JSON store rewrites the whole file per insert (O(n)
// each); the binary store appends one journal frame (O(1)) — so the gap
// widens as n grows.
std::string runBench(const Store& store, int count, Durability durability)
{
    const auto dir = fs::path {store.dir} / "bench";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const auto jsonPath = (dir / "docs.json").string();
    const auto binaryPath = (dir / "docs.ember").string();

    const auto run = [count](Collection<Note>& target)
    {
        const auto start = Clock::now();
        for (auto i = 0; i < count; ++i)
            target.set("note-" + std::to_string(i), benchNote(i));
        return elapsedMs(start);
    };

    auto jsonStore = Collection<Note> {jsonPath, Format::Json, durability};
    auto binaryStore = Collection<Note> {binaryPath, Format::Binary, durability};

    const auto jsonMs = run(jsonStore);
    const auto binaryMs = run(binaryStore);

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

    auto out = std::ostringstream {};
    out << std::fixed << std::setprecision(1);
    out << count << " single-document inserts, "
        << (durability == Durability::Durable ? "fsync every write"
                                              : "no fsync (Durability::Atomic)")
        << ":\n"
        << line("json  ", jsonMs, fileBytes(jsonPath), "rewrites the whole file")
        << line("binary", binaryMs, fileBytes(binaryPath), "appends one frame")
        << "binary is " << jsonMs / binaryMs
        << "x faster here, and the gap grows with collection size.\n";
    return out.str();
}

std::string statsFor(Store& store)
{
    auto out = std::ostringstream {};
    out << "data dir: " << store.dir << " (Format::Binary, .ember journals)\n";

    if (store.collections.empty())
        out << "no collections touched yet this run\n";

    for (auto& [name, collection]: store.collections)
    {
        const auto path = store.filePathFor(name);
        out << "  /d/" << name << "  " << collection.size() << " docs   "
            << fileBytes(path) << " bytes on disk\n";
    }

    return out.str();
}

eacp::HTTP::Response handleDocument(Store& store,
                                    const eacp::HTTP::Request& req,
                                    const std::string& name,
                                    const std::string& id)
{
    auto& collection = store.collection(name);

    if (req.type == "GET")
    {
        if (auto note = collection.get(id))
            return json(200, Miro::toJSONString(*note, 2));

        return plain(404, "no such document\n");
    }

    if (req.type == "PUT")
    {
        const auto note = Miro::createFromJSONString<Note>(req.body);
        const auto start = Clock::now();
        const auto ok = collection.set(id, note);
        const auto micros = static_cast<int>(elapsedMs(start) * 1000.0);

        if (!ok)
            return plain(500, "write failed\n");

        return json(200,
                    "{ \"ok\": true, \"writeMicros\": " + std::to_string(micros)
                        + " }");
    }

    if (req.type == "DELETE")
    {
        if (!collection.remove(id))
            return plain(500, "remove failed\n");
        return json(200, "{ \"ok\": true }");
    }

    return plain(405, "use GET, PUT or DELETE\n");
}

eacp::HTTP::Response handle(Store& store, const eacp::HTTP::Request& req)
{
    const auto segments = pathSegments(req.pathWithoutQuery());

    if (segments.empty())
        return plain(200, usage);

    if (segments[0] == "stats")
        return plain(200, statsFor(store));

    if (segments[0] == "bench" && req.type == "POST")
    {
        const auto requested =
            req.hasParam("n") ? std::atoi(req.getParam("n").c_str()) : 500;
        const auto count = std::clamp(requested, 1, 20000);
        const auto durability = req.getParam("durability") == "durable"
                                    ? Durability::Durable
                                    : Durability::Atomic;

        return plain(200, runBench(store, count, durability));
    }

    if (segments[0] == "d"
        && !std::all_of(segments.begin() + 1, segments.end(), isSafeName))
        return plain(400, "names and ids must be [A-Za-z0-9_-]\n");

    if (segments[0] == "d" && segments.size() == 2 && req.type == "GET")
    {
        auto& collection = store.collection(segments[1]);
        auto all = std::map<std::string, Note> {};

        for (const auto& id: collection.ids())
            if (auto note = collection.get(id))
                all[id] = *note;

        return json(200, Miro::toJSONString(all, 2));
    }

    if (segments[0] == "d" && segments.size() == 3)
        return handleDocument(store, req, segments[1], segments[2]);

    return plain(404, "not found — GET / for usage\n");
}
} // namespace

int main(int argc, char** argv)
{
    const auto port = argc > 1 ? std::atoi(argv[1]) : 8080;
    auto store = Store {argc > 2 ? argv[2] : "firestore-data"};

    // Handlers dispatch onto the event loop below, which is the main thread —
    // exactly what emberstore's message-thread-only contract wants.
    auto options = eacp::HTTP::ServerOptions {};
    options.bindTo = eacp::BindInterface::any;

    auto server = eacp::HTTP::Server {options};
    const auto ok =
        server.listen(port,
                      [&store](const eacp::HTTP::Request& req)
                      {
                          try
                          {
                              return handle(store, req);
                          }
                          catch (const std::exception& e)
                          {
                              return plain(500, std::string {e.what()} + "\n");
                          }
                      });

    if (!ok)
    {
        std::cerr << "firestore: failed to listen on port " << port << "\n";
        return 1;
    }

    std::cout << "firestore listening on http://localhost:" << port
              << "  (all interfaces, NO AUTH — demo only)\n"
              << "data dir: " << store.dir << "\n"
              << "try: curl -X POST 'localhost:" << port << "/bench?n=1000'\n";

    eacp::Threads::runEventLoop();
    return 0;
}
