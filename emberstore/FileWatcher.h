#pragma once

#include <eacp/Core/Utils/FilePath.h>

#include <functional>
#include <memory>

namespace emberstore
{
// Watches a single file for changes using native OS notifications (FSEvents on
// macOS). It watches the file's parent directory and filters by name, so it
// survives the atomic temp + rename saves our own writer and editors like nvim
// use — a per-descriptor watch would go stale when the inode is swapped.
//
// `onChange` is delivered on the main (message) thread, so a handler can touch
// UI / bridge state directly. No callback fires after destruction.
class FileWatcher
{
public:
    using Callback = std::function<void()>;

    FileWatcher(const eacp::FilePath& path, Callback onChange);
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace emberstore
