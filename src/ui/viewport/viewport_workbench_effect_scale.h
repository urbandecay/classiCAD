/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace classiCAD {

// Blender Workbench samples outline and curvature IDs at U.pixelsize, an
// integer UI-pixel footprint. Qt DPR describes the render-target scale but can
// be fractional, so round it once and share this scale across both renderers.
// Blender's separately configurable UI line-width/DPI contribution is not
// available in classiCAD and is therefore not represented here.
inline int workbenchEffectUiScale(qreal devicePixelRatio) noexcept
{
    if (!std::isfinite(devicePixelRatio)) return 1;
    return std::max(1, qRound(devicePixelRatio));
}

// Blender's curvature shader offsets normalized texture coordinates by the
// floating-point UI scale. Unlike integer outline taps, curvature samples
// must retain fractional device-pixel ratios until texture lookup.
inline float workbenchEffectFractionalUiScale(qreal devicePixelRatio) noexcept
{
    if (!std::isfinite(devicePixelRatio)) return 1.0f;
    return std::max(1.0f, static_cast<float>(devicePixelRatio));
}

} // namespace classiCAD
