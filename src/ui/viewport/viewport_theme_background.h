/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QRect>

#include <algorithm>
#include <cmath>

namespace classiCAD {

enum class ViewportThemeBackgroundStyle {
    SingleColor = 0,
    Linear = 1,
    Radial = 2,
};

struct ViewportThemeBackground {
    ViewportThemeBackgroundStyle style = ViewportThemeBackgroundStyle::Radial;
    QColor highColor = QColor(61, 61, 61);
    QColor lowColor = QColor(48, 48, 48);

    bool operator==(const ViewportThemeBackground &) const = default;
};

inline bool isValidViewportThemeBackground(
    const ViewportThemeBackground &background)
{
    const int style = static_cast<int>(background.style);
    return style >= static_cast<int>(ViewportThemeBackgroundStyle::SingleColor) &&
           style <= static_cast<int>(ViewportThemeBackgroundStyle::Radial) &&
           background.highColor.isValid() && background.lowColor.isValid();
}

inline void paintViewportThemeBackground(
    QPainter &painter, const QRect &bounds,
    const ViewportThemeBackground &themeBackground)
{
    if (bounds.isEmpty() || !isValidViewportThemeBackground(themeBackground))
        return;
    const QColor high = themeBackground.style ==
            ViewportThemeBackgroundStyle::SingleColor
        ? themeBackground.lowColor : themeBackground.highColor;
    const QColor low = themeBackground.lowColor;
    if (themeBackground.style == ViewportThemeBackgroundStyle::SingleColor) {
        painter.fillRect(bounds, low);
        return;
    }

    QGradientStops stops;
    constexpr int stopCount = 32;
    for (int stopIndex = 0; stopIndex <= stopCount; ++stopIndex) {
        const qreal factor = static_cast<qreal>(stopIndex) / stopCount;
        const auto blendChannel = [factor](qreal highChannel,
                                           qreal lowChannel) {
            constexpr qreal gamma = 2.2;
            return std::pow(std::pow(highChannel, 1.0 / gamma) *
                                (1.0 - factor) +
                            std::pow(lowChannel, 1.0 / gamma) * factor,
                            gamma);
        };
        stops.append({factor, QColor::fromRgbF(
            blendChannel(high.redF(), low.redF()),
            blendChannel(high.greenF(), low.greenF()),
            blendChannel(high.blueF(), low.blueF()))});
    }

    if (themeBackground.style == ViewportThemeBackgroundStyle::Linear) {
        QLinearGradient gradient(QPointF(bounds.left(), bounds.top()),
                                 QPointF(bounds.left(), bounds.bottom()));
        gradient.setStops(stops);
        painter.fillRect(bounds, gradient);
    } else {
        const QPointF center(bounds.x() + bounds.width() * 0.5,
                             bounds.y() + bounds.height() * 0.5);
        const qreal radius = 0.5 * std::hypot(bounds.width(), bounds.height());
        QRadialGradient gradient(center, std::max<qreal>(radius, 1.0), center);
        gradient.setStops(stops);
        painter.fillRect(bounds, gradient);
    }
}

} // namespace classiCAD
