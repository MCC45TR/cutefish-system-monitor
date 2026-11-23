// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

struct Gpu;

// A function of the library by its name; null when the library has none.
using NvmlSymbols = std::function<void*(const char* name)>;
// Opens the library: its functions, or an empty function when it cannot be opened.
using NvmlLoader = std::function<NvmlSymbols()>;

// The NVIDIA driver's management library, which reports what that driver's
// cards expose no counters for in sysfs. It is opened the first time a card
// is read; once opening or initialising it fails, it is not tried again.
class NvmlReader
{
public:
    explicit NvmlReader(NvmlLoader loader = systemLibrary());
    ~NvmlReader();

    NvmlReader(const NvmlReader&) = delete;
    NvmlReader& operator=(const NvmlReader&) = delete;

    // Opens the shared library at the path.
    static NvmlLoader library(std::string path);
    static NvmlLoader systemLibrary();

    // Fills the measurements of the card at the PCI address, such as
    // "0000:01:00.0", and the has-flags of those it reports; false when the
    // library or the card is unavailable.
    bool sample(const std::string& pciSlot, Gpu* gpu);
    // Opening or initialising the library failed.
    bool failed() const { return m_state == State::Failed; }

private:
    enum class State
    {
        Unloaded,
        Ready,
        Failed,
    };

    struct Functions;

    bool load();

    NvmlLoader m_loader;
    // Holds the library open.
    NvmlSymbols m_symbols;
    std::unique_ptr<Functions> m_functions;
    State m_state = State::Unloaded;
    // Null for a card the library does not know.
    std::unordered_map<std::string, void*> m_devices;
};
