// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "testsupport.h"
#include "procreader.h"

TEST(GpuTest, GpuAccounting)
{
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 1 0 1 98 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/meminfo", "MemTotal: 1000 kB\n"));
    {
        ProcReader reader(proc, sys);
        EXPECT_FALSE(reader.sample().metrics.gpuAvailable);
    }

    const std::string first = sys + "/class/drm/card0/device";
    ASSERT_TRUE(writeFile(first + "/gpu_busy_percent", "37\n"));
    ASSERT_TRUE(writeFile(first + "/mem_info_vram_used", "1024\n"));
    ASSERT_TRUE(writeFile(first + "/mem_info_vram_total", "4096\n"));
    ASSERT_TRUE(writeFile(first + "/product_name", "Fixture Graphics\n"));
    ASSERT_TRUE(writeFile(first + "/power/runtime_status", "active\n"));
    ASSERT_TRUE(writeFile(first + "/hwmon/hwmon4/temp1_input", "41500\n"));
    ASSERT_TRUE(writeFile(first + "/hwmon/hwmon4/freq1_input", "800000000\n"));
    fs::create_directories(sys + "/bus/pci/drivers/fixturegpu");
    fs::create_symlink(sys + "/bus/pci/drivers/fixturegpu", first + "/driver");
    const std::string second = sys + "/class/drm/card1/device";
    ASSERT_TRUE(writeFile(second + "/boot_vga", "1\n"));
    ASSERT_TRUE(writeFile(second + "/power/runtime_status", "suspended\n"));
    ASSERT_TRUE(writeFile(second + "/hwmon/hwmon5/temp1_input", "60000\n"));
    fs::create_directories(sys + "/class/drm/card0-eDP-1/device/hwmon/hwmon6");
    ASSERT_TRUE(writeFile(sys + "/class/drm/card0-eDP-1/device/gpu_busy_percent", "99\n"));
    ASSERT_TRUE(writeFile(sys + "/class/drm/card2/device/vendor", "0x1234\n"));

    ProcReader reader(proc, sys);
    const auto metrics = reader.sample().metrics;
    ASSERT_EQ(metrics.gpus.size(), 2u);

    // The boot display GPU is listed first; it is suspended and is not read.
    const Gpu& suspended = metrics.gpus[0];
    EXPECT_FALSE(suspended.active);
    EXPECT_TRUE(suspended.hasTemperature);
    EXPECT_EQ(suspended.temperature, -1.0);
    EXPECT_FALSE(suspended.hasBusy);

    const Gpu& active = metrics.gpus[1];
    EXPECT_EQ(active.name, "Fixture Graphics");
    EXPECT_EQ(active.driver, "fixturegpu");
    EXPECT_EQ(active.busy, 37);
    EXPECT_EQ(active.temperature, 41.5);
    EXPECT_EQ(active.frequency, 800.0);
    EXPECT_EQ(active.vramUsed, 1024u);
    EXPECT_EQ(active.vramTotal, 4096u);

    // The overview prefers a GPU with an activity counter.
    EXPECT_TRUE(metrics.gpuAvailable);
    EXPECT_TRUE(metrics.gpuHasBusy);
    EXPECT_EQ(metrics.gpuName, "Fixture Graphics");
    EXPECT_EQ(metrics.gpuPercent, 37.0);
    EXPECT_EQ(metrics.gpuTemperature, 41.5);
}

TEST(GpuTest, DrmClientAccounting)
{
    const DrmClient render = GpuReader::parseDrmFdinfo("pos:\t0\ndrm-driver:\ti915\ndrm-client-id:\t12\n"
                                                       "drm-engine-render:\t1000 ns\ndrm-engine-video:\t400 ns\n"
                                                       "drm-engine-capacity-video:\t2\n");
    EXPECT_EQ(render.id, "12");
    EXPECT_EQ(render.engines.size(), 2u);
    EXPECT_EQ(render.engines.at("render").busyTime, 1000u);
    EXPECT_EQ(render.engines.at("video").capacity, 2);
    const DrmClient cycles = GpuReader::parseDrmFdinfo("drm-client-id:\t3\ndrm-cycles-rcs:\t50\n"
                                                       "drm-total-cycles-rcs:\t200\n");
    EXPECT_TRUE(cycles.engines.at("rcs").hasCycles);
    EXPECT_EQ(cycles.engines.at("rcs").totalCycles, 200u);
    EXPECT_TRUE(GpuReader::parseDrmFdinfo("pos:\t0\nflags:\t02100002\n").engines.empty());

    // Clients add up per engine; the busiest engine, over its capacity, is the activity.
    auto client = [](const std::string& id, uint64_t render, uint64_t video) {
        DrmClient result;
        result.id = id;
        result.engines["render"] = {render, 0, 0, 1, true, false};
        result.engines["video"] = {video, 0, 0, 2, true, false};
        return result;
    };
    const DrmClients before{{"1", client("1", 0, 0)}, {"2", client("2", 0, 0)}};
    const DrmClients after{{"1", client("1", 200, 900)}, {"2", client("2", 100, 900)}, {"3", client("3", 1000, 0)}};
    EXPECT_EQ(GpuReader::drmBusy(after, before, 1000), 90.0);
    EXPECT_EQ(GpuReader::drmBusy(after, {}, 1000), 0.0);
    EXPECT_EQ(GpuReader::drmBusy({{"1", client("1", 5000, 0)}}, before, 1000), 100.0);

    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    ASSERT_TRUE(writeFile(proc + "/stat", "cpu 1 0 1 98 0 0 0 0\n"));
    ASSERT_TRUE(writeFile(proc + "/meminfo", "MemTotal: 1000 kB\n"));
    ASSERT_TRUE(writeFile(proc + "/200/stat", processStat(200, "client", 0, 1000)));
    fs::create_directories(proc + "/200/fd");
    fs::create_symlink("/dev/dri/renderD130", proc + "/200/fd/7");
    fs::create_symlink("/dev/dri/renderD130", proc + "/200/fd/8");
    fs::create_symlink("/dev/null", proc + "/200/fd/9");
    const std::string device = sys + "/class/drm/card3/device";
    fs::create_directories(device + "/drm/card3");
    fs::create_directories(device + "/drm/renderD130");
    ASSERT_TRUE(writeFile(device + "/product_name", "Client Graphics\n"));

    // A card without a counter or a temperature stays hidden until a client reports busyness.
    ASSERT_TRUE(writeFile(proc + "/200/fdinfo/7", "pos:\t0\n"));
    ASSERT_TRUE(writeFile(proc + "/200/fdinfo/8", "pos:\t0\n"));
    ProcReader reader(proc, sys);
    EXPECT_FALSE(reader.sample().metrics.gpuAvailable);

    // A descriptor duplicated by the client counts once.
    const std::string firstInfo = "drm-client-id:\t5\ndrm-cycles-rcs:\t100\ndrm-total-cycles-rcs:\t1000\n";
    ASSERT_TRUE(writeFile(proc + "/200/fdinfo/7", firstInfo));
    ASSERT_TRUE(writeFile(proc + "/200/fdinfo/8", firstInfo));
    auto metrics = reader.sample().metrics;
    EXPECT_TRUE(metrics.gpuHasBusy);
    EXPECT_EQ(metrics.gpuName, "Client Graphics");
    EXPECT_EQ(metrics.gpuPercent, 0.0);

    const std::string secondInfo = "drm-client-id:\t5\ndrm-cycles-rcs:\t400\ndrm-total-cycles-rcs:\t2000\n";
    ASSERT_TRUE(writeFile(proc + "/200/fdinfo/7", secondInfo));
    ASSERT_TRUE(writeFile(proc + "/200/fdinfo/8", secondInfo));
    metrics = reader.sample().metrics;
    EXPECT_EQ(metrics.gpuPercent, 30.0);

    // A descriptor a running process opens later is found within five samples.
    fs::create_symlink("/dev/dri/renderD130", proc + "/200/fd/10");
    double busy = 0;
    for (int sample = 1; sample <= 7 && busy == 0; ++sample) {
        ASSERT_TRUE(writeFile(proc + "/200/fdinfo/10", "drm-client-id:\t6\ndrm-cycles-rcs:\t" + std::to_string(sample * 500)
                                                           + "\ndrm-total-cycles-rcs:\t" + std::to_string(sample * 1000) + "\n"));
        busy = reader.sample().metrics.gpuPercent;
    }
    EXPECT_EQ(busy, 50.0);

    // A suspended card's clients are not read.
    ASSERT_TRUE(writeFile(device + "/power/runtime_status", "suspended\n"));
    metrics = reader.sample().metrics;
    EXPECT_TRUE(metrics.gpuHasBusy);
    EXPECT_EQ(metrics.gpuPercent, 0.0);
}
