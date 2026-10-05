/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "workbench_lighting.h"

#include <QString>

namespace classiCAD {

enum class ViewportShadingMode {
    Wireframe,
    Solid,
};

enum class ViewportLightingMode {
    Studio,
    MatCap,
    Flat,
};

// Blender keeps wireframe X-Ray separate from the solid X-Ray setting. The
// initial wireframe state mirrors classiCAD's existing through-visible view.
struct ViewportShadingSettings {
    ViewportShadingMode mode = ViewportShadingMode::Wireframe;
    ViewportLightingMode lightingMode = ViewportLightingMode::Studio;
    QString studioLightPreset = QStringLiteral("Default");
    QString matcapPreset = QStringLiteral("basic_grey");
    int studioLightRotationDegrees = 0;
    bool worldSpaceLighting = false;
    bool hasCustomStudioLighting = false;
    WorkbenchStudioLighting customStudioLighting;
    bool xray = false;
    bool xrayWireframe = true;

    bool xrayEnabled() const noexcept
    {
        return mode == ViewportShadingMode::Wireframe
                   ? xrayWireframe
                   : xray;
    }

    void toggleXray() noexcept
    {
        if (mode == ViewportShadingMode::Wireframe) {
            xrayWireframe = !xrayWireframe;
        } else {
            xray = !xray;
        }
    }
};

} // namespace classiCAD
