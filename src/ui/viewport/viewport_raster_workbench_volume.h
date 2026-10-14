/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ui/viewport/viewport_raster_workbench_linear.h"
#include "ui/viewport/viewport_render_frame.h"

#include <QVector>

namespace classiCAD {

// One physical target-pixel sample from a scalar volume's Blender slice view.
struct RasterWorkbenchVolumeSliceSample {
    int x = 0;
    int y = 0;
    float cameraDepth = 0.0f;
    float density = 0.0f;
    float sliceStepWorld = 0.0f;
};

// Builds samples only for a valid, enabled scalar-volume slice. False means
// unsupported input, a safety limit was exceeded, or the slice projected no
// covered pixels. The output is cleared on failure.
bool rasterWorkbenchVolumeSliceSamples(
    const ViewportRenderObject &object, const ViewportTransform &camera,
    const QSize &viewportSize, qreal devicePixelRatio,
    QVector<RasterWorkbenchVolumeSliceSample> *samples);

} // namespace classiCAD
