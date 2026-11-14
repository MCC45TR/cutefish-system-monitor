// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "processespage.h"

#include "base/signal.h"
#include "ui/menu.h"
#include "ui/toast.h"
#include "ui/window.h"

#include <memory>

class Monitor;
class ProcessDetails;
class ResourcePage;

namespace cutefish::ui {
class AboutDialog;
class ConfirmDialog;
}

// The system monitor's window: the resources in a sidebar, the process list
// or a resource's page beside it.
class MainWindow
{
public:
    explicit MainWindow(Monitor& monitor);
    ~MainWindow();

    cutefish::ui::Window& window() { return m_window; }
    int currentPage() const { return m_page; }
    void setCurrentPage(int page);
    // Shows, raises and activates the window.
    void activate();
    void openDetails();
    void confirmEnd(bool force);
    // The selected process or application, while it runs.
    const ProcessRecord* selectedProcess() const;
    ProcessesPage* processesPage() const { return m_processes; }
    ResourcePage* resourcePage() const { return m_resource; }
    ProcessDetails* details() const { return m_details.get(); }
    cutefish::ui::Menu& contextMenu() { return m_contextMenu; }
    cutefish::ui::ConfirmDialog* confirmDialog() const { return m_confirm.get(); }

private:
    class Root;

    // The views and menus, all of which show text.
    void buildContent();
    // The menus the menu bar shows while the window is active.
    void buildMainMenu();
    void openAbout();
    void retranslate();
    void update();
    void updateContextMenu();

    Monitor& m_monitor;
    cutefish::ui::Window m_window;
    // Errors from ending processes, as FishUI's passive notification.
    cutefish::ui::Toast m_toast;
    Root* m_root;
    Selection m_selection;
    int m_page = 0;
    ProcessesPage* m_processes;
    ResourcePage* m_resource = nullptr;
    std::unique_ptr<cutefish::ui::Menu> m_mainMenu;
    cutefish::ui::Menu m_contextMenu;
    std::unique_ptr<cutefish::ui::AboutDialog> m_about;
    std::unique_ptr<cutefish::ui::ConfirmDialog> m_confirm;
    std::unique_ptr<ProcessDetails> m_details;
    cutefish::base::Subscription m_updated;
    cutefish::base::Subscription m_intervalChanged;
    cutefish::base::Subscription m_languageChanged;
};
