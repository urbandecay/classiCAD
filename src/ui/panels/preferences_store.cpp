/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "preferences_store.h"

#include "ui/viewport/blender_grid_appearance.h"

#include <QColor>
#include <QSettings>

namespace classiCAD {
namespace {

QColor readGridColor(const QSettings &settings,
                     const QString &key,
                     const QColor &defaultColor)
{
    const QColor stored(settings.value(key, defaultColor.name(QColor::HexArgb)).toString());
    return stored.isValid() ? stored : defaultColor;
}

template <typename WriteValues>
void writeSettings(WriteValues writeValues)
{
    QSettings settings;
    writeValues(settings);
    settings.sync();
}

} // namespace

StoredPreferences loadStoredPreferences()
{
    QSettings settings;
    StoredPreferences stored;
    PreferencesDialogValues &dialog = stored.dialogValues;

    dialog.panButton = settings.value(QStringLiteral("keymap/panButton"),
                                     QStringLiteral("middle"))
                               .toString() == QStringLiteral("right")
                           ? Qt::RightButton
                           : Qt::MiddleButton;
    dialog.snapLabelsVisible =
        settings.value(QStringLiteral("viewport/snapLabelsVisible"), true).toBool();
    dialog.smoothCurveDisplay =
        settings.value(QStringLiteral("viewport/smoothCurveDisplay"), true).toBool();
    dialog.architecturalDimensionFont =
        settings.value(QStringLiteral("dimensions/architecturalFont"), false).toBool();

    BlenderGridAppearance gridAppearance;
    gridAppearance.gridColor = readGridColor(settings,
                                             QStringLiteral("viewport/gridColor"),
                                             gridAppearance.gridColor);
    gridAppearance.emphasisColor = readGridColor(
        settings, QStringLiteral("viewport/gridEmphasisColor"), gridAppearance.emphasisColor);
    gridAppearance.axisXColor = readGridColor(settings,
                                              QStringLiteral("viewport/gridAxisXColor"),
                                              gridAppearance.axisXColor);
    gridAppearance.axisYColor = readGridColor(settings,
                                              QStringLiteral("viewport/gridAxisYColor"),
                                              gridAppearance.axisYColor);
    gridAppearance.axisZColor = readGridColor(settings,
                                              QStringLiteral("viewport/gridAxisZColor"),
                                              gridAppearance.axisZColor);
    gridAppearance.opacity = settings.value(QStringLiteral("viewport/gridOpacity"),
                                             gridAppearance.opacity).toDouble();
    gridAppearance.lowAlphaStipple = settings.value(
        QStringLiteral("viewport/gridStipple"), gridAppearance.lowAlphaStipple).toBool();

    if (!isValidBlenderGridAppearance(gridAppearance)) {
        gridAppearance = BlenderGridAppearance{};
    } else if ((gridAppearance.gridColor == QColor(QStringLiteral("#38858585")) &&
                gridAppearance.emphasisColor == QColor(QStringLiteral("#4da1a1a1")) &&
                gridAppearance.axisXColor == QColor(QStringLiteral("#b8c7332e")) &&
                gridAppearance.axisYColor == QColor(QStringLiteral("#b842ad38")) &&
                gridAppearance.axisZColor == QColor(QStringLiteral("#b83378d1"))) ||
               (gridAppearance.gridColor == QColor(QStringLiteral("#805e5e5e")) &&
                gridAppearance.emphasisColor == QColor(QStringLiteral("#ff686868")) &&
                gridAppearance.axisXColor == QColor(QStringLiteral("#ebd1233e")) &&
                gridAppearance.axisYColor == QColor(QStringLiteral("#eb6eb300")) &&
                gridAppearance.axisZColor == QColor(QStringLiteral("#eb1a73d1"))) ||
               (gridAppearance.gridColor == QColor(QStringLiteral("#80545454")) &&
                gridAppearance.emphasisColor == QColor(QStringLiteral("#ff545454")) &&
                gridAppearance.axisXColor == QColor(QStringLiteral("#ebd1233e")) &&
                gridAppearance.axisYColor == QColor(QStringLiteral("#eb70b612")) &&
                gridAppearance.axisZColor == QColor(QStringLiteral("#eb1a73d1"))) ||
               (gridAppearance.gridColor == QColor(QStringLiteral("#80545454")) &&
                gridAppearance.emphasisColor == QColor(QStringLiteral("#ff545454")) &&
                gridAppearance.axisXColor == QColor(QStringLiteral("#ebd1233e")) &&
                gridAppearance.axisYColor == QColor(QStringLiteral("#eb73be0e")) &&
                gridAppearance.axisZColor == QColor(QStringLiteral("#eb1a73d1")))) {
        // Migrate only complete known shipped palettes; custom colors survive.
        gridAppearance.gridColor = BlenderGridAppearance{}.gridColor;
        gridAppearance.emphasisColor = BlenderGridAppearance{}.emphasisColor;
        gridAppearance.axisXColor = BlenderGridAppearance{}.axisXColor;
        gridAppearance.axisYColor = BlenderGridAppearance{}.axisYColor;
        gridAppearance.axisZColor = BlenderGridAppearance{}.axisZColor;
    }
    dialog.gridAppearance = gridAppearance;

    ViewportCameraPreferences camera;
    camera.focalLengthMillimeters = settings.value(
        QStringLiteral("viewport/focalLengthMillimeters"),
        camera.focalLengthMillimeters).toDouble();
    camera.clipStart = settings.value(QStringLiteral("viewport/clipStart"),
                                      camera.clipStart).toDouble();
    camera.clipEnd = settings.value(QStringLiteral("viewport/clipEnd"),
                                    camera.clipEnd).toDouble();
    dialog.cameraPreferences = camera;

    ViewportNavigationPreferences navigation;
    navigation.autoPerspective = settings.value(
        QStringLiteral("navigation/autoPerspective"), navigation.autoPerspective).toBool();
    navigation.zoomToMouse = settings.value(
        QStringLiteral("navigation/zoomToMouse"), navigation.zoomToMouse).toBool();
    navigation.orbitAroundActive = settings.value(
        QStringLiteral("navigation/orbitAroundActive"), navigation.orbitAroundActive).toBool();
    navigation.useMouseDepthNavigate = settings.value(
        QStringLiteral("navigation/useMouseDepthNavigate"),
        navigation.useMouseDepthNavigate).toBool();
    navigation.turntableSensitivityRadiansPerPixel = settings.value(
        QStringLiteral("navigation/turntableSensitivityRadiansPerPixel"),
        navigation.turntableSensitivityRadiansPerPixel).toDouble();
    navigation.orbitMethod = settings.value(QStringLiteral("navigation/orbitMethod"), 0).toInt() == 1
                                  ? ViewportOrbitMethod::Trackball
                                  : ViewportOrbitMethod::Turntable;
    navigation.trackballSensitivity = settings.value(
        QStringLiteral("navigation/trackballSensitivity"),
        navigation.trackballSensitivity).toDouble();
    navigation.invertMouseZoom = settings.value(
        QStringLiteral("navigation/invertMouseZoom"), navigation.invertMouseZoom).toBool();
    navigation.invertZoomWheel = settings.value(
        QStringLiteral("navigation/invertZoomWheel"), navigation.invertZoomWheel).toBool();
    navigation.zoomMethod = settings.value(QStringLiteral("navigation/zoomMethod"), 0).toInt() == 1
                                ? ViewportZoomMethod::Scale
                                : ViewportZoomMethod::Dolly;
    navigation.zoomAxis = settings.value(QStringLiteral("navigation/zoomAxis"), 0).toInt() == 1
                              ? ViewportZoomAxis::Horizontal
                              : ViewportZoomAxis::Vertical;
    dialog.navigationPreferences = navigation;

    RotateToolPreferences rotate;
    rotate.angleSnapIncrementDegrees = settings.value(
        QStringLiteral("rotate/angleSnapIncrementDegrees"),
        rotate.angleSnapIncrementDegrees).toDouble();
    rotate.angleSnapIncrementRadiansDegrees = settings.value(
        QStringLiteral("rotate/angleSnapIncrementRadiansDegrees"),
        rotate.angleSnapIncrementRadiansDegrees).toDouble();
    rotate.angleSnapStrengthDegrees = settings.value(
        QStringLiteral("rotate/angleSnapStrengthDegrees"),
        rotate.angleSnapStrengthDegrees).toDouble();
    rotate.angleSnapEnabled = settings.value(
        QStringLiteral("rotate/angleSnapEnabled"), rotate.angleSnapEnabled).toBool();
    rotate.useRadians = settings.value(QStringLiteral("rotate/useRadians"),
                                       rotate.useRadians).toBool();
    dialog.rotateToolPreferences = rotate;

    const int requestedAaSamples = settings.value(
        QStringLiteral("system/viewportAaSamples"), 8).toInt();
    dialog.viewportAaSamples = requestedAaSamples == 2 || requestedAaSamples == 4 ||
                                       requestedAaSamples == 8
                                   ? requestedAaSamples
                                   : 0;

    stored.orthoEnabled =
        settings.value(QStringLiteral("modeling/orthoEnabled"), false).toBool();
    stored.osnapEnabled = settings.value(QStringLiteral("osnap/enabled"), false).toBool();
    stored.endpointSnapEnabled =
        settings.value(QStringLiteral("osnap/endpoint"), true).toBool();
    stored.midpointSnapEnabled =
        settings.value(QStringLiteral("osnap/midpoint"), true).toBool();
    stored.intersectionSnapEnabled =
        settings.value(QStringLiteral("osnap/intersection"), true).toBool();
    stored.centerSnapEnabled =
        settings.value(QStringLiteral("osnap/center"), true).toBool();
    stored.perpendicularSnapEnabled =
        settings.value(QStringLiteral("osnap/perpendicular"), false).toBool();
    stored.tangentSnapEnabled =
        settings.value(QStringLiteral("osnap/tangent"), false).toBool();
    stored.nearSnapEnabled = settings.value(QStringLiteral("osnap/near"), false).toBool();
    stored.controlPointSnapEnabled =
        settings.value(QStringLiteral("osnap/controlPoint"), false).toBool();
    stored.controlPointsVisible =
        settings.value(QStringLiteral("view/controlPoints"), false).toBool();
    return stored;
}

void savePanButtonPreference(Qt::MouseButton button)
{
    writeSettings([button](QSettings &settings) {
        settings.setValue(QStringLiteral("keymap/panButton"),
                          button == Qt::RightButton ? QStringLiteral("right")
                                                    : QStringLiteral("middle"));
    });
}

void saveSnapLabelsPreference(bool visible)
{
    writeSettings([visible](QSettings &settings) {
        settings.setValue(QStringLiteral("viewport/snapLabelsVisible"), visible);
    });
}

void saveSmoothCurveDisplayPreference(bool enabled)
{
    writeSettings([enabled](QSettings &settings) {
        settings.setValue(QStringLiteral("viewport/smoothCurveDisplay"), enabled);
    });
}

void saveArchitecturalDimensionFontPreference(bool enabled)
{
    writeSettings([enabled](QSettings &settings) {
        settings.setValue(QStringLiteral("dimensions/architecturalFont"), enabled);
    });
}

void saveGridAppearancePreference(const BlenderGridAppearance &appearance)
{
    writeSettings([&appearance](QSettings &settings) {
        settings.setValue(QStringLiteral("viewport/gridColor"),
                          appearance.gridColor.name(QColor::HexArgb));
        settings.setValue(QStringLiteral("viewport/gridEmphasisColor"),
                          appearance.emphasisColor.name(QColor::HexArgb));
        settings.setValue(QStringLiteral("viewport/gridAxisXColor"),
                          appearance.axisXColor.name(QColor::HexArgb));
        settings.setValue(QStringLiteral("viewport/gridAxisYColor"),
                          appearance.axisYColor.name(QColor::HexArgb));
        settings.setValue(QStringLiteral("viewport/gridAxisZColor"),
                          appearance.axisZColor.name(QColor::HexArgb));
        settings.setValue(QStringLiteral("viewport/gridOpacity"), appearance.opacity);
        settings.setValue(QStringLiteral("viewport/gridStipple"),
                          appearance.lowAlphaStipple);
    });
}

void saveCameraPreferences(const ViewportCameraPreferences &preferences)
{
    writeSettings([&preferences](QSettings &settings) {
        settings.setValue(QStringLiteral("viewport/focalLengthMillimeters"),
                          preferences.focalLengthMillimeters);
        settings.setValue(QStringLiteral("viewport/clipStart"), preferences.clipStart);
        settings.setValue(QStringLiteral("viewport/clipEnd"), preferences.clipEnd);
    });
}

void saveNavigationPreferences(const ViewportNavigationPreferences &preferences)
{
    writeSettings([&preferences](QSettings &settings) {
        settings.setValue(QStringLiteral("navigation/autoPerspective"),
                          preferences.autoPerspective);
        settings.setValue(QStringLiteral("navigation/zoomToMouse"), preferences.zoomToMouse);
        settings.setValue(QStringLiteral("navigation/orbitAroundActive"),
                          preferences.orbitAroundActive);
        settings.setValue(QStringLiteral("navigation/useMouseDepthNavigate"),
                          preferences.useMouseDepthNavigate);
        settings.setValue(QStringLiteral("navigation/turntableSensitivityRadiansPerPixel"),
                          preferences.turntableSensitivityRadiansPerPixel);
        settings.setValue(QStringLiteral("navigation/orbitMethod"),
                          preferences.orbitMethod == ViewportOrbitMethod::Trackball ? 1 : 0);
        settings.setValue(QStringLiteral("navigation/trackballSensitivity"),
                          preferences.trackballSensitivity);
        settings.setValue(QStringLiteral("navigation/invertMouseZoom"),
                          preferences.invertMouseZoom);
        settings.setValue(QStringLiteral("navigation/invertZoomWheel"),
                          preferences.invertZoomWheel);
        settings.setValue(QStringLiteral("navigation/zoomMethod"),
                          preferences.zoomMethod == ViewportZoomMethod::Scale ? 1 : 0);
        settings.setValue(QStringLiteral("navigation/zoomAxis"),
                          preferences.zoomAxis == ViewportZoomAxis::Horizontal ? 1 : 0);
    });
}

void saveRotateToolPreferences(const RotateToolPreferences &preferences)
{
    writeSettings([&preferences](QSettings &settings) {
        settings.setValue(QStringLiteral("rotate/angleSnapIncrementDegrees"),
                          preferences.angleSnapIncrementDegrees);
        settings.setValue(QStringLiteral("rotate/angleSnapIncrementRadiansDegrees"),
                          preferences.angleSnapIncrementRadiansDegrees);
        settings.setValue(QStringLiteral("rotate/angleSnapStrengthDegrees"),
                          preferences.angleSnapStrengthDegrees);
        settings.setValue(QStringLiteral("rotate/angleSnapEnabled"),
                          preferences.angleSnapEnabled);
        settings.setValue(QStringLiteral("rotate/useRadians"), preferences.useRadians);
    });
}

void saveViewportAaSamplesPreference(int samples)
{
    writeSettings([samples](QSettings &settings) {
        settings.setValue(QStringLiteral("system/viewportAaSamples"), samples);
    });
}

void saveOrthoPreference(bool enabled)
{
    writeSettings([enabled](QSettings &settings) {
        settings.setValue(QStringLiteral("modeling/orthoEnabled"), enabled);
    });
}

void saveOsnapEnabledPreference(bool enabled)
{
    writeSettings([enabled](QSettings &settings) {
        settings.setValue(QStringLiteral("osnap/enabled"), enabled);
    });
}

void saveSnapModesPreference(const StoredPreferences &preferences)
{
    writeSettings([&preferences](QSettings &settings) {
        settings.setValue(QStringLiteral("osnap/endpoint"), preferences.endpointSnapEnabled);
        settings.setValue(QStringLiteral("osnap/midpoint"), preferences.midpointSnapEnabled);
        settings.setValue(QStringLiteral("osnap/intersection"), preferences.intersectionSnapEnabled);
        settings.setValue(QStringLiteral("osnap/center"), preferences.centerSnapEnabled);
        settings.setValue(QStringLiteral("osnap/perpendicular"), preferences.perpendicularSnapEnabled);
        settings.setValue(QStringLiteral("osnap/tangent"), preferences.tangentSnapEnabled);
        settings.setValue(QStringLiteral("osnap/near"), preferences.nearSnapEnabled);
        settings.setValue(QStringLiteral("osnap/controlPoint"), preferences.controlPointSnapEnabled);
    });
}

void saveControlPointsPreference(bool visible)
{
    writeSettings([visible](QSettings &settings) {
        settings.setValue(QStringLiteral("view/controlPoints"), visible);
    });
}

} // namespace classiCAD
