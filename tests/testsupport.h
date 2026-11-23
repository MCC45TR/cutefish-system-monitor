// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "fileutil.h"
#include "monitor.h"
#include "processrecord.h"

#include "base/event_loop.h"
#include "base/timer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
namespace base = cutefish::base;
namespace fs = std::filesystem;

inline std::string processStat(int pid, const std::string& name, uint64_t ticks, uint64_t start, uint32_t flags = 0)
{
    std::vector<std::string> fields(22, "0");
    fields[0] = "S";
    fields[1] = "42";
    fields[6] = std::to_string(flags);
    fields[11] = std::to_string(ticks);
    fields[16] = "5";
    fields[17] = "3";
    fields[19] = std::to_string(start);
    fields[21] = "7";
    std::string result = std::to_string(pid) + " (" + name + ")";
    for (const std::string& field : fields)
        result += ' ' + field;
    return result;
}

inline std::string commandLine(const std::vector<std::string>& arguments)
{
    std::string result;
    for (const std::string& argument : arguments)
        result += argument + '\0';
    return result;
}

inline bool writeFile(const std::string& path, const std::string& contents)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return bool(file);
}

inline ProcessRecord record(int pid, const std::string& name, double cpu = 0)
{
    ProcessRecord process;
    process.pid = pid;
    process.uid = getuid();
    process.name = name;
    process.displayName = name;
    process.command = "/usr/bin/" + name + " --example";
    process.user = "test-user";
    process.startTicks = 1000;
    process.cpu = cpu;
    process.memory = uint64_t(pid) * 1024;
    return process;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        std::string pattern = (fs::temp_directory_path() / "monitor-test-XXXXXX").string();
        m_path = mkdtemp(pattern.data());
    }
    ~TemporaryDirectory() { fs::remove_all(m_path); }
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

// A child process, killed when it goes.
class Child
{
public:
    explicit Child(std::vector<std::string> arguments, bool pipeInput = false)
    {
        int input[2] = {-1, -1};
        if (pipeInput && pipe(input) != 0)
            return;
        m_pid = fork();
        if (m_pid == 0) {
            if (pipeInput) {
                dup2(input[0], STDIN_FILENO);
                close(input[0]);
                close(input[1]);
            }
            std::vector<char*> argv;
            for (std::string& argument : arguments)
                argv.push_back(argument.data());
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
        if (pipeInput) {
            close(input[0]);
            m_input = input[1];
        }
        // Until it runs the program, the child is a copy of the test.
        const std::string program = fs::canonical(arguments[0]).string();
        for (int i = 0; i < 300 && symLinkTarget("/proc/" + std::to_string(m_pid) + "/exe") != program; ++i)
            usleep(10000);
    }
    ~Child()
    {
        if (m_input >= 0)
            close(m_input);
        if (m_pid > 0 && !m_finished) {
            kill(m_pid, SIGKILL);
            waitpid(m_pid, nullptr, 0);
        }
    }

    int pid() const { return m_pid; }
    void closeInput()
    {
        close(m_input);
        m_input = -1;
    }
    bool running()
    {
        if (m_finished)
            return false;
        int status = 0;
        if (waitpid(m_pid, &status, WNOHANG) == m_pid)
            m_finished = true;
        return !m_finished;
    }
    bool waitForFinished(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running() && std::chrono::steady_clock::now() < deadline)
            usleep(5000);
        return m_finished;
    }

private:
    int m_pid = -1;
    int m_input = -1;
    bool m_finished = false;
};

inline std::string ownExecutable()
{
    return fs::canonical("/proc/self/exe").string();
}

// Runs the loop until the condition holds, or the time is up.
inline bool runUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5000ms)
{
    if (condition())
        return true;
    base::EventLoop& loop = *base::EventLoop::current();
    base::Timer poll;
    poll.onTimeout = [&] {
        if (condition())
            loop.quit();
    };
    poll.start(5ms);
    base::Timer deadline;
    deadline.setSingleShot(true);
    deadline.onTimeout = [&] { loop.quit(); };
    deadline.start(timeout);
    loop.run();
    return condition();
}

class MonitorTest : public testing::Test
{
protected:
    base::EventLoop m_loop;
};

// Desktop entries as the session's registry would resolve them: by id first,
// then by the program the entry runs.
struct FixtureApplications
{
    struct Entry
    {
        DesktopApplication application;
        std::string program;
    };
    std::vector<Entry> entries;

    ApplicationLookup lookup() const
    {
        ApplicationLookup lookup;
        lookup.resolve = [this](const std::string& name, const std::string& program)
            -> std::optional<DesktopApplication> {
            const Entry* best = nullptr;
            int bestScore = 0;
            for (const Entry& entry : entries) {
                int score = 0;
                if (entry.application.id == name)
                    score = 100;
                else if (entry.program == program)
                    score = 85;
                else if (fileName(entry.program) == name)
                    score = 80;
                if (score > bestScore) {
                    best = &entry;
                    bestScore = score;
                }
            }
            return best ? std::optional(best->application) : std::nullopt;
        };
        lookup.byId = [this](const std::string& id) -> std::optional<DesktopApplication> {
            for (const Entry& entry : entries) {
                if (entry.application.id == id)
                    return entry.application;
            }
            return std::nullopt;
        };
        return lookup;
    }
};
