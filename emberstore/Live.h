#pragma once

#include "Document.h"

#include <Miro/Bridge.h>
#include "FileWatcher.h"

#include <memory>
#include <string>
#include <utility>

namespace emberstore
{
// Couples a Document<T> with a Miro::Event<T> so every write also publishes the
// new value over the bridge — the web side then gets a live, auto-updating
// hook (Firestore's onSnapshot, but local). You declare the Event as a
// reflected member of your API and hand it to LiveDocument; get/set forward to
// both the file and the event. Name the event to match the getter (event `dog`
// ↔ `getDog`) and the codegen pairs them into one `useDog()` hook:
//
//   class DemoApi
//   {
//   public:
//       Dog  getDog() { return store.get(); }
//       void setDog(const Dog& d)                      // writes to disk + publishes
//       {
//           if (!store.set(d))
//               throw std::runtime_error("write failed"); // rejects the JS promise
//       }
//       Miro::Event<Dog> dog;                          // reflected → live useDog()
//       MIRO_REFLECT_API(getDog, setDog, dog)
//   private:
//       emberstore::Database db {dataDirectory()};
//       emberstore::LiveDocument<Dog> store {db.document<Dog>("dog"), dog};
//   };
//
// Call watchForExternalChanges() (from the message thread) to also republish
// when the file changes underneath us — another program, a sibling app,
// or your editor. Message-thread only, like the Document it wraps.
template <typename T>
class LiveDocument
{
public:
    LiveDocument(Document<T> documentToUse, Miro::Event<T>& eventToUse)
        : document(std::move(documentToUse))
        , event(eventToUse)
    {
    }

    T get() { return document.get(); }

    const eacp::FilePath& filePath() const { return document.filePath(); }

    // Persist the value, then publish it to every subscriber. A failed disk
    // write suppresses the publish, so subscribers never see a value that
    // isn't actually on disk.
    bool set(const T& value)
    {
        if (!document.set(value))
            return false;
        lastPublishedJson = Miro::toJSONString(value);
        event.publish(value);
        return true;
    }

    // Read-modify-write, then publish the result.
    //
    // Handed to the Document rather than composed here out of get() + set():
    // that would drop the inter-process lock between the read and the write,
    // and a sibling process's change could land in the gap and be edited over.
    // Document::mutate holds the lock across the whole cycle.
    template <typename Fn>
    bool update(Fn&& fn)
    {
        if (!document.mutate(std::forward<Fn>(fn)))
            return false;

        auto value = document.get(); // cache hit — we just wrote it
        lastPublishedJson = Miro::toJSONString(value);
        event.publish(value);
        return true;
    }

    // Start republishing when the file changes on disk from outside this store.
    // The watcher delivers on the message thread, so the republish is safe.
    void watchForExternalChanges()
    {
        lastPublishedJson = Miro::toJSONString(document.get());
        watcher = std::make_unique<FileWatcher>(
            document.filePath(), [this] { republishExternalChange(); });
    }

private:
    // Re-read the file and publish only if the content actually changed — this
    // filters the watcher event our own writes trigger, and any no-op touch.
    void republishExternalChange()
    {
        auto value = document.get();
        auto json = Miro::toJSONString(value);
        if (json == lastPublishedJson)
            return;
        lastPublishedJson = std::move(json);
        event.publish(value);
    }

    Document<T> document;
    Miro::Event<T>& event;
    std::string lastPublishedJson;
    std::unique_ptr<FileWatcher> watcher;
};
} // namespace emberstore
