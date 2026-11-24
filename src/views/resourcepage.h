// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/signal.h"
#include "ui/scroll_view.h"

#include <vector>

class ChartCard;
class InfoRow;
class ListCard;
class Monitor;
class SectionLabel;
struct Gpu;

namespace cutefish::ui {
class Column;
class ColumnLayout;
class GridLayout;
class Label;
class ProgressBar;
} // namespace cutefish::ui

// A resource's chart and details: CPU (1), memory (2), network (3), disks (4)
// or one GPU (5 and on, in the order of Metrics::gpus).
class ResourcePage : public cutefish::ui::ScrollView
{
public:
    ResourcePage(Monitor& monitor, int page);

    void setPage(int page);
    int page() const { return m_page; }

protected:
    void layout() override;

private:
    struct Section
    {
        cutefish::ui::ColumnLayout* column = nullptr;
        SectionLabel* title = nullptr;
        ListCard* card = nullptr;
        cutefish::ui::ProgressBar* usage = nullptr;
        std::vector<InfoRow*> rows;
    };
    struct Tile;

    void rebuild();
    // The page's GPU, while it is listed.
    const Gpu* gpu() const;
    void update();
    void updateChart();
    Section addSection(cutefish::ui::View* parent, size_t rows);
    cutefish::ui::Label* addHeading(const char* text);

    Monitor& m_monitor;
    int m_page;
    cutefish::ui::Column* m_content;
    // As the Qt version's ColumnLayout, inside the scrolled margins.
    cutefish::ui::ColumnLayout* m_layout;
    ChartCard* m_chart = nullptr;
    cutefish::ui::GridLayout* m_cores = nullptr;
    std::vector<Tile*> m_tiles;
    ListCard* m_card = nullptr;
    std::vector<Section> m_sections;
    std::vector<Section> m_volumes;
    size_t m_built[3] = {0, 0, 0};
    cutefish::base::Subscription m_updated;
};
