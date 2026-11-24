// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "testsupport.h"
#include "preferences.h"
#include "procreader.h"

#include <algorithm>

TEST(ProcessListTest, UpdatesKeepIdentity)
{
    ProcessList list;
    list.apply({record(10, "first"), record(20, "second")});
    list.apply({record(5, "new"), record(20, "second", 30)});
    EXPECT_EQ(list.processes().size(), 2u);
    EXPECT_EQ(list.find(10), nullptr);
    ASSERT_NE(list.find(20), nullptr);
    EXPECT_EQ(list.find(20)->name, "second");
    EXPECT_EQ(list.find(20)->cpu, 30.0);
    list.apply({});
    EXPECT_EQ(list.find(20), nullptr);
    EXPECT_EQ(list.count(), 0u);
}

TEST(ProcessListTest, FilteringAndSorting)
{
    ProcessList list;
    auto foreign = record(30, "foreign", 90);
    foreign.uid = getuid() + 1;
    list.apply({record(10, "Terminal", 10), record(20, "Browser", 30), foreign});
    EXPECT_EQ(list.count(), 2u);
    EXPECT_EQ(list.at(0)->pid, 20);
    list.setSearch("terminal");
    EXPECT_EQ(list.count(), 1u);
    list.setSearch("--example");
    EXPECT_EQ(list.count(), 2u);
    list.setSearch("20");
    EXPECT_EQ(list.count(), 1u);
    list.setSearch({});
    list.setAllUsers(true);
    EXPECT_EQ(list.count(), 3u);
    EXPECT_EQ(list.at(0)->pid, 30);
    list.sortBy("pid");
    EXPECT_EQ(list.at(0)->pid, 10);
    list.sortBy("pid");
    EXPECT_EQ(list.at(0)->pid, 30);
    EXPECT_EQ(list.indexOf(20), 1);
    EXPECT_EQ(list.at(100), nullptr);
    list.sortBy("name");
    EXPECT_EQ(list.at(0)->displayName, "Browser");
    EXPECT_TRUE(list.ascending());
}

TEST(ProcessListTest, SearchFoldsCase)
{
    ProcessList list;
    auto process = record(10, "Ärger");
    list.apply({process});
    list.setSearch("ärger");
    EXPECT_EQ(list.count(), 1u);
    list.setSearch("  ÄRGER ");
    EXPECT_EQ(list.count(), 1u);
}

class TerminateTest : public testing::TestWithParam<bool>
{
};

TEST_P(TerminateTest, TerminateChild)
{
    const bool force = GetParam();
    Child child({"/usr/bin/sleep", "30"});
    ASSERT_GT(child.pid(), 0);
    ProcessRecord process;
    ASSERT_TRUE(ProcReader::parseProcess(readFile("/proc/" + std::to_string(child.pid()) + "/stat"), &process, 1));
    EXPECT_EQ(Monitor::signalProcess(child.pid(), process.startTicks, force), "");
    EXPECT_TRUE(child.waitForFinished(3000ms));
}

INSTANTIATE_TEST_SUITE_P(Monitor, TerminateTest, testing::Values(false, true));

TEST(SignalTest, RejectReusedAndProtectedPid)
{
    EXPECT_FALSE(Monitor::signalProcess(1, 0, false).empty());
    EXPECT_FALSE(Monitor::signalProcess(getpid(), 0, false).empty());
    Child child({"/usr/bin/sleep", "30"});
    EXPECT_FALSE(Monitor::signalProcess(child.pid(), 0, true).empty());
    EXPECT_TRUE(child.running());
}

TEST(SignalTest, RejectForeignProcess)
{
    // kthreadd belongs to root; the kernel refuses the signal, not the monitor.
    const std::string stat = readFile("/proc/2/stat");
    if (stat.empty() || getuid() == 0)
        GTEST_SKIP() << "Needs an unprivileged user and a visible kthreadd";
    ProcessRecord process;
    ASSERT_TRUE(ProcReader::parseProcess(stat, &process, 1));
    EXPECT_EQ(Monitor::signalProcess(2, process.startTicks, false), "You can only end your own processes.");
}

TEST_F(MonitorTest, DesktopIdentity)
{
    const std::string program = ownExecutable();
    FixtureApplications fixture;
    fixture.entries.push_back({{"a-fixture-shortcut", "Fixture Shortcut", {}}, program});
    fixture.entries.push_back({{fileName(program), "Fixture Application", "fixture-icon"}, program});

    Monitor monitor(fixture.lookup());
    ASSERT_TRUE(runUntil([&] { return monitor.processes().indexOf(getpid()) >= 0; }));
    const ProcessRecord* self = monitor.processes().find(getpid());
    ASSERT_NE(self, nullptr);
    EXPECT_EQ(self->displayName, "Fixture Application");
    EXPECT_EQ(self->iconName, "fixture-icon");
}

TEST_F(MonitorTest, ApplicationGroups)
{
    const std::string program = ownExecutable();
    FixtureApplications fixture;
    fixture.entries.push_back({{fileName(program), "Fixture Application", "fixture-icon"}, program});
    // A program of its own: any other sleep of the user's would join the application.
    TemporaryDirectory directory;
    const std::string sleeper = directory.path() + "/fixture-sleeper";
    fs::copy_file("/usr/bin/sleep", sleeper);
    fs::permissions(sleeper, fs::perms::owner_all);
    fixture.entries.push_back({{"fixture-standalone", "Fixture Standalone", {}}, sleeper});
    DesktopApplication hidden{"cat", "Fixture Hidden", {}};
    hidden.noDisplay = true;
    fixture.entries.push_back({hidden, "/usr/bin/cat"});

    Child helper({"/bin/sh", "-c", "read line"}, true);
    Child separate({sleeper, "30"});
    Child handler({"/usr/bin/cat"}, true);

    Monitor monitor(fixture.lookup());
    ProcessList& applications = monitor.applications();
    ASSERT_TRUE(runUntil([&] {
        return applications.indexOf(getpid()) >= 0 && applications.indexOf(separate.pid()) >= 0;
    }));

    // The helper has no entry of its own and the handler only a hidden one,
    // so both join their parent; the second program has a listed entry and
    // stays apart even though this process started it.
    const ProcessRecord* self = applications.at(size_t(applications.indexOf(getpid())));
    ASSERT_NE(self, nullptr);
    EXPECT_EQ(self->displayName, "Fixture Application");
    EXPECT_EQ(self->iconName, "fixture-icon");
    EXPECT_EQ(self->members.size(), 3u);
    std::vector<int> pids;
    std::vector<std::string> names;
    double memory = 0;
    for (const ProcessRecord& member : self->members) {
        pids.push_back(member.pid);
        names.push_back(ProcessList::programName(member));
        memory += double(member.memory);
    }
    const auto contains = [&names](const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    EXPECT_TRUE(contains(fileName(program)));
    // Inline shell code names no program; the executable behind /bin/sh does.
    EXPECT_TRUE(contains(fileName(canonicalPath("/bin/sh"))));
    EXPECT_TRUE(contains("cat"));
    std::vector<int> expected{int(getpid()), helper.pid(), handler.pid()};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(pids, expected);
    EXPECT_EQ(double(self->memory), memory);
    EXPECT_GT(memory, 0);
    const ProcessRecord* led = monitor.application(getpid(), self->startTicks);
    ASSERT_NE(led, nullptr);
    EXPECT_FALSE(Monitor::canManage(*led));

    // A hidden entry claims no name or icon for its process.
    ProcessList& processes = monitor.processes();
    ASSERT_TRUE(runUntil([&] { return processes.indexOf(handler.pid()) >= 0; }));
    const ProcessRecord* handled = processes.find(handler.pid());
    ASSERT_NE(handled, nullptr);
    EXPECT_EQ(handled->displayName, "cat");
    EXPECT_EQ(handled->iconName, "application-x-executable");

    applications.setSearch(std::to_string(helper.pid()));
    EXPECT_EQ(applications.count(), 1u);
    applications.setSearch({});

    const ProcessRecord* other = applications.at(size_t(applications.indexOf(separate.pid())));
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->displayName, "Fixture Standalone");
    EXPECT_EQ(other->members.size(), 1u);
    const uint64_t start = other->startTicks;
    const ProcessRecord* standalone = monitor.application(separate.pid(), start);
    ASSERT_NE(standalone, nullptr);
    EXPECT_TRUE(Monitor::canManage(*standalone));
    EXPECT_FALSE(monitor.endApplication(separate.pid(), 1, false).empty());
    EXPECT_TRUE(separate.running());
    EXPECT_EQ(monitor.endApplication(separate.pid(), start, false), "");
    EXPECT_TRUE(separate.waitForFinished(3000ms));

    helper.closeInput();
    handler.closeInput();
    EXPECT_TRUE(helper.waitForFinished(3000ms));
    EXPECT_TRUE(handler.waitForFinished(3000ms));
}

TEST_F(MonitorTest, FirstPublishedSample)
{
    Monitor monitor;
    int firstHistory = -1;
    const auto subscription = monitor.updated.connect([&] {
        if (firstHistory < 0 && monitor.ready())
            firstHistory = int(monitor.history().timestamps.size());
    });
    ASSERT_TRUE(runUntil([&] { return monitor.ready(); }));
    // The baseline sample only primes the counters and never reaches the history.
    EXPECT_EQ(firstHistory, 1);
    EXPECT_GT(monitor.metrics().memory.total, 0u);
}

TEST_F(MonitorTest, IntervalPreservesHistory)
{
    Monitor monitor;
    ASSERT_TRUE(runUntil([&] { return monitor.ready(); }));
    const History history = monitor.history();
    EXPECT_FALSE(history.cpuPercent.empty());
    for (const int interval : {2000, 5000, 1000, 5000}) {
        monitor.setInterval(interval);
        EXPECT_EQ(monitor.interval(), interval);
        EXPECT_EQ(monitor.historyDuration(), 60);
    }
    monitor.setInterval(42);
    EXPECT_EQ(monitor.interval(), 5000);
    const auto& timestamps = monitor.history().timestamps;
    EXPECT_GE(timestamps.size(), history.timestamps.size());
    EXPECT_EQ(timestamps.front(), history.timestamps.front());
}

TEST_F(MonitorTest, HistoryPerGpu)
{
    TemporaryDirectory directory;
    const std::string sys = directory.path() + "/sys";
    ASSERT_TRUE(writeFile(sys + "/class/drm/card0/device/gpu_busy_percent", "20\n"));
    ASSERT_TRUE(writeFile(sys + "/class/drm/card1/device/boot_vga", "1\n"));
    ASSERT_TRUE(writeFile(sys + "/class/drm/card1/device/gpu_busy_percent", "80\n"));
    ASSERT_TRUE(writeFile(sys + "/class/drm/card1/device/hwmon/hwmon0/temp1_input", "45000\n"));

    Monitor monitor({}, {}, std::make_shared<ProcReader>("/proc", sys));
    ASSERT_TRUE(runUntil([&] { return monitor.history().timestamps.size() >= 2; }));
    const History& history = monitor.history();
    ASSERT_EQ(history.gpus.size(), 2u);
    EXPECT_EQ(history.gpus[0].id, "card1");
    EXPECT_EQ(history.gpus[1].id, "card0");
    for (const GpuHistory& gpu : history.gpus) {
        EXPECT_EQ(gpu.percent.size(), history.timestamps.size());
        EXPECT_EQ(gpu.temperature.size(), history.timestamps.size());
    }
    EXPECT_EQ(history.gpus[0].percent.back(), 80.0);
    EXPECT_EQ(history.gpus[0].temperature.back(), 45.0);
    EXPECT_EQ(history.gpus[1].percent.back(), 20.0);
    EXPECT_EQ(history.gpus[1].temperature.back(), 0.0);
}

TEST_F(MonitorTest, RememberedPreferences)
{
    TemporaryDirectory directory;
    const std::string file = directory.path() + "/cutefish/system-monitor.conf";
    ASSERT_TRUE(writeFile(file, "[WindowSize]\nMain=@Size(840 600)\n"));
    {
        Monitor monitor({}, file);
        EXPECT_EQ(monitor.interval(), 1000);
        EXPECT_EQ(monitor.scope(), 1);
        EXPECT_FALSE(monitor.processes().allUsers());
        EXPECT_EQ(monitor.processes().sortColumn(), "cpu");
        EXPECT_FALSE(monitor.processes().ascending());
        monitor.setScope(2);
        monitor.setInterval(5000);
        monitor.sortBy("memory");
        monitor.sortBy("memory");
    }

    Monitor monitor({}, file);
    EXPECT_EQ(monitor.interval(), 5000);
    EXPECT_EQ(monitor.scope(), 2);
    EXPECT_TRUE(monitor.processes().allUsers());
    for (ProcessList* list : {&monitor.processes(), &monitor.applications()}) {
        EXPECT_EQ(list->sortColumn(), "memory");
        EXPECT_TRUE(list->ascending());
    }
    // The window's own keys stay.
    EXPECT_NE(readFile(file).find("Main=@Size(840 600)"), std::string::npos);
}

TEST(FormatTest, NumbersAsTheLocaleWritesThem)
{
    EXPECT_EQ(Monitor::formatBytes(0), "0 B");
    EXPECT_EQ(Monitor::formatBytes(1023), "1,023 B");
    EXPECT_EQ(Monitor::formatBytes(1024), "1.0 KiB");
    EXPECT_EQ(Monitor::formatBytes(1024 * 1023.95), "1,024.0 KiB");
    EXPECT_EQ(Monitor::formatRate(3.5 * 1024 * 1024), "3.5 MiB/s");
    // Halfway values round away from zero, as the binary value is exactly half.
    EXPECT_EQ(Monitor::formatPercent(12.25), "12.3%");
    EXPECT_EQ(Monitor::formatPercent(0.15), "0.1%");
    EXPECT_EQ(Monitor::formatPercent(99.96), "100.0%");
    EXPECT_EQ(Monitor::formatTemperature(41.5), "42 °C");
    EXPECT_EQ(Monitor::formatPower(14.52), "14.5 W");
    EXPECT_EQ(Monitor::formatBytes(1023.5), "1,024 B");
}

TEST(FormatTest, Durations)
{
    EXPECT_EQ(Monitor::formatDuration(-5), "0 s");
    EXPECT_EQ(Monitor::formatDuration(59), "59 s");
    EXPECT_EQ(Monitor::formatDuration(60), "1 min");
    EXPECT_EQ(Monitor::formatDuration(3599), "59 min");
    EXPECT_EQ(Monitor::formatDuration(3600 + 5 * 60 + 30), "1 h 5 min");
    EXPECT_EQ(Monitor::formatDuration(86400 - 1), "23 h 59 min");
    EXPECT_EQ(Monitor::formatDuration(3 * 86400 + 4 * 3600 + 59), "3 d 4 h");
}
