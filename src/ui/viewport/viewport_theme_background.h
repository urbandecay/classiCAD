/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QColor>
#include <QImage>
#include <QPainter>
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

    const qreal devicePixelRatio = painter.device()
        ? std::max<qreal>(painter.device()->devicePixelRatioF(), 1.0)
        : 1.0;
    const QSize physicalSize(
        qRound(bounds.width() * devicePixelRatio),
        qRound(bounds.height() * devicePixelRatio));
    QImage gradientImage(physicalSize, QImage::Format_RGB32);
    gradientImage.setDevicePixelRatio(devicePixelRatio);
    const qreal width = std::max(1, physicalSize.width());
    const qreal height = std::max(1, physicalSize.height());
    for (int y = 0; y < gradientImage.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(gradientImage.scanLine(y));
        const qreal centeredY =
            (static_cast<qreal>(y) + 0.5) / height - 0.5;
        for (int x = 0; x < gradientImage.width(); ++x) {
            const qreal centeredX =
                (static_cast<qreal>(x) + 0.5) / width - 0.5;
            const qreal factor = themeBackground.style ==
                    ViewportThemeBackgroundStyle::Linear
                ? std::clamp((static_cast<qreal>(y) + 0.5) / height,
                             0.0, 1.0)
                : std::clamp(
                    std::hypot(centeredX, centeredY) * std::sqrt(2.0),
                    0.0, 1.0);
            const auto blendChannel = [factor](int highChannel,
                                               int lowChannel) {
                // Keep the fallback aligned with the viewport background's
                // direct-to-display shader path at every physical pixel.
                constexpr qreal gamma = 2.2;
                const qreal highValue = highChannel / 255.0;
                const qreal lowValue = lowChannel / 255.0;
                return qRound(std::pow(
                    std::pow(highValue, 1.0 / gamma) * (1.0 - factor) +
                        std::pow(lowValue, 1.0 / gamma) * factor,
                    gamma) * 255.0);
            };
            line[x] = qRgb(blendChannel(high.red(), low.red()),
                           blendChannel(high.green(), low.green()),
                           blendChannel(high.blue(), low.blue()));
        }
    }
    painter.drawImage(bounds.topLeft(), gradientImage);
}

} // namespace classiCAD
