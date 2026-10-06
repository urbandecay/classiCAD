/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ui/viewport_widget_api.h"

namespace classiCAD {

struct PreferencesDialogValues {
    Qt::MouseButton panButton = Qt::MiddleButton;
    bool snapLabelsVisible = true;
    bool smoothCurveDisplay = true;
    bool architecturalDimensionFont = false;
    BlenderGridAppearance gridAppearance;
    ViewportCameraPreferences cameraPreferences;
    ViewportNavigationPreferences navigationPreferences;
    RotateToolPreferences rotateToolPreferences;
    int viewportAaSamples = 0;
    bool smoothWiresOverlay = true;
    bool smoothWiresEditMode = true;
};

// Returns true when the user accepts the dialog and writes the selected values.
bool showPreferencesDialog(QWidget *parent,
                           const PreferencesDialogValues &initialValues,
                           PreferencesDialogValues *selectedValues);

} // namespace classiCAD
