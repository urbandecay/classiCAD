/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ui/viewport/viewport_raster_workbench_linear.h"
#include "ui/viewport/viewport_render_frame.h"

#include <QVector>

#include <cstddef>

namespace classiCAD {

// One physical target-pixel sample from a scalar volume's Blender slice view.
struct RasterWorkbenchVolumeSliceSample {
    int x = 0;
    int y = 0;
    float cameraDepth = 0.0f;
    float density = 0.0f;
    float sliceStepWorld = 0.0f;
};

// One physically bounded ray-marched scalar-volume contribution per pixel.
struct RasterWorkbenchVolumeRaySample {
    int x = 0;
    int y = 0;
    float cameraDepth = 0.0f;
    RasterWorkbenchLinearPixel premultipliedColor;
};

// Translate a top-left CPU row into Workbench's bottom-left Bayer coordinates.
float rasterWorkbenchVolumeDitherNoise(int x, int y,
                                       int viewportPixelHeight) noexcept;

// Builds samples only for a valid, enabled scalar-volume slice. False means
// unsupported input, a safety limit was exceeded, or the slice projected no
// covered pixels. The output is cleared on failure.
bool rasterWorkbenchVolumeSliceSamples(
    const ViewportRenderObject &object, const ViewportTransform &camera,
    const QSize &viewportSize, qreal devicePixelRatio,
    QVector<RasterWorkbenchVolumeSliceSample> *samples);

// Integrates a ray only up to the nearest ordinary-scene depth. Front-domain
// coverage discards the volume ray, matching Workbench's stencil test.
bool rasterWorkbenchVolumeRaySamples(
    const ViewportRenderObject &object, const ViewportTransform &camera,
    const QSize &viewportSize, qreal devicePixelRatio,
    const QVector<float> &sceneDepth, const QVector<bool> &inFrontCoverage,
    std::size_t maximumIntegrationSamples,
    std::size_t *integratedSampleCount,
    QVector<RasterWorkbenchVolumeRaySample> *samples);

} // namespace classiCAD
