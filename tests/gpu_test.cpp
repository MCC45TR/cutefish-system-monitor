// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "testsupport.h"
#include "nvmlreader.h"
#include "procreader.h"

#include <cstdio>
#include <cstring>

// The NVIDIA library's stand-in: what it reports, and how often it was asked.
struct FakeNvml
{
    struct Utilization
    {
        unsigned int gpu;
        unsigned int memory;
    };
    struct Memory
    {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    };

    static inline int loads = 0;
    static inline int inits = 0;
    static inline int calls = 0;
    static inline std::string busId;
    static inline bool initFails = false;
    static inline bool knowsCard = true;
    static inline bool hasPower = true;

    static void reset()
    {
        loads = inits = calls = 0;
        busId.clear();
        initFails = false;
        knowsCard = true;
        hasPower = true;
    }

    static int init()
    {
        ++inits;
        return initFails ? 1 : 0;
    }
    static int shutdown() { return 0; }
    static int handle(const char* id, void** device)
    {
        ++calls;
        busId = id;
        if (!knowsCard)
            return 6;
        *device = &calls;
        return 0;
    }
    static int name(void*, char* name, unsigned int length)
    {
        ++calls;
        std::snprintf(name, length, "NVIDIA GeForce RTX 4050 Laptop GPU");
        return 0;
    }
    static int utilization(void*, Utilization* utilization)
    {
        ++calls;
        *utilization = {63, 20};
        return 0;
    }
    static int encoder(void*, unsigned int* value, unsigned int* period)
    {
        ++calls;
        *value = 12;
        *period = 167000;
        return 0;
    }
    static int decoder(void*, unsigned int* value, unsigned int* period)
    {
        ++calls;
        *value = 340;
        *period = 167000;
        return 0;
    }
    static int memory(void*, Memory* memory)
    {
        ++calls;
        *memory = {6ull << 30, 5ull << 30, 1ull << 30};
        return 0;
    }
    static int temperature(void*, int sensor, unsigned int* value)
    {
        ++calls;
        if (sensor != 0)
            return 2;
        *value = 55;
        return 0;
    }
    static int clock(void*, int type, unsigned int* value)
    {
        ++calls;
        if (type == 0)
            *value = 1755;
        else if (type == 2)
            *value = 8001;
        else
            return 2;
        return 0;
    }
    static int power(void*, unsigned int* milliwatts)
    {
        ++calls;
        *milliwatts = 14520;
        return 0;
    }

    static NvmlLoader loader()
    {
        return [] {
            ++loads;
            return NvmlSymbols([](const char* symbol) -> void* {
                const std::string name = symbol;
                if (name == "nvmlInit_v2")
                    return reinterpret_cast<void*>(&init);
                if (name == "nvmlShutdown")
                    return reinterpret_cast<void*>(&shutdown);
                if (name == "nvmlDeviceGetHandleByPciBusId_v2")
                    return reinterpret_cast<void*>(&handle);
                if (name == "nvmlDeviceGetName")
                    return reinterpret_cast<void*>(&FakeNvml::name);
                if (name == "nvmlDeviceGetUtilizationRates")
                    return reinterpret_cast<void*>(&utilization);
                if (name == "nvmlDeviceGetEncoderUtilization")
                    return reinterpret_cast<void*>(&encoder);
                if (name == "nvmlDeviceGetDecoderUtilization")
                    return reinterpret_cast<void*>(&decoder);
                if (name == "nvmlDeviceGetMemoryInfo")
                    return reinterpret_cast<void*>(&FakeNvml::memory);
                if (name == "nvmlDeviceGetTemperature")
                    return reinterpret_cast<void*>(&temperature);
                if (name == "nvmlDeviceGetClockInfo")
                    return reinterpret_cast<void*>(&clock);
                if (name == "nvmlDeviceGetPowerUsage" && hasPower)
                    return reinterpret_cast<void*>(&power);
                return nullptr;
            });
        };
    }
};

// A card the NVIDIA driver runs, with its DRM nodes and no counters of its own in sysfs.
static void addNvidiaCard(const std::string& sys, const std::string& status)
{
    const std::string device = sys + "/class/drm/card1/device";
    fs::create_directories(device + "/drm/card1");
    fs::create_directories(device + "/drm/renderD129");
    ASSERT_TRUE(writeFile(device + "/uevent", "DRIVER=nvidia\nPCI_CLASS=30000\nPCI_SLOT_NAME=0000:01:00.0\n"));
    ASSERT_TRUE(writeFile(device + "/power/runtime_status", status));
    fs::create_directories(sys + "/bus/pci/drivers/nvidia");
    fs::create_symlink(sys + "/bus/pci/drivers/nvidia", device + "/driver");
}

static Metrics sampleGpus(GpuReader& reader)
{
    Metrics metrics;
    reader.sample(&metrics, {}, 1);
    return metrics;
}

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

TEST(GpuTest, NvidiaThroughLibrary)
{
    FakeNvml::reset();
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    fs::create_directories(proc);
    addNvidiaCard(sys, "suspended\n");
    GpuReader reader(proc, sys, FakeNvml::loader());

    // A card suspended from the start is listed powered off, and the library is not even opened.
    Metrics metrics = sampleGpus(reader);
    ASSERT_EQ(metrics.gpus.size(), 1u);
    EXPECT_FALSE(metrics.gpus[0].active);
    EXPECT_TRUE(metrics.gpus[0].hasBusy);
    EXPECT_EQ(metrics.gpus[0].driver, "nvidia");
    EXPECT_EQ(FakeNvml::loads, 0);

    // Once it is awake, everything comes from the library, converted to the monitor's units.
    ASSERT_TRUE(writeFile(sys + "/class/drm/card1/device/power/runtime_status", "active\n"));
    metrics = sampleGpus(reader);
    EXPECT_EQ(FakeNvml::loads, 1);
    EXPECT_EQ(FakeNvml::inits, 1);
    EXPECT_EQ(FakeNvml::busId, "0000:01:00.0");
    ASSERT_EQ(metrics.gpus.size(), 1u);
    Gpu gpu = metrics.gpus[0];
    EXPECT_EQ(gpu.id, "card1");
    EXPECT_EQ(gpu.name, "NVIDIA GeForce RTX 4050 Laptop GPU");
    EXPECT_TRUE(gpu.active);
    EXPECT_TRUE(gpu.hasBusy);
    EXPECT_EQ(gpu.busy, 63);
    EXPECT_TRUE(gpu.hasEncoder);
    EXPECT_EQ(gpu.encoder, 12);
    EXPECT_TRUE(gpu.hasDecoder);
    EXPECT_EQ(gpu.decoder, 100);
    EXPECT_TRUE(gpu.hasVram);
    EXPECT_EQ(gpu.vramUsed, 1ull << 30);
    EXPECT_EQ(gpu.vramTotal, 6ull << 30);
    EXPECT_TRUE(gpu.hasTemperature);
    EXPECT_EQ(gpu.temperature, 55.0);
    EXPECT_TRUE(gpu.hasFrequency);
    EXPECT_EQ(gpu.frequency, 1755.0);
    EXPECT_TRUE(gpu.hasMemoryFrequency);
    EXPECT_EQ(gpu.memoryFrequency, 8001.0);
    EXPECT_TRUE(gpu.hasPower);
    EXPECT_DOUBLE_EQ(gpu.power, 14.52);
    EXPECT_TRUE(metrics.gpuHasBusy);
    EXPECT_EQ(metrics.gpuPercent, 63.0);

    // Suspended again, the card is not asked anything; it keeps its rows and its name.
    ASSERT_TRUE(writeFile(sys + "/class/drm/card1/device/power/runtime_status", "suspended\n"));
    const int calls = FakeNvml::calls;
    metrics = sampleGpus(reader);
    EXPECT_EQ(FakeNvml::calls, calls);
    ASSERT_EQ(metrics.gpus.size(), 1u);
    gpu = metrics.gpus[0];
    EXPECT_FALSE(gpu.active);
    EXPECT_EQ(gpu.name, "NVIDIA GeForce RTX 4050 Laptop GPU");
    EXPECT_TRUE(gpu.hasPower);
    EXPECT_EQ(gpu.busy, 0);
    EXPECT_EQ(gpu.temperature, -1.0);
    EXPECT_EQ(gpu.power, 0.0);

    ASSERT_TRUE(writeFile(sys + "/class/drm/card1/device/power/runtime_status", "active\n"));
    sampleGpus(reader);
    EXPECT_EQ(FakeNvml::loads, 1);
    EXPECT_EQ(FakeNvml::inits, 1);
}

TEST(GpuTest, NvidiaWithoutLibrary)
{
    FakeNvml::reset();
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    fs::create_directories(proc);
    addNvidiaCard(sys, "active\n");

    // Without the library the card has nothing to show, as before; it is not tried again.
    int loads = 0;
    GpuReader missing(proc, sys, [&loads] {
        ++loads;
        return NvmlSymbols();
    });
    for (int sample = 0; sample < 3; ++sample)
        EXPECT_TRUE(sampleGpus(missing).gpus.empty());
    EXPECT_EQ(loads, 1);

    // Nor when it cannot be initialised, or does not know the card.
    FakeNvml::initFails = true;
    GpuReader failing(proc, sys, FakeNvml::loader());
    EXPECT_TRUE(sampleGpus(failing).gpus.empty());
    EXPECT_TRUE(sampleGpus(failing).gpus.empty());
    EXPECT_EQ(FakeNvml::inits, 1);

    FakeNvml::reset();
    FakeNvml::knowsCard = false;
    GpuReader unknown(proc, sys, FakeNvml::loader());
    EXPECT_TRUE(sampleGpus(unknown).gpus.empty());
    const int calls = FakeNvml::calls;
    EXPECT_TRUE(sampleGpus(unknown).gpus.empty());
    EXPECT_EQ(FakeNvml::calls, calls);

    // A function the library lacks leaves only its row out.
    FakeNvml::reset();
    FakeNvml::hasPower = false;
    GpuReader partial(proc, sys, FakeNvml::loader());
    const Metrics metrics = sampleGpus(partial);
    ASSERT_EQ(metrics.gpus.size(), 1u);
    EXPECT_FALSE(metrics.gpus[0].hasPower);
    EXPECT_TRUE(metrics.gpus[0].hasBusy);

    // A library file that is not there fails once.
    NvmlReader absent(NvmlReader::library(directory.path() + "/libnvidia-ml.so.1"));
    Gpu gpu;
    EXPECT_FALSE(absent.sample("0000:01:00.0", &gpu));
    EXPECT_TRUE(absent.failed());
    EXPECT_FALSE(gpu.hasBusy);
}

TEST(GpuTest, HwmonPowerAndMemoryClock)
{
    TemporaryDirectory directory;
    const std::string proc = directory.path() + "/proc";
    const std::string sys = directory.path() + "/sys";
    fs::create_directories(proc);
    const std::string first = sys + "/class/drm/card0/device";
    ASSERT_TRUE(writeFile(first + "/boot_vga", "1\n"));
    ASSERT_TRUE(writeFile(first + "/gpu_busy_percent", "5\n"));
    ASSERT_TRUE(writeFile(first + "/hwmon/hwmon2/power1_average", "15250000\n"));
    ASSERT_TRUE(writeFile(first + "/hwmon/hwmon2/power1_input", "99000000\n"));
    ASSERT_TRUE(writeFile(first + "/hwmon/hwmon2/freq2_input", "1000000000\n"));
    const std::string second = sys + "/class/drm/card1/device";
    ASSERT_TRUE(writeFile(second + "/gpu_busy_percent", "0\n"));
    ASSERT_TRUE(writeFile(second + "/hwmon/hwmon3/power1_input", "7500000\n"));
    const std::string third = sys + "/class/drm/card2/device";
    ASSERT_TRUE(writeFile(third + "/gpu_busy_percent", "0\n"));
    ASSERT_TRUE(writeFile(third + "/hwmon/hwmon4/temp1_input", "40000\n"));
    ASSERT_TRUE(writeFile(third + "/power/runtime_status", "suspended\n"));
    ASSERT_TRUE(writeFile(third + "/hwmon/hwmon4/power1_input", "7500000\n"));

    GpuReader reader(proc, sys);
    const Metrics metrics = sampleGpus(reader);
    ASSERT_EQ(metrics.gpus.size(), 3u);

    // The average is preferred where both are there; microwatts become watts, hertz megahertz.
    const Gpu& averaged = metrics.gpus[0];
    EXPECT_EQ(averaged.id, "card0");
    EXPECT_TRUE(averaged.hasPower);
    EXPECT_DOUBLE_EQ(averaged.power, 15.25);
    EXPECT_TRUE(averaged.hasMemoryFrequency);
    EXPECT_EQ(averaged.memoryFrequency, 1000.0);
    EXPECT_FALSE(averaged.hasEncoder);
    EXPECT_FALSE(averaged.hasDecoder);

    const Gpu& instant = metrics.gpus[1];
    EXPECT_TRUE(instant.hasPower);
    EXPECT_DOUBLE_EQ(instant.power, 7.5);
    EXPECT_FALSE(instant.hasMemoryFrequency);

    const Gpu& suspended = metrics.gpus[2];
    EXPECT_FALSE(suspended.active);
    EXPECT_TRUE(suspended.hasPower);
    EXPECT_EQ(suspended.power, 0.0);

    // Without a hardware monitor, neither is shown.
    const std::string bare = directory.path() + "/bare";
    ASSERT_TRUE(writeFile(bare + "/class/drm/card0/device/gpu_busy_percent", "5\n"));
    GpuReader plain(proc, bare);
    const Metrics none = sampleGpus(plain);
    ASSERT_EQ(none.gpus.size(), 1u);
    EXPECT_FALSE(none.gpus[0].hasPower);
    EXPECT_FALSE(none.gpus[0].hasMemoryFrequency);
}
