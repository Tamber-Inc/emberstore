#pragma once

#include <eacp/Core/Process/Process.h>

#include <memory>
#include <string>

namespace emberstore::testing
{
// Real multi-process contention for file-locking tests, without a second
// binary: the test executable re-invokes *itself* as
//
//   <self> --contest-file <path> <mode> [arg]
//
// and that child becomes a genuine OS process fighting over the file. Modes:
//
//   try          take the lock, report "locked" or "busy", exit (releasing it)
//   hold <ms>    take the lock, report "locked", hold it that long, then exit
//   crash        take the lock, report "locked", then die still holding it
//   write <text> overwrite the file ignoring the lock, as an editor would
//
// Needed because a second FileLock inside one process only proves the lock
// binds to the descriptor. It says nothing about whether the lock actually
// crosses a process boundary, and nothing at all about the OS releasing it when
// a holder dies — which is the property the design leans on.
//
// Wired up by linking TestContest instead of NanoTestMain; see ContestMain.cpp.

// Records argv[0] so children can be launched from this same executable.
void rememberSelfExecutable(const char* argv0);

// Runs the contender and returns its exit code when argv is a --contest-file
// invocation; returns -1 otherwise, meaning "carry on and run the tests".
int runIfContestInvocation(int argc, char** argv);

// Launches a contender and blocks until it exits, returning what it printed
// ("locked", "busy", "wrote").
std::string runContender(const std::string& mode,
                         const std::string& path,
                         const std::string& arg = {});

// Launches a contender that takes the lock and holds it, returning once the
// child has *announced* it holds the lock — so a caller tests against a known
// state rather than racing the child's startup. Null if it never got there.
std::unique_ptr<eacp::Processes::Process> startLockHolder(const std::string& path,
                                                          int holdForMs);
} // namespace emberstore::testing
