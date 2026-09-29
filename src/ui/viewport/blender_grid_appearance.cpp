/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "blender_grid_appearance.h"

#include <cmath>

namespace classiCAD {

bool isValidBlenderGridAppearance(const BlenderGridAppearance &appearance)
{
    return appearance.gridColor.isValid() && appearance.emphasisColor.isValid() &&
           appearance.axisXColor.isValid() && appearance.axisYColor.isValid() &&
           appearance.axisZColor.isValid() && std::isfinite(appearance.opacity) &&
           appearance.opacity >= 0.0 && appearance.opacity <= 2.0 &&
           std::isfinite(appearance.stippleThreshold) &&
           appearance.stippleThreshold > 0.0 && appearance.stippleThreshold <= 1.0 &&
           std::isfinite(appearance.stippleDashWidth) &&
           appearance.stippleDashWidth >= 0.25 && appearance.stippleDashWidth <= 32.0 &&
           std::isfinite(appearance.grazingFadeExponent) &&
           appearance.grazingFadeExponent >= 0.1 &&
           appearance.grazingFadeExponent <= 8.0 &&
           std::isfinite(appearance.orthographicEdgeFadeExponent) &&
           appearance.orthographicEdgeFadeExponent >= 0.1 &&
           appearance.orthographicEdgeFadeExponent <= 8.0 &&
           std::isfinite(appearance.farFadeStart) &&
           std::isfinite(appearance.farFadeEnd) &&
           appearance.farFadeStart >= 0.0 && appearance.farFadeStart < appearance.farFadeEnd &&
           appearance.farFadeEnd <= 1.0;
}

} // namespace classiCAD
