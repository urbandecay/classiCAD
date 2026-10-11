/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "workbench_lighting.h"

#include <QColor>
#include <QString>

namespace classiCAD {

inline QColor viewportSelectionColor()
{
    // Blender's default 3D View edge-selection theme color.
    return QColor(QStringLiteral("#ff9900"));
}

enum class ViewportShadingMode {
    Wireframe,
    Solid,
};

enum class ViewportLightingMode {
    Studio,
    MatCap,
    Flat,
};

enum class ViewportColorMode {
    Material,
    Object,
    Random,
    Attribute,
    Texture,
    Custom,
};

enum class ViewportWireColorMode {
    Theme,
    Object,
    Random,
};

enum class ViewportBackgroundMode {
    Theme,
    World,
    Custom,
};

enum class ViewportCavityType {
    World = 0,  // Blender's world-space SSAO cavity.
    Screen = 1, // Blender's screen-space normal curvature.
    Both = 2,
};

// Blender keeps wireframe X-Ray separate from the solid X-Ray setting. The
// initial wireframe state mirrors classiCAD's existing through-visible view.
struct ViewportShadingSettings {
    // Solid is the useful modeling default: scene surfaces occlude geometry
    // behind them. Wireframe remains available as an explicit viewport mode.
    ViewportShadingMode mode = ViewportShadingMode::Solid;
    ViewportLightingMode lightingMode = ViewportLightingMode::Studio;
    QString studioLightPreset = QStringLiteral("Default");
    QString matcapPreset = QStringLiteral("basic_grey");
    bool matcapFlipX = false;
    int studioLightRotationDegrees = 0;
    bool worldSpaceLighting = false;
    bool xray = false;
    bool xrayWireframe = true;
    ViewportWireColorMode wireColorMode = ViewportWireColorMode::Theme;
    ViewportColorMode colorMode = ViewportColorMode::Material;
    ViewportBackgroundMode backgroundMode = ViewportBackgroundMode::Theme;
    ViewportCavityType cavityType = ViewportCavityType::Screen;
    QColor customColor = QColor::fromRgbF(0.8, 0.8, 0.8);
    QColor outlineColor = QColor(Qt::black);
    QColor customBackgroundColor = QColor::fromRgb(48, 48, 48);
    QVector3D shadowDirection = QVector3D(0.57735026f, 0.57735026f,
                                          0.57735026f);
    bool backfaceCulling = false;
    // Blender 5.2.2 initializes View3DShading with specular enabled, while
    // object outline, shadows, and cavity are opt-in shading flags.
    bool outline = false;
    bool specularLighting = true;
    bool shadows = false;
    bool depthOfField = false;
    bool cavity = false;
    // Blender stores the screen-space AO controls on SceneDisplay and the
    // ridge/valley factors on View3DShading. Keep their matching defaults.
    qreal cavityDistance = 0.2;
    qreal cavityAttenuation = 1.0;
    int cavitySamples = 16;
    qreal cavityRidgeFactor = 1.0;
    qreal cavityValleyFactor = 1.0;
    qreal curvatureRidgeFactor = 1.0;
    qreal curvatureValleyFactor = 1.0;
    qreal xrayAlpha = 0.5;
    qreal shadowIntensity = 0.5;
    qreal shadowOffset = 0.1;
    qreal shadowFocus = 0.0;

    bool xrayEnabled() const noexcept
    {
        return mode == ViewportShadingMode::Wireframe
                   ? xrayWireframe
                   : xray && xrayAlpha < 1.0;
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
