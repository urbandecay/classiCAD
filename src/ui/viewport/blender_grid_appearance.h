/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QColor>

namespace classiCAD {

// Theme-provided grid colors and the tunable appearance controls. Fade and
// stipple defaults follow Blender's overlay_grid_frag.glsl behavior.
struct BlenderGridAppearance {
    // Blender 5.2's saved user theme uses RGB 84 for both grid levels; their
    // alpha differs (0.5 for minor lines, 1.0 for major lines).
    QColor gridColor = QColor::fromRgb(84, 84, 84, 128);
    QColor emphasisColor = QColor::fromRgb(84, 84, 84, 255);
    QColor axisXColor = QColor::fromRgb(209, 35, 62, 235);
    // Blender's saved UI axis colors are shaded using its grid-axis brightness
    // before compositing; the resulting green includes a blue component.
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
