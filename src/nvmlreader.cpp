// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "nvmlreader.h"

#include "fileutil.h"
#include "metrics.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <memory>

// The library's C interface, as its documentation declares it.
using NvmlReturn = int;
using NvmlDevice = void*;
static constexpr NvmlReturn kNvmlSuccess = 0;
static constexpr int kNvmlTemperatureGpu = 0;
static constexpr int kNvmlClockGraphics = 0;
static constexpr int kNvmlClockMemory = 2;

struct NvmlUtilization
{
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory
{
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct NvmlReader::Functions
{
    NvmlReturn (*init)() = nullptr;
    NvmlReturn (*shutdown)() = nullptr;
    NvmlReturn (*handleByPciBusId)(const char* pciBusId, NvmlDevice* device) = nullptr;
    NvmlReturn (*name)(NvmlDevice device, char* name, unsigned int length) = nullptr;
    NvmlReturn (*utilization)(NvmlDevice device, NvmlUtilization* utilization) = nullptr;
    NvmlReturn (*encoder)(NvmlDevice device, unsigned int* utilization, unsigned int* samplingPeriod) = nullptr;
    NvmlReturn (*decoder)(NvmlDevice device, unsigned int* utilization, unsigned int* samplingPeriod) = nullptr;
    NvmlReturn (*memory)(NvmlDevice device, NvmlMemory* memory) = nullptr;
    NvmlReturn (*temperature)(NvmlDevice device, int sensor, unsigned int* temperature) = nullptr;
    NvmlReturn (*clock)(NvmlDevice device, int type, unsigned int* clock) = nullptr;
    NvmlReturn (*power)(NvmlDevice device, unsigned int* milliwatts) = nullptr;
};

NvmlReader::NvmlReader(NvmlLoader loader)
    : m_loader(std::move(loader))
{
}

NvmlReader::~NvmlReader()
{
    if (m_state == State::Ready && m_functions->shutdown)
        m_functions->shutdown();
}

NvmlLoader NvmlReader::library(std::string path)
{
    return [path = std::move(path)]() -> NvmlSymbols {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            return {};
        const std::shared_ptr<void> library(handle, dlclose);
        return [library](const char* name) { return dlsym(library.get(), name); };
    };
}

NvmlLoader NvmlReader::systemLibrary()
{
    return library("libnvidia-ml.so.1");
}

template<typename Function>
static void resolve(const NvmlSymbols& symbols, const char* name, Function* function)
{
    *function = reinterpret_cast<Function>(symbols(name));
}

bool NvmlReader::load()
{
    if (m_state != State::Unloaded)
        return m_state == State::Ready;

    m_state = State::Failed;
    m_symbols = m_loader ? m_loader() : NvmlSymbols();
    if (!m_symbols)
        return false;
    m_functions = std::make_unique<Functions>();
    Functions& functions = *m_functions;
    resolve(m_symbols, "nvmlInit_v2", &functions.init);
    resolve(m_symbols, "nvmlShutdown", &functions.shutdown);
    resolve(m_symbols, "nvmlDeviceGetHandleByPciBusId_v2", &functions.handleByPciBusId);
    resolve(m_symbols, "nvmlDeviceGetName", &functions.name);
    resolve(m_symbols, "nvmlDeviceGetUtilizationRates", &functions.utilization);
    resolve(m_symbols, "nvmlDeviceGetEncoderUtilization", &functions.encoder);
    resolve(m_symbols, "nvmlDeviceGetDecoderUtilization", &functions.decoder);
    resolve(m_symbols, "nvmlDeviceGetMemoryInfo", &functions.memory);
    resolve(m_symbols, "nvmlDeviceGetTemperature", &functions.temperature);
    resolve(m_symbols, "nvmlDeviceGetClockInfo", &functions.clock);
    resolve(m_symbols, "nvmlDeviceGetPowerUsage", &functions.power);
    if (!functions.init || !functions.handleByPciBusId || functions.init() != kNvmlSuccess) {
        m_functions.reset();
        m_symbols = {};
        return false;
    }
    m_state = State::Ready;
    return true;
}

bool NvmlReader::sample(const std::string& pciSlot, Gpu* gpu)
{
    if (pciSlot.empty() || !load())
        return false;

    const Functions& functions = *m_functions;
    auto device = m_devices.find(pciSlot);
    if (device == m_devices.end()) {
        NvmlDevice handle = nullptr;
        if (functions.handleByPciBusId(pciSlot.c_str(), &handle) != kNvmlSuccess)
            handle = nullptr;
        device = m_devices.emplace(pciSlot, handle).first;
    }
    NvmlDevice handle = device->second;
    if (!handle)
        return false;

    char name[96] = {};
    if (functions.name && functions.name(handle, name, sizeof(name)) == kNvmlSuccess) {
        const std::string text = validUtf8(trimmed(std::string_view(name, strnlen(name, sizeof(name)))));
        if (!text.empty())
            gpu->name = text;
    }
    NvmlUtilization utilization = {};
    gpu->hasBusy = functions.utilization && functions.utilization(handle, &utilization) == kNvmlSuccess;
    if (gpu->hasBusy)
        gpu->busy = int(std::min(utilization.gpu, 100u));

    unsigned int value = 0;
    unsigned int period = 0;
    gpu->hasEncoder = functions.encoder && functions.encoder(handle, &value, &period) == kNvmlSuccess;
    if (gpu->hasEncoder)
        gpu->encoder = int(std::min(value, 100u));
    gpu->hasDecoder = functions.decoder && functions.decoder(handle, &value, &period) == kNvmlSuccess;
    if (gpu->hasDecoder)
        gpu->decoder = int(std::min(value, 100u));

    NvmlMemory memory = {};
    gpu->hasVram = functions.memory && functions.memory(handle, &memory) == kNvmlSuccess;
    if (gpu->hasVram) {
        gpu->vramUsed = memory.used;
        gpu->vramTotal = memory.total;
    }
    gpu->hasTemperature = functions.temperature
                          && functions.temperature(handle, kNvmlTemperatureGpu, &value) == kNvmlSuccess;
    if (gpu->hasTemperature)
        gpu->temperature = value;
    gpu->hasFrequency = functions.clock && functions.clock(handle, kNvmlClockGraphics, &value) == kNvmlSuccess;
    if (gpu->hasFrequency)
        gpu->frequency = value;
    gpu->hasMemoryFrequency = functions.clock && functions.clock(handle, kNvmlClockMemory, &value) == kNvmlSuccess;
    if (gpu->hasMemoryFrequency)
        gpu->memoryFrequency = value;
    gpu->hasPower = functions.power && functions.power(handle, &value) == kNvmlSuccess;
    if (gpu->hasPower)
        gpu->power = value / 1000.0;
    return true;
}
