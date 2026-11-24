// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "metrics.h"
#include "processlist.h"
#include "base/signal.h"

#include "base/thread_pool.h"
#include "base/timer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ProcReader;
struct Snapshot;

// A desktop entry as the monitor names processes after it.
struct DesktopApplication
{
    std::string id;
    std::string name;
    std::string icon;
    bool noDisplay = false;
    bool hidden = false;
};

// Finds the desktop entries processes belong to.
struct ApplicationLookup
{
    // The entry of a program, by its file name and path, as a window of it would be.
    std::function<std::optional<DesktopApplication>(const std::string& name, const std::string& program)> resolve;
    std::function<std::optional<DesktopApplication>(const std::string& id)> byId;
};

// A GPU's activity and temperature, as History keeps them.
struct GpuHistory
{
    std::string id;
    std::vector<double> percent;
    std::vector<double> temperature;
};

// The last minute of the system-wide measurements, one value per sample.
struct History
{
    // Milliseconds since the monitor started.
    std::vector<int64_t> timestamps;
    std::vector<double> cpuPercent;
    std::vector<double> memoryPercent;
    std::vector<double> download;
    std::vector<double> upload;
    std::vector<double> diskRead;
    std::vector<double> diskWrite;
    // One per GPU, in the order of Metrics::gpus.
    std::vector<GpuHistory> gpus;
};

class Monitor
{
public:
    // Reads and writes its preferences in the file, when one is given; samples
    // the system's /proc and /sys unless given a reader.
    explicit Monitor(ApplicationLookup lookup = {}, std::string preferences = {},
                     std::shared_ptr<ProcReader> reader = {});
    ~Monitor();

    const Metrics& metrics() const { return m_metrics; }
    const History& history() const { return m_history; }
    ProcessList& processes() { return m_processes; }
    ProcessList& applications() { return m_applications; }
    // Index of the process list's segment: applications, own processes, all processes.
    int scope() const { return m_scope; }
    void setScope(int scope);
    bool ready() const { return m_ready; }
    const std::string& error() const { return m_error; }
    // Milliseconds: 1000, 2000 or 5000.
    int interval() const;
    void setInterval(int interval);
    int historyDuration() const { return 60; }

    // The process, or application led by it, while it is the one that started then.
    const ProcessRecord* process(int pid, uint64_t startTicks) const;
    const ProcessRecord* application(int pid, uint64_t startTicks) const;
    static bool canManage(const ProcessRecord& process);
    // An error message, or empty once the signal went out.
    std::string endProcess(int pid, uint64_t startTicks, bool force);
    std::string endApplication(int pid, uint64_t startTicks, bool force);
    static std::string signalProcess(int pid, uint64_t startTicks, bool force);
    void sortBy(const std::string& column);
    // Desktop entries changed: processes are named again.
    void applicationsChanged();

    static std::string formatBytes(double bytes);
    static std::string formatRate(double bytes);
    static std::string formatPercent(double percent);
    static std::string formatTemperature(double celsius);
    static std::string formatPower(double watts);
    // Abbreviated to its two largest units, such as "2 h 5 min".
    static std::string formatDuration(int64_t seconds);

    // After every published sample, and after the interval changed.
    cutefish::base::Signal<> updated;
    cutefish::base::Signal<> intervalChanged;
    cutefish::base::Signal<> scopeChanged;

private:
    struct Application
    {
        std::string id;
        std::string name;
        std::string icon;
        // The sample that last named it.
        uint64_t seen = 0;
    };

    void savePreferences() const;
    void sample();
    void acceptSample(Snapshot snapshot);
    void identify(ProcessRecord* process);
    std::vector<ProcessRecord> groupApplications(const std::vector<ProcessRecord>& processes);
    static Application describeEntry(const std::optional<DesktopApplication>& entry);

    ApplicationLookup m_lookup;
    std::string m_preferences;
    ProcessList m_processes;
    ProcessList m_applications;
    int m_scope = 1;
    std::unordered_map<std::string, Application> m_applicationIds;
    std::unordered_map<std::string, Application> m_programs;
    uint64_t m_samples = 0;
    std::shared_ptr<ProcReader> m_reader;
    cutefish::base::Job m_sampleJob;
    cutefish::base::Job m_volumeJob;
    std::vector<Volume> m_volumes;
    cutefish::base::Timer m_timer;
    cutefish::base::Timer m_baselineTimer;
    Metrics m_metrics;
    History m_history;
    std::chrono::steady_clock::time_point m_historyClock = std::chrono::steady_clock::now();
    std::string m_error;
    bool m_ready = false;
};
