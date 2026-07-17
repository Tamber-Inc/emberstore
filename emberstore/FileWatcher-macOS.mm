#include "FileWatcher.h"

#include <eacp/Core/Utils/StdPath.h>

#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace emberstore
{
namespace fs = std::filesystem;

struct FileWatcher::Impl
{
    Impl(const fs::path& path, Callback onChangeToUse)
        : onChange(std::move(onChangeToUse))
        , filename(path.filename().string())
    {
        // FSEvents needs the directory to exist; the store creates it on first
        // write, but the watcher may start before that.
        auto directory = path.parent_path();
        auto error = std::error_code {};
        fs::create_directories(directory, error);

        auto* directoryString =
            [NSString stringWithUTF8String:directory.c_str()];
        auto* paths = @[directoryString];

        auto context = FSEventStreamContext {0, this, nullptr, nullptr, nullptr};
        stream = FSEventStreamCreate(
            kCFAllocatorDefault, &Impl::onEvents, &context,
            (__bridge CFArrayRef) paths, kFSEventStreamEventIdSinceNow, 0.1,
            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
        if (stream == nullptr)
            return;

        // Delivered on the main queue — drained by the main run loop — so
        // onEvents fires on the main thread.
        FSEventStreamSetDispatchQueue(stream, dispatch_get_main_queue());
        FSEventStreamStart(stream);
    }

    ~Impl()
    {
        if (stream == nullptr)
            return;
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
    }

    static void onEvents(ConstFSEventStreamRef,
                         void* info,
                         size_t numEvents,
                         void* eventPaths,
                         const FSEventStreamEventFlags[],
                         const FSEventStreamEventId[])
    {
        auto* self = static_cast<Impl*>(info);
        auto** paths = static_cast<char**>(eventPaths);
        for (size_t i = 0; i < numEvents; ++i)
        {
            if (fs::path {paths[i]}.filename().string() == self->filename)
            {
                self->onChange();
                return;
            }
        }
    }

    Callback onChange;
    std::string filename;
    FSEventStreamRef stream = nullptr;
};

FileWatcher::FileWatcher(const eacp::FilePath& path, Callback onChange)
    : impl(std::make_unique<Impl>(eacp::toStdPath(path), std::move(onChange)))
{
}

FileWatcher::~FileWatcher() = default;
} // namespace emberstore
