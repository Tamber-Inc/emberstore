#include "ContestFile.h"

#include <emberstore/FileLock.h>

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace emberstore::testing
{
namespace
{
std::string selfExecutable;

eacp::Vector<std::string> contestArguments(const std::string& path,
                                           const std::string& mode,
                                           const std::string& arg)
{
    auto arguments = eacp::Vector<std::string> {};
    arguments.add("--contest-file");
    arguments.add(path);
    arguments.add(mode);
    if (!arg.empty())
        arguments.add(arg);
    return arguments;
}
} // namespace

void rememberSelfExecutable(const char* argv0)
{
    auto error = std::error_code {};
    const auto absolute = std::filesystem::absolute(argv0, error);
    selfExecutable = error ? std::string {argv0} : absolute.string();
}

int runIfContestInvocation(int argc, char** argv)
{
    if (argc < 4 || std::string {argv[1]} != "--contest-file")
        return -1;

    const auto path = std::string {argv[2]};
    const auto mode = std::string {argv[3]};
    const auto arg = argc > 4 ? std::string {argv[4]} : std::string {};

    // Ignores the lock entirely — what a text editor does.
    if (mode == "write")
    {
        auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
        out << arg;
        out.flush();
        std::puts("wrote");
        return 0;
    }

    auto lock = emberstore::FileLock {path};
    if (!lock.tryAcquire())
    {
        std::puts("busy");
        std::fflush(stdout);
        return 2;
    }

    // Announced before doing anything else, so the parent can wait for a known
    // state instead of guessing when we got the lock.
    std::puts("locked");
    std::fflush(stdout);

    if (mode == "try")
        return 0; // released on exit

    if (mode == "crash")
        std::abort(); // die holding it; only the OS can clean this up

    if (mode == "hold")
    {
        const auto holdForMs = arg.empty() ? 1000 : std::stoi(arg);
        std::this_thread::sleep_for(std::chrono::milliseconds {holdForMs});
        return 0;
    }

    return 64; // unknown mode
}

std::string runContender(const std::string& mode,
                         const std::string& path,
                         const std::string& arg)
{
    const auto result =
        eacp::Processes::run(selfExecutable, contestArguments(path, mode, arg));
    return result.output;
}

std::unique_ptr<eacp::Processes::Process> startLockHolder(const std::string& path,
                                                          int holdForMs)
{
    auto child = std::make_unique<eacp::Processes::Process>(
        selfExecutable, contestArguments(path, "hold", std::to_string(holdForMs)));
    if (!child->launched())
        return nullptr;

    // Wait for the child to say it has the lock.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (child->output().find("locked") != std::string::npos)
            return child;
        if (!child->isRunning())
            return nullptr; // died or failed to take it
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return nullptr;
}
} // namespace emberstore::testing
