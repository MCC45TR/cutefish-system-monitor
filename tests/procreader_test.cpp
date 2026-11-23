// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "testsupport.h"
#include "procreader.h"

#include <algorithm>

TEST(ProcReaderTest, CpuAccounting)
{
    const auto cpu = ProcReader::parseCpu("cpu 100 20 30 800 40 5 3 2 50 10");
    EXPECT_EQ(cpu.total, 1000u);
    EXPECT_EQ(cpu.idle, 840u);
    EXPECT_EQ(ProcReader::cpuUsage({2000, 1700}, {1000, 850}), 15.0);
    EXPECT_EQ(ProcReader::cpuUsage({1000, 800}, {1000, 800}), 0.0);
    EXPECT_EQ(ProcReader::cpuUsage({10, 5}, {1000, 800}), 0.0);
    EXPECT_EQ(ProcReader::cpuUsage({1100, 790}, {1000, 800}), 100.0);
}

TEST(ProcReaderTest, MemoryAccounting)
{
    const auto memory = ProcReader::parseMemory("MemTotal: 1000 kB\nMemAvailable: 600 kB\n"
                                                "MemFree: 100 kB\nCached: 200 kB\nSReclaimable: 50 kB\n"
                                                "SwapTotal: 500 kB\nSwapFree: 300 kB\n");
    EXPECT_EQ(memory.used, 400u * 1024);
    EXPECT_EQ(memory.percent, 40.0);
    EXPECT_EQ(memory.swapUsed, 200u * 1024);
    const auto fallback = ProcReader::parseMemory("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\n"
                                                  "Cached: 200 kB\nSReclaimable: 50 kB\nShmem: 30 kB\n");
    EXPECT_EQ(fallback.available, 370u * 1024);
    EXPECT_EQ(ProcReader::parseMemory({}).percent, 0.0);
    EXPECT_EQ(ProcReader::parseMemory("MemTotal: 100 kB\nMemAvailable: 200 kB\n").used, 0u);
}

TEST(ProcReaderTest, ProcessNamesAndFields)
{
    ProcessRecord process;
    ASSERT_TRUE(ProcReader::parseProcess(processStat(123, "worker (pool) 3", 25, 999), &process, 4096));
    EXPECT_EQ(process.pid, 123);
    EXPECT_EQ(process.parentPid, 42);
    EXPECT_EQ(process.name, "worker (pool) 3");
    EXPECT_EQ(process.cpuTicks, 25u);
    EXPECT_EQ(process.startTicks, 999u);
    EXPECT_EQ(process.rss, 7u * 4096);
    EXPECT_EQ(process.memory, process.rss);
    EXPECT_EQ(process.threads, 3);
    EXPECT_EQ(process.nice, 5);
    EXPECT_FALSE(process.kernelThread);
    ASSERT_TRUE(ProcReader::parseProcess(processStat(2, "kthreadd", 0, 1, 0x00208040), &process, 4096));
    EXPECT_TRUE(process.kernelThread);
    EXPECT_FALSE(ProcReader::parseProcess("123 (short) R 1", &process, 4096));
    EXPECT_FALSE(ProcReader::parseProcess("broken", &process, 4096));

    uint32_t uid = 0;
    EXPECT_TRUE(ProcReader::parseUid("Name:\tagent\nUmask:\t0077\nUid:\t1000\t1001\t1000\t1001\n", &uid));
    EXPECT_EQ(uid, 1001u);
    EXPECT_FALSE(ProcReader::parseUid("Name:\tagent\n", &uid));

    EXPECT_EQ(ProcReader::applicationProgram(commandLine({"/usr/lib/chromium/chromium", "--type=renderer"})),
              "/usr/lib/chromium/chromium");
    EXPECT_EQ(ProcReader::applicationProgram(commandLine({"/usr/bin/python3", "-u", "/usr/bin/fixture-app", "--flag"})),
              "/usr/bin/fixture-app");
    EXPECT_EQ(ProcReader::applicationProgram(commandLine({"sh", "-c", "fixture-app"})), "");

    const std::string browser = "/usr/lib/chromium/chromium";
    const std::string title = commandLine({"/usr/lib/chromium/chromium --type=renderer --lang=en-US"});
    EXPECT_EQ(ProcReader::commandArguments(title, browser),
              commandLine({"/usr/lib/chromium/chromium", "--type=renderer", "--lang=en-US"}));
    EXPECT_EQ(ProcReader::commandArguments(title, "/usr/bin/other"), title);
    const std::string separate = commandLine({"/usr/lib/chromium/chromium", "--user-data-dir=/tmp/a b"});
    EXPECT_EQ(ProcReader::commandArguments(separate, browser), separate);
    EXPECT_EQ(ProcReader::helperType(ProcReader::commandArguments(title, browser)), "renderer");

    EXPECT_EQ(ProcReader::helperType(commandLine({"/usr/lib/chromium/chromium"})), "");
    EXPECT_EQ(ProcReader::helperType(commandLine({"/usr/lib/chromium/chromium", "--type=gpu-process", "--lang=en-US"})),
              "gpu-process");
    EXPECT_EQ(ProcReader::helperType(commandLine({"/usr/lib/chromium/chromium", "--type=renderer", "--extension-process"})),
              "extension");
    EXPECT_EQ(ProcReader::helperType(commandLine({"/usr/lib/chromium/chromium", "--type=utility",
                                                  "--utility-sub-type=network.mojom.NetworkService"})),
              "network.mojom.NetworkService");
    EXPECT_EQ(ProcReader::helperType(commandLine({"/usr/bin/fixture-app", "--verbose", "--type=renderer"})), "");
    EXPECT_EQ(ProcReader::helperName("gpu-process"), "GPU");
    EXPECT_EQ(ProcReader::helperName("network.mojom.NetworkService"), "Network");
    EXPECT_EQ(ProcReader::helperName("unzip.mojom.Unzipper"), "Utility");
    EXPECT_EQ(ProcReader::helperName("broker"), "broker");

    ProcessRecord helper = record(11, "fixture");
    helper.program = "/opt/fixture/fixture-app";
    helper.helperType = "renderer";
    EXPECT_EQ(ProcessList::programName(helper), "fixture-app (Renderer)");
}

TEST(ProcReaderTest, SamplingAndPidReuse)
{
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 100 0 50 850 0 0 0 0\ncpu0 100 0 50 850 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n"));
    ASSERT_TRUE(writeFile(proc + "/123/stat", processStat(123, "sample", 20, 1000)));
    fs::create_symlink("/usr/bin/fixture-tool (deleted)", proc + "/123/exe");
    ProcReader reader(proc, sys);
    const auto first = reader.sample();
    ASSERT_TRUE(first.available);
    EXPECT_TRUE(first.baseline);
    ASSERT_EQ(first.processes.size(), 1u);
    EXPECT_EQ(first.processes[0].uid, getuid());
    EXPECT_FALSE(first.processes[0].kernelThread);
    EXPECT_EQ(first.processes[0].cpu, 0.0);
    EXPECT_EQ(first.processes[0].displayName, "fixture-tool");
    EXPECT_FALSE(first.processes[0].ioAvailable);

    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 200 0 100 1700 0 0 0 0\ncpu0 200 0 100 1700 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/123/stat", processStat(123, "sample", 70, 1000)));
    const auto second = reader.sample();
    EXPECT_FALSE(second.baseline);
    EXPECT_EQ(second.metrics.cpuPercent, 15.0);
    EXPECT_EQ(second.processes[0].cpu, 5.0);

    // An exec keeps the PID and start time; the new name reads the new program.
    fs::remove(proc + "/123/exe");
    fs::create_symlink("/usr/bin/other-tool", proc + "/123/exe");
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 250 0 125 2125 0 0 0 0\ncpu0 250 0 125 2125 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/123/stat", processStat(123, "other-tool", 95, 1000)));
    const auto execed = reader.sample();
    EXPECT_EQ(execed.processes[0].displayName, "other-tool");
    EXPECT_EQ(execed.processes[0].cpu, 5.0);

    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 300 0 150 2550 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/123/stat", processStat(123, "replacement", 9999, 2000)));
    const auto reused = reader.sample();
    EXPECT_EQ(reused.processes[0].cpu, 0.0);
    EXPECT_EQ(reused.processes[0].name, "replacement");
}

TEST(ProcReaderTest, OfflineProcessors)
{
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    ASSERT_TRUE(writeFile(proc + "/meminfo", "MemTotal: 1000 kB\n"));
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 0 0 0 200 0 0 0 0\ncpu0 0 0 0 100 0 0 0 0\n"
                                          "cpu2 0 0 0 100 0 0 0 0\n"));
    ProcReader reader(proc, directory.path() + "/sys");
    reader.sample();
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 100 0 0 300 0 0 0 0\ncpu0 0 0 0 200 0 0 0 0\n"
                                          "cpu2 100 0 0 100 0 0 0 0\n"));
    const auto sample = reader.sample();
    ASSERT_EQ(sample.metrics.cores.size(), 2u);
    EXPECT_EQ(sample.metrics.cores[0].number, 0);
    EXPECT_EQ(sample.metrics.cores[0].usage, 0.0);
    EXPECT_EQ(sample.metrics.cores[1].number, 2);
    EXPECT_EQ(sample.metrics.cores[1].usage, 100.0);
}

TEST(ProcReaderTest, DeviceAccounting)
{
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 1 0 1 98 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/meminfo", "MemTotal: 1000 kB\n"));
    fs::create_directories(sys + "/class/net/test0/device");
    fs::create_directories(sys + "/block/sda/device");
    const std::string network = "test0: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n"
                                "lo: 5000 0 0 0 0 0 0 0 5000 0 0 0 0 0 0 0\n"
                                "veth0: 9000 0 0 0 0 0 0 0 9000 0 0 0 0 0 0 0\n";
    ASSERT_TRUE(writeFile(proc + "/net/dev", network));
    ASSERT_TRUE(writeFile(proc + "/diskstats", "8 0 sda 0 0 100 0 0 0 200 0 0 0 0\n8 1 sda1 0 0 100 0 0 0 200 0 0 0 0\n"));
    ProcReader reader(proc, sys);
    const auto first = reader.sample();
    EXPECT_EQ(first.metrics.received, 1000u);
    EXPECT_EQ(first.metrics.interfaces.size(), 2u);
    EXPECT_EQ(first.metrics.disks.size(), 1u);
    EXPECT_EQ(first.metrics.diskRead, 0.0);
    usleep(20000);
    ASSERT_TRUE(writeFile(proc + "/net/dev", "test0: 3000 0 0 0 0 0 0 0 4000 0 0 0 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/diskstats", "8 0 sda 0 0 200 0 0 0 400 0 0 0 0\n"));
    const auto second = reader.sample();
    EXPECT_GT(second.metrics.download, 0);
    EXPECT_GT(second.metrics.diskRead, 0);
    ASSERT_TRUE(writeFile(proc + "/net/dev", "test0: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"));
    EXPECT_EQ(reader.sample().metrics.download, 0.0);
}

TEST(ProcReaderTest, UnavailableProc)
{
    TemporaryDirectory directory;
    ProcReader reader(directory.path(), directory.path());
    const auto snapshot = reader.sample();
    EXPECT_FALSE(snapshot.available);
    EXPECT_FALSE(snapshot.error.empty());
}

TEST(ProcReaderTest, Volumes)
{
    EXPECT_EQ(DiskReader::unescapePath("/media/My\\040Disk"), "/media/My Disk");
    const auto volumes = DiskReader::volumes();
    ASSERT_FALSE(volumes.empty());
    EXPECT_TRUE(std::any_of(volumes.begin(), volumes.end(), [](const Volume& volume) { return volume.mount == "/"; }));
    for (const Volume& volume : volumes) {
        EXPECT_NE(volume.type, "tmpfs");
        EXPECT_FALSE(volume.mount.starts_with("/proc") || volume.mount.starts_with("/sys")
                     || volume.mount.starts_with("/dev/"));
        EXPECT_GT(volume.total, 0u);
        EXPECT_LE(volume.used, volume.total);
    }
}

TEST(ProcReaderTest, ScopesAndPss)
{
    EXPECT_EQ(ProcReader::scopeApplication("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                                           "app-cutefish-cutefish\\x2dterminal-2091752.scope\n"),
              "cutefish-terminal");
    EXPECT_EQ(ProcReader::scopeApplication("0::/user.slice/app.slice/app-org.example.Viewer-12.scope"),
              "org.example.Viewer");
    EXPECT_EQ(ProcReader::scopeApplication("0::/app.slice/app-gnome-org.example.Editor@3a9f.service"),
              "org.example.Editor");
    EXPECT_EQ(ProcReader::scopeApplication("0::/app.slice/app-org.example.Player.service"), "org.example.Player");
    EXPECT_EQ(ProcReader::scopeApplication("0::/user.slice/user-1000.slice/session-580.scope"), "");
    EXPECT_EQ(ProcReader::scopeApplication("0::/app.slice/xdg-desktop-portal-cutefish.service"), "");
    EXPECT_EQ(ProcReader::scopeApplication("0::/app.slice/app-broken.scope"), "");
    EXPECT_EQ(ProcReader::scopeApplication({}), "");

    uint64_t pss = 0;
    EXPECT_TRUE(ProcReader::parsePss("55d0-7ffd ---p 00000000 00:00 0 [rollup]\nRss: 161924 kB\n"
                                     "Pss: 99783 kB\nPss_Dirty: 42360 kB\n",
                                     &pss));
    EXPECT_EQ(pss, 99783u * 1024);
    EXPECT_FALSE(ProcReader::parsePss("Rss: 10 kB\n", &pss));
}

TEST(ProcReaderTest, SampledScopesAndPss)
{
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 100 0 50 850 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n"));
    ASSERT_TRUE(writeFile(proc + "/100/stat", processStat(100, "viewer", 0, 1000)));
    ASSERT_TRUE(writeFile(proc + "/100/cgroup", "0::/user.slice/user-1000.slice/session-2.scope\n"));
    ASSERT_TRUE(writeFile(proc + "/100/smaps_rollup", "Pss: 300 kB\n"));
    ASSERT_TRUE(writeFile(proc + "/200/stat", processStat(200, "tool", 0, 1000)));
    ASSERT_TRUE(writeFile(proc + "/200/smaps_rollup", "Pss: 50 kB\n"));
    ASSERT_TRUE(writeFile(proc + "/400/stat", processStat(400, "locked", 0, 1000)));
    ProcReader reader(proc, directory.path() + "/sys");
    auto snapshot = reader.sample();
    ASSERT_EQ(snapshot.processes.size(), 3u);
    EXPECT_TRUE(snapshot.processes[0].scopeId.empty());

    // Every process of the user is measured; an unreadable one keeps its resident memory.
    EXPECT_TRUE(snapshot.processes[0].pssAvailable);
    EXPECT_EQ(snapshot.processes[0].memory, 300u * 1024);
    EXPECT_TRUE(snapshot.processes[1].pssAvailable);
    EXPECT_EQ(snapshot.processes[1].memory, 50u * 1024);
    EXPECT_FALSE(snapshot.processes[2].pssAvailable);
    EXPECT_GT(snapshot.processes[2].rss, 0u);
    EXPECT_EQ(snapshot.processes[2].memory, snapshot.processes[2].rss);

    // The launcher adopts the process into its scope after the first read.
    ASSERT_TRUE(writeFile(proc + "/100/cgroup", "0::/user.slice/app.slice/app-cutefish-org.example.Viewer-100.scope\n"));
    snapshot = reader.sample();
    EXPECT_EQ(snapshot.processes[0].scopeId, "org.example.Viewer");

    // Settled processes keep their scope and, for a few seconds, their PSS;
    // new processes are measured at once.
    ASSERT_TRUE(writeFile(proc + "/100/cgroup", "0::/elsewhere.scope\n"));
    ASSERT_TRUE(writeFile(proc + "/100/smaps_rollup", "Pss: 400 kB\n"));
    std::string child = processStat(300, "helper", 0, 1100);
    child.replace(child.find(" S 42 "), 6, " S 200 ");
    ASSERT_TRUE(writeFile(proc + "/300/stat", child));
    ASSERT_TRUE(writeFile(proc + "/300/smaps_rollup", "Pss: 20 kB\n"));
    snapshot = reader.sample();
    ASSERT_EQ(snapshot.processes.size(), 4u);
    EXPECT_EQ(snapshot.processes[0].scopeId, "org.example.Viewer");
    EXPECT_EQ(snapshot.processes[0].memory, 300u * 1024);
    EXPECT_EQ(snapshot.processes[1].memory, 50u * 1024);
    EXPECT_EQ(snapshot.processes[2].parentPid, 200);
    EXPECT_EQ(snapshot.processes[2].memory, 20u * 1024);
}

TEST(ProcReaderTest, DetailsExtras)
{
    EXPECT_EQ(ProcReader::cgroupUnit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                                     "app-cutefish-cutefish\\x2dterminal-2091752.scope\n"),
              "app-cutefish-cutefish\\x2dterminal-2091752.scope");
    EXPECT_EQ(ProcReader::cgroupUnit("12:cpu:/legacy\n0::/init.scope\n"), "init.scope");
    EXPECT_EQ(ProcReader::cgroupUnit("0::/\n"), "");
    EXPECT_EQ(ProcReader::cgroupUnit({}), "");
    EXPECT_EQ(ProcReader::bootTime("cpu 1 2 3 4\nintr 5\nbtime 1763500000\nprocesses 9\n"), 1763500000);
    EXPECT_EQ(ProcReader::bootTime("cpu 1 2 3 4\n"), 0);
    EXPECT_EQ(ProcReader::bootTime("btime soon\n"), 0);

    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    ASSERT_TRUE(writeFile(proc + "/100/stat", processStat(100, "viewer", 0, 1000)));
    ASSERT_TRUE(writeFile(proc + "/100/cgroup", "0::/user.slice/app.slice/app-org.example.Viewer-100.scope\n"));
    for (const char* descriptor : {"0", "1", "2", "7"})
        ASSERT_TRUE(writeFile(proc + "/100/fd/" + descriptor, {}));
    ProcessExtras extras = ProcReader::readExtras(proc, 100, 1000);
    EXPECT_EQ(extras.openFiles, 4);
    EXPECT_EQ(extras.unit, "app-org.example.Viewer-100.scope");

    // Another user's descriptors cannot be listed; a reused PID tells nothing.
    fs::remove_all(proc + "/100/fd");
    extras = ProcReader::readExtras(proc, 100, 1000);
    EXPECT_EQ(extras.openFiles, -1);
    EXPECT_EQ(extras.unit, "app-org.example.Viewer-100.scope");
    extras = ProcReader::readExtras(proc, 100, 2000);
    EXPECT_EQ(extras.openFiles, -1);
    EXPECT_EQ(extras.unit, "");

    // The test's own descriptors, through the real files.
    ProcessRecord self;
    ASSERT_TRUE(ProcReader::parseProcess(readFile("/proc/self/stat"), &self, 1));
    extras = ProcReader::readExtras("/proc", getpid(), self.startTicks);
    EXPECT_GE(extras.openFiles, 3);
    EXPECT_GT(ProcReader::bootTime(readFile("/proc/stat")), 0);
}
