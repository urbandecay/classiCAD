/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QColor>

namespace classiCAD {

// Theme-provided grid colors and the tunable appearance controls. Fade and
// stipple defaults follow Blender's overlay_grid_frag.glsl behavior.
struct BlenderGridAppearance {
    // Derived from the saved Blender 5.2 theme with the shade and blend rules
    // in overlay_instance.cc and ui::theme::get_color_blend_shade_4fv.
    QColor gridColor = QColor::fromRgb(94, 94, 94, 128);
    QColor emphasisColor = QColor::fromRgb(104, 104, 104, 255);
    QColor axisXColor = QColor::fromRgb(209, 35, 62, 235);
    QColor axisYColor = QColor::fromRgb(110, 179, 0, 235);
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
