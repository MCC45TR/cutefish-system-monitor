// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/geometry.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/painter.h"
#include "ui/cell_layout.h"
#include "ui/view.h"

#include <optional>
#include <string>

// FishUI's point sizes in logical pixels.
inline double points(double size)
{
    return size * 4 / 3;
}

// The colour laid over the base, as Qt.tint does.
cutefish::ui::Color tint(cutefish::ui::Color base, cutefish::ui::Color overlay);

cutefish::ui::Label* addLabel(cutefish::ui::View* parent, std::string text, double pointSize = 0, int weight = 400,
                              std::optional<cutefish::ui::Color> color = std::nullopt);

// A rounded plate in a colour, its corners the theme's unless told otherwise.
class Plate : public cutefish::ui::View
{
public:
    explicit Plate(double radius = 0);

    void setRadius(double radius);
    // Unset: the theme's second background.
    void setColor(std::optional<cutefish::ui::Color> color);
    void setContinuousCorners(std::optional<bool> continuous);

protected:
    void paint(cutefish::ui::Painter& painter) override;

    cutefish::ui::Corners corners() const;

private:
    double m_radius;
    std::optional<cutefish::ui::Color> m_color;
    std::optional<bool> m_continuous;
};

// One device pixel at any scale, across the bottom of its rect.
void paintSeparator(cutefish::ui::Painter& painter, double x, double bottom, double width);

// One read-only row of a ListCard: the label, then its value in secondary text.
class InfoRow : public cutefish::ui::View
{
public:
    InfoRow(std::string text = {}, std::string value = {});

    void setText(std::string text);
    void setValue(std::string value);
    void setValueElide(cutefish::ui::Label::Elide elide);
    // Drawn below every visible row but the card's last.
    void setSeparatorVisible(bool visible);

    cutefish::ui::SizeF sizeHint() const override { return {0, 44}; }

protected:
    void layout() override;
    void paint(cutefish::ui::Painter& painter) override;

private:
    cutefish::ui::RowLayout* m_row;
    cutefish::ui::Label* m_text;
    cutefish::ui::Label* m_value;
    bool m_separator = false;
};

// A card of InfoRows: no inner padding, the rows carry their own.
class ListCard : public Plate
{
public:
    ListCard();

    InfoRow* addRow(std::string text, std::string value = {});
    // After rows were shown or hidden.
    void updateSeparators();

    cutefish::ui::SizeF sizeHint() const override;
    double heightForWidth(double width) const override;

protected:
    void layout() override;
};

// Names the card below it; padded to the card's text edge.
class SectionLabel : public cutefish::ui::View
{
public:
    explicit SectionLabel(std::string text = {});

    void setText(std::string text);
    void setElide(cutefish::ui::Label::Elide elide);

    cutefish::ui::SizeF sizeHint() const override;
    double heightForWidth(double width) const override;

protected:
    void layout() override;

private:
    cutefish::ui::Label* m_label;
};
