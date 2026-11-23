// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MemoryMetrics
{
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t available = 0;
    uint64_t cache = 0;
    double percent = 0;
    uint64_t swapTotal = 0;
    uint64_t swapUsed = 0;
    double swapPercent = 0;
};

struct NetworkInterface
{
    std::string name;
    std::string state;
    std::string address;
    double download = 0;
    double upload = 0;
    uint64_t received = 0;
    uint64_t sent = 0;
    bool physical = false;
};

struct Disk
{
    std::string name;
    std::string model;
    double read = 0;
    double write = 0;
    uint64_t size = 0;
};

struct Volume
{
    std::string mount;
    std::string device;
    std::string type;
    uint64_t total = 0;
    uint64_t available = 0;
    uint64_t used = 0;
};

struct Gpu
{
    // The card's DRM name, such as "card1", which stays while the card is there.
    std::string id;
    std::string name;
    std::string driver;
    bool active = false;
    bool hasBusy = false;
    int busy = 0;
    bool hasTemperature = false;
    double temperature = -1;
    // Clocks in MHz.
    bool hasFrequency = false;
    double frequency = 0;
    bool hasMemoryFrequency = false;
    double memoryFrequency = 0;
    bool hasVram = false;
    uint64_t vramUsed = 0;
    uint64_t vramTotal = 0;
    bool hasEncoder = false;
    int encoder = 0;
    bool hasDecoder = false;
    int decoder = 0;
    // Watts.
    bool hasPower = false;
    double power = 0;
};

struct Processor
{
    // The kernel's number, which skips offline processors.
    int number = 0;
    double usage = 0;
};

struct Metrics
{
    MemoryMetrics memory;

    double cpuPercent = 0;
    std::vector<Processor> cores;
    std::string cpuName;
    int processCount = 0;

    double download = 0;
    double upload = 0;
    uint64_t received = 0;
    uint64_t sent = 0;
    std::vector<NetworkInterface> interfaces;

    double diskRead = 0;
    double diskWrite = 0;
    std::vector<Disk> disks;
    std::vector<Volume> volumes;

    std::vector<Gpu> gpus;
    bool gpuAvailable = false;
    std::string gpuName;
    bool gpuHasBusy = false;
    bool gpuHasTemperature = false;
    double gpuPercent = 0;
    double gpuTemperature = 0;
};
