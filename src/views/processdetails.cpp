// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "processdetails.h"

#include "components.h"
#include "fileutil.h"
#include "monitor.h"
#include "procreader.h"

#include "base/i18n.h"
#include "base/locale.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/cell_layout.h"
#include "ui/list_view.h"
#include "ui/text_area.h"
#include "ui/theme.h"
#include "ui/window.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <unistd.h>

using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;
using cutefish::i18n::trn;
using cutefish::i18n::trNoop;

static constexpr double kTop = 8;
static constexpr double kSpacing = 12;
static constexpr double kInset = 16;
static constexpr double kIcon = 40;
static constexpr double kRowHeight = 36;
static constexpr double kMemberHeaderHeight = 28;
static constexpr int kVisibleMembers = 7;
static constexpr int kCommandLines = 3;
static constexpr double kCpuWidth = 64;
static constexpr double kMemoryWidth = 80;
static constexpr double kPidWidth = 56;

static std::string dash(bool available, const std::string& text)
{
    return available ? text : "—";
}

// The full command line as the card's last row: selectable, wrapped at any
// character and scrolled past a few lines.
class CommandRow : public View
{
public:
    CommandRow()
        : m_title(addChild<Label>(tr("Command")))
        , m_text(addChild<TextArea>())
        , m_line(addChild<TextArea>("—"))
    {
        m_text->setName("detailsCommand");
        for (TextArea* area : {m_text, m_line}) {
            area->setReadOnly(true);
            area->setWrapMode(TextArea::WrapMode::WrapAnywhere);
            area->setFontSize(points(9));
            area->setPadding({kInset, 0, kInset, 0});
            area->setFrameVisible(false);
        }
        // Measures one line of the text's font.
        m_line->setVisible(false);
    }

    void setText(const std::string& text)
    {
        if (text == m_text->text())
            return;
        m_text->setText(text);
        m_text->setCursorPosition(0);
        requestLayout();
    }

    double heightForWidth(double width) const override
    {
        return 2 * padding() + m_title->sizeHint().height + 2 + textHeight(width);
    }

    SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        const double title = m_title->sizeHint().height;
        m_title->setGeometry({kInset, padding(), std::max(0.0, width - 2 * kInset), title});
        m_text->setTextColor(Theme::current().disabledText);
        m_text->setGeometry({0, padding() + title + 2, width, textHeight(width)});
    }

private:
    // The title sits where a row's label does, and the text ends as far from the bottom.
    double padding() const { return rounding::centeredStart(kRowHeight, m_title->sizeHint().height); }

    double textHeight(double width) const
    {
        return std::min(m_text->heightForWidth(width), kCommandLines * m_line->heightForWidth(width));
    }

    Label* m_title;
    TextArea* m_text;
    TextArea* m_line;
};

struct MemberColumn
{
    double width;
    bool secondary;
};

// One process of an application, or with no record the titles over them:
// its program, CPU, memory and PID.
class MemberRow : public View
{
public:
    explicit MemberRow(bool header = false)
        : m_header(header)
        , m_name(addLabel(this, header ? tr("Name") : std::string(), header ? 8 : 9, header ? 500 : 400))
    {
        m_name->setVerticallyCentered(true);
        static const char* const titles[] = {trNoop("CPU"), trNoop("Memory"), trNoop("PID")};
        for (size_t i = 0; i < m_values.size(); ++i) {
            m_values[i] = addLabel(this, header ? tr(titles[i]) : std::string(), header ? 8 : 9, header ? 500 : 400);
            m_values[i]->setVerticallyCentered(true);
            m_values[i]->setTextAlignment(Label::TextAlignment::Right);
        }
    }

    void bind(const ProcessRecord& member, bool last)
    {
        m_name->setText(ProcessList::programName(member));
        m_values[0]->setText(Monitor::formatPercent(member.cpu));
        m_values[1]->setText(Monitor::formatBytes(double(member.memory)));
        m_values[2]->setText(std::to_string(member.pid));
        m_last = last;
        requestLayout();
        requestPaint();
    }

    SizeF sizeHint() const override { return {0, m_header ? kMemberHeaderHeight : kRowHeight}; }

protected:
    void layout() override
    {
        const Theme& theme = Theme::current();
        static constexpr std::array<MemberColumn, 3> columns{{{kCpuWidth, false}, {kMemoryWidth, false}, {kPidWidth, true}}};
        const double height = geometry().height;
        double x = geometry().width - kInset;
        for (size_t i = columns.size(); i-- > 0;) {
            x -= columns[i].width;
            m_values[i]->setColor(m_header || columns[i].secondary ? theme.disabledText : theme.text);
            m_values[i]->setGeometry({x, 0, columns[i].width, height});
        }
        m_name->setColor(m_header ? theme.disabledText : theme.text);
        m_name->setGeometry({kInset, 0, std::max(0.0, x - kSpacing - kInset), height});
    }

    void paint(Painter& painter) override
    {
        if (!m_last)
            paintSeparator(painter, kInset, geometry().height, geometry().width - 2 * kInset);
    }

private:
    bool m_header;
    Label* m_name;
    std::array<Label*, 3> m_values{};
    bool m_last = false;
};

// An application's processes under their column titles, as tall as a few
// of them and scrolled past that.
class MembersCard : public Plate
{
public:
    MembersCard()
        : Plate(Theme::current().bigRadius)
        , m_header(addChild<MemberRow>(true))
        , m_list(addChild<ListView>())
    {
        m_list->setName("detailsMembers");
        m_list->setBottomCornerRadius(Theme::current().bigRadius);
        m_list->setRowHeight(kRowHeight);
        m_list->createItem = [] { return std::make_unique<MemberRow>(); };
        m_list->bindItem = [this](View* item, size_t row) {
            if (row < m_members.size())
                static_cast<MemberRow*>(item)->bind(m_members[row], row + 1 == m_members.size());
        };
    }

    void setMembers(std::vector<ProcessRecord> members)
    {
        // Heaviest first; PSS changes every few seconds, so the order rarely moves.
        std::sort(members.begin(), members.end(), [](const ProcessRecord& left, const ProcessRecord& right) {
            return left.memory != right.memory ? left.memory > right.memory : left.pid < right.pid;
        });
        m_members = std::move(members);
        m_list->setRowCount(m_members.size());
        m_list->reload();
    }

    double heightForWidth(double) const override
    {
        const size_t rows = std::clamp<size_t>(m_members.size(), 1, kVisibleMembers);
        return kMemberHeaderHeight + double(rows) * kRowHeight;
    }

    SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        m_header->setGeometry({0, 0, width, kMemberHeaderHeight});
        m_list->setGeometry({0, kMemberHeaderHeight, width, std::max(0.0, geometry().height - kMemberHeaderHeight)});
    }

private:
    MemberRow* m_header;
    ListView* m_list;
    std::vector<ProcessRecord> m_members;
};

// Under the title: the icon, name and state, then cards of what the sampler
// and a few files of the process tell, sized to their rows.
class ProcessDetails::Content : public View
{
public:
    explicit Content(bool application)
        : m_application(application)
        , m_column(addChild<ColumnLayout>())
        , m_bootTime(ProcReader::bootTime(readFile("/proc/stat")))
    {
        m_column->setSpacing(kSpacing);
        auto* header = m_column->addChild<RowLayout>();
        header->setSpacing(kSpacing);
        header->setLayoutHints({.fillWidth = true});
        m_icon = header->addChild<ThemeIcon>("application-x-executable", kIcon);
        m_icon->setLayoutHints({.preferredWidth = kIcon, .preferredHeight = kIcon});
        auto* names = header->addChild<ColumnLayout>();
        names->setSpacing(4);
        names->setLayoutHints({.fillWidth = true});
        m_name = addLabel(names, {}, 12, 500);
        m_name->setLayoutHints({.fillWidth = true});
        m_subtitle = addLabel(names, {}, 9);
        m_subtitle->setName("detailsSubtitle");
        m_subtitle->setLayoutHints({.fillWidth = true});

        m_activity = m_column->addChild<ListCard>();
        m_activity->setName("detailsActivity");
        m_activity->setLayoutHints({.fillWidth = true});
        m_cpu = addRow(m_activity, tr("CPU"));
        m_memory = addRow(m_activity, tr("Memory"));
        if (!application)
            m_resident = addRow(m_activity, tr("Resident memory"));
        m_read = addRow(m_activity, tr("Disk read"));
        m_write = addRow(m_activity, tr("Disk write"));
        m_threads = addRow(m_activity, tr("Threads"));
        if (application) {
            m_running = addRow(m_activity, tr("Running time"));
        } else {
            m_openFiles = addRow(m_activity, tr("Open files"));

            m_process = m_column->addChild<ListCard>();
            m_process->setName("detailsProcess");
            m_process->setLayoutHints({.fillWidth = true});
            m_parent = addRow(m_process, tr("Parent process"));
            m_running = addRow(m_process, tr("Running time"));
            m_nice = addRow(m_process, tr("Nice value"));
            m_unit = addRow(m_process, tr("Unit"));
            m_unit->setValueElide(Label::Elide::Middle);
            m_executable = addRow(m_process, tr("Executable"));
            m_executable->setValueElide(Label::Elide::Middle);
            m_command = m_process->addChild<CommandRow>();
            // The command follows the last row.
            m_executable->setSeparatorVisible(true);
        }

        if (application) {
            m_members = m_column->addChild<MembersCard>();
            m_members->setLayoutHints({.fillWidth = true});
        }
    }

    void update(Monitor& monitor, const ProcessRecord* process, int pid)
    {
        m_icon->setIconName(process && !process->iconName.empty() ? process->iconName : "application-x-executable");
        m_name->setText(process ? process->displayName : m_application ? tr("Application quit") : tr("Process exited"));
        std::vector<std::string> subtitle = {format(tr("PID {pid}"), {{"pid", std::to_string(pid)}})};
        if (process && m_application) {
            subtitle.push_back(format(trn("{count} process", "{count} processes", process->members.size()),
                                      {{"count", std::to_string(process->members.size())}}));
        } else if (process) {
            const std::string state = ProcReader::stateName(process->state);
            if (!state.empty())
                subtitle.push_back(state);
            if (!process->user.empty())
                subtitle.push_back(process->user);
        }
        std::string joined;
        for (const std::string& part : subtitle)
            joined += (joined.empty() ? "" : " · ") + part;
        m_subtitle->setText(joined);

        m_cpu->setValue(dash(process, process ? Monitor::formatPercent(process->cpu) : std::string()));
        m_memory->setValue(dash(process, process ? Monitor::formatBytes(double(process->memory)) : std::string()));
        uint64_t readBytes = process ? process->readBytes : 0;
        uint64_t writeBytes = process ? process->writeBytes : 0;
        if (process && m_application) {
            readBytes = 0;
            writeBytes = 0;
            for (const ProcessRecord& member : process->members) {
                if (!member.ioAvailable)
                    continue;
                readBytes += member.readBytes;
                writeBytes += member.writeBytes;
            }
        }
        const bool io = process && process->ioAvailable;
        // TRANSLATORS: a disk rate and the amount since the process started, such as "1.2 MiB/s, 340.5 MiB in total".
        const char* const transfer = trNoop("{rate}, {total} in total");
        m_read->setValue(dash(io, io ? format(tr(transfer), {{"rate", Monitor::formatRate(process->readRate)},
                                                           {"total", Monitor::formatBytes(double(readBytes))}})
                                     : std::string()));
        m_write->setValue(dash(io, io ? format(tr(transfer), {{"rate", Monitor::formatRate(process->writeRate)},
                                                            {"total", Monitor::formatBytes(double(writeBytes))}})
                                      : std::string()));
        m_threads->setValue(dash(process && process->threads, process ? std::to_string(process->threads) : std::string()));
        m_running->setValue(process ? runningTime(*process) : "—");

        if (m_members)
            m_members->setMembers(process ? process->members : std::vector<ProcessRecord>());
        if (m_application) {
            requestLayout();
            return;
        }

        const ProcessExtras extras = process ? ProcReader::readExtras("/proc", process->pid, process->startTicks)
                                             : ProcessExtras();
        m_resident->setValue(dash(process, process ? Monitor::formatBytes(double(process->rss)) : std::string()));
        m_openFiles->setValue(dash(extras.openFiles >= 0, std::to_string(extras.openFiles)));
        std::string parent = "—";
        if (process && process->parentPid > 0) {
            const ProcessRecord* record = monitor.processes().find(process->parentPid);
            parent = record ? format(tr("{name} ({pid})"), {{"name", record->displayName},
                                                            {"pid", std::to_string(process->parentPid)}})
                            : std::to_string(process->parentPid);
        }
        m_parent->setValue(parent);
        m_nice->setValue(process ? std::to_string(process->nice) : "—");
        if (process) {
            m_unit->setValue(dash(!extras.unit.empty(), extras.unit));
            m_unit->setToolTip(extras.unit);
            m_executable->setValue(dash(!process->executable.empty(), process->executable));
            m_executable->setToolTip(process->executable);
            m_command->setText(!process->command.empty() ? process->command
                                                         : dash(!process->executable.empty(), process->executable));
        }
        requestLayout();
    }

    double heightForWidth(double width) const override { return kTop + m_column->heightForWidth(width); }

    SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override
    {
        m_subtitle->setColor(Theme::current().disabledText);
        m_column->setGeometry(localRect().adjusted(0, kTop, 0, 0));
    }

private:
    static InfoRow* addRow(ListCard* card, std::string text)
    {
        InfoRow* row = card->addRow(std::move(text), "—");
        row->setLayoutHints({.preferredHeight = kRowHeight});
        return row;
    }

    std::string runningTime(const ProcessRecord& process) const
    {
        const long ticks = sysconf(_SC_CLK_TCK);
        if (m_bootTime <= 0 || ticks <= 0)
            return "—";
        const int64_t started = m_bootTime + int64_t(process.startTicks / uint64_t(ticks));
        const int64_t now = int64_t(std::time(nullptr));
        const cutefish::base::Locale& locale = cutefish::base::Locale::system();
        const auto start = cutefish::base::DateTime::fromSecondsSinceEpoch(started);
        const bool today = start.date == cutefish::base::DateTime::fromSecondsSinceEpoch(now).date;
        const std::string time = today ? locale.toString(start.time, cutefish::base::Locale::FormatType::Short)
                                       : locale.toString(start, cutefish::base::Locale::FormatType::Short);
        // TRANSLATORS: how long a process has run and when it started, such as "2 h 5 min, since 14:03".
        return format(tr("{duration}, since {time}"), {{"duration", Monitor::formatDuration(now - started)}, {"time", time}});
    }

    bool m_application;
    ColumnLayout* m_column;
    int64_t m_bootTime;
    ThemeIcon* m_icon;
    Label* m_name;
    Label* m_subtitle;
    InfoRow* m_cpu;
    InfoRow* m_memory;
    ListCard* m_activity;
    ListCard* m_process = nullptr;
    InfoRow* m_resident = nullptr;
    InfoRow* m_read;
    InfoRow* m_write;
    InfoRow* m_threads;
    InfoRow* m_openFiles = nullptr;
    InfoRow* m_parent = nullptr;
    InfoRow* m_running = nullptr;
    InfoRow* m_nice = nullptr;
    InfoRow* m_unit = nullptr;
    InfoRow* m_executable = nullptr;
    CommandRow* m_command = nullptr;
    MembersCard* m_members = nullptr;
};

ProcessDetails::ProcessDetails(Window* parent, Monitor& monitor, const Selection& target)
    : m_monitor(monitor)
    , m_target(target)
    , m_dialog(parent, target.application ? tr("Application Details") : tr("Process Details"), 480)
{
    auto content = std::make_unique<Content>(target.application);
    m_content = content.get();
    m_dialog.setContentView(std::move(content));
    m_dialog.onClosed = [this] {
        if (onClosed)
            onClosed();
    };
    m_updated = m_monitor.updated.connect([this] { update(); });
    update();
}

void ProcessDetails::show()
{
    m_dialog.show();
    if (Window* window = m_dialog.window())
        window->addShortcut(cutefish::ui::NoModifier, cutefish::ui::key::Escape, [this] { m_dialog.close(); });
}

void ProcessDetails::update()
{
    m_content->update(m_monitor,
                      m_target.application ? m_monitor.application(m_target.pid, m_target.startTicks)
                                           : m_monitor.process(m_target.pid, m_target.startTicks),
                      m_target.pid);
}
