/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "workbench_lighting.h"
#include "core/document/document_settings.h"

#include <QColor>
#include <QString>
#include <QByteArray>

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
    // Retain fractional degrees from Blender's saved rotation. The UI slider
    // remains whole-degree, while rendering and sessions keep the precision.
    qreal studioLightRotationDegrees = 0.0;
    bool worldSpaceLighting = false;
    bool xray = false;
    bool xrayWireframe = true;
    ViewportWireColorMode wireColorMode = ViewportWireColorMode::Theme;
    bool solidWireframes = false;
    // Blender's global overlays switch gates Solid-mode wire overlays,
    // including per-object show_wire and WIRE display flags.
    bool wireOverlaysVisible = true;
    // Blender's Overlay pass draws a separate silhouette for selected objects.
    bool selectedObjectOutlineVisible = true;
    bool gridFloorVisible = true;
    bool orthographicGridVisible = true;
    bool gridAxisXVisible = true;
    bool gridAxisYVisible = true;
    bool gridAxisZVisible = false;
    qreal wireframeOpacity = 1.0;
    qreal wireframeThreshold = 1.0;
    ViewportColorMode colorMode = ViewportColorMode::Material;
    ViewportBackgroundMode backgroundMode = ViewportBackgroundMode::Theme;
    ViewportCavityType cavityType = ViewportCavityType::Screen;
    QColor customColor = QColor::fromRgbF(0.8, 0.8, 0.8);
    // Theme wire colors are separate from Workbench fill colors and from
    // object/layer display colors, matching Blender's TH_WIRE and
    // TH_WIRE_EDIT roles in the editable viewport.
    QColor themeWireColor = QColor(Qt::black);
    QColor themeEditWireColor = QColor(Qt::black);
    QColor themeWireBackgroundColor = QColor(Qt::black);
    QColor themeActiveObjectColor = QColor(QStringLiteral("#ff8a00"));
    QColor themeSelectedObjectColor = QColor(QStringLiteral("#ff8a00"));
    QColor outlineColor = QColor(Qt::black);
    QColor customBackgroundColor = QColor::fromRgb(48, 48, 48);
    QVector3D shadowDirection = QVector3D(0.57735026f, 0.57735026f,
                                          0.57735026f);
    bool backfaceCulling = false;
    // Blender 5.2.2 enables specular highlights and object outlines by
    // default, while shadows and cavity start disabled.
    bool outline = true;
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
    qreal xrayWireframeAlpha = 0.5;
    qreal shadowIntensity = 0.5;
    qreal shadowOffset = 0.1;
    qreal shadowFocus = 0.0;
    // Imported scene display transform controls used by offscreen scene renders.
    // 0 AgX, 1 Standard, 2 Khronos PBR Neutral, 3 Filmic, 4 imported LUT.
    int colorTransform = 0;
    QString colorLutId;
    QByteArray colorLutData;
    ImportedBlenderColorLutMetadata colorLutMetadata;
    qreal colorExposure = 0.0;
    qreal colorGamma = 1.0;
    // Render-settings display dither; ordinary Workbench Solid uses zero.
    qreal colorDitherIntensity = 0.0;

    bool xrayEnabled() const noexcept
    {
        return mode == ViewportShadingMode::Wireframe
                   ? xrayWireframe
                   : xray && xrayAlpha < 1.0;
    }

    qreal xrayAlphaForMode() const noexcept
    {
        return mode == ViewportShadingMode::Wireframe
                   ? xrayWireframeAlpha
                   : xrayAlpha;
    }

    bool xrayEnabledForMode() const noexcept
    {
        return mode == ViewportShadingMode::Wireframe ? xrayWireframe
                                                       : xray;
    }

    void setXrayEnabledForMode(bool enabled) noexcept
    {
        if (mode == ViewportShadingMode::Wireframe) {
            xrayWireframe = enabled;
        } else {
            xray = enabled;
        }
    }

    void setXrayAlphaForMode(qreal alpha) noexcept
    {
        if (mode == ViewportShadingMode::Wireframe) {
            xrayWireframeAlpha = alpha;
        } else {
            xrayAlpha = alpha;
        }
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

// Scene renders use Blender's Scene.display.shading record when one was
// imported. The interactive View3D shading remains the fallback for native
// documents without imported scene settings.
inline ViewportShadingSettings workbenchRenderShadingSettings(
    const ViewportShadingSettings &interactiveSettings,
    const ViewportShadingSettings &sceneSettings,
    bool hasSceneSettings)
{
    return hasSceneSettings ? sceneSettings : interactiveSettings;
}

} // namespace classiCAD
