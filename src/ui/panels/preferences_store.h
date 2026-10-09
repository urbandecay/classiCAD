/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "preferences_dialog.h"

namespace classiCAD {

struct StoredPreferences {
    PreferencesDialogValues dialogValues;
    bool orthoEnabled = false;
    bool osnapEnabled = false;
    bool endpointSnapEnabled = true;
    bool midpointSnapEnabled = true;
    bool intersectionSnapEnabled = true;
    bool centerSnapEnabled = true;
    bool perpendicularSnapEnabled = false;
    bool tangentSnapEnabled = false;
    bool nearSnapEnabled = false;
    bool controlPointSnapEnabled = false;
    bool controlPointsVisible = false;
    bool lineAutoWeldEnabled = true;
};

StoredPreferences loadStoredPreferences();

void savePanButtonPreference(Qt::MouseButton button);
void saveSnapLabelsPreference(bool visible);
void saveSmoothCurveDisplayPreference(bool enabled);
void saveArchitecturalDimensionFontPreference(bool enabled);
void saveGridAppearancePreference(const BlenderGridAppearance &appearance);
void saveCameraPreferences(const ViewportCameraPreferences &preferences);
void saveNavigationPreferences(const ViewportNavigationPreferences &preferences);
void saveRotateToolPreferences(const RotateToolPreferences &preferences);
void saveViewportAaSamplesPreference(int samples);
void saveSmoothWirePreferences(bool overlay, bool editMode);
void saveOrthoPreference(bool enabled);
void saveOsnapEnabledPreference(bool enabled);
void saveSnapModesPreference(const StoredPreferences &preferences);
void saveControlPointsPreference(bool visible);
void saveLineAutoWeldPreference(bool enabled);

} // namespace classiCAD
