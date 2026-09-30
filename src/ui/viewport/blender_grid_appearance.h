/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QColor>

namespace classiCAD {

// Theme-provided grid colors and the tunable appearance controls. Fade and
// stipple defaults follow Blender's overlay_grid_frag.glsl behavior.
struct BlenderGridAppearance {
    // Blender 5.2's saved theme uses the same RGB for grid/grid-major; their
    // alpha differs (0.5 for minor lines, 1.0 for major lines).
    QColor gridColor = QColor::fromRgb(84, 84, 84, 128);
    QColor emphasisColor = QColor::fromRgb(84, 84, 84, 255);
    QColor axisXColor = QColor::fromRgb(209, 35, 62, 235);
    // Calibrated against Blender 5.2's composited grid-axis screenshot. The
    // blue component is intentional; Blender's Y axis is not pure green.
    QColor axisYColor = QColor::fromRgb(109, 176, 23, 235);
    QColor axisZColor = QColor::fromRgb(26, 115, 209, 235);
    qreal opacity = 1.0;
    bool lowAlphaStipple = true;
    qreal stippleThreshold = 0.1;
    qreal stippleDashWidth = 4.0;
    qreal grazingFadeExponent = 3.0;
    qreal orthographicEdgeFadeExponent = 2.0;
    qreal farFadeStart = 0.5;
    qreal farFadeEnd = 1.0;

    bool operator==(const BlenderGridAppearance &) const = default;
};

bool isValidBlenderGridAppearance(const BlenderGridAppearance &appearance);

} // namespace classiCAD
