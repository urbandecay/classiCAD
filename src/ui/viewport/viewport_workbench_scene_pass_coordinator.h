/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_render_frame.h"
#include "viewport_shading.h"
#include "viewport_surface_renderer.h"

namespace classiCAD {

// Immutable scene-color inputs for one synchronous GPU sample. The object
// vector is an implicitly-shared snapshot copied from the render frame (or
// its presentation-adjusted draw list). The targets are borrowed from the AA
// renderer and are valid only while that sample is active.
struct ViewportWorkbenchScenePassPacket {
    QVector<ViewportRenderObject> objects;
    ViewportTransform camera;
    QSize viewportSize;
    qreal devicePixelRatio = 1.0;
    ViewportShadingSettings settings;
    const ViewportRenderTargets *sharedTargets = nullptr;
    bool sceneLinearOutput = false;

    static ViewportWorkbenchScenePassPacket fromFrame(
        const ViewportRenderFrame &frame,
        const QVector<ViewportRenderObject> &drawObjects,
        const ViewportTransform &sampleCamera,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets *sharedTargets,
        bool sceneLinearOutput);

    static ViewportWorkbenchScenePassPacket fromScene(
        const QVector<ViewportRenderObject> &objects,
        const ViewportTransform &camera,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets *sharedTargets,
        bool sceneLinearOutput);

    bool isValid() const noexcept;
};

struct ViewportWorkbenchSceneColorPassResult {
    bool rendered = false;
    ViewportRenderPassReport report;

    bool completed(ViewportRenderPass pass) const noexcept
    {
        return report.passCompleted(pass);
    }
};

class ViewportWorkbenchScenePassCoordinator final {
public:
    static ViewportWorkbenchSceneColorPassResult renderSceneColor(
        ViewportSurfaceRenderer &surfaceRenderer,
        const ViewportWorkbenchScenePassPacket &packet);
};

} // namespace classiCAD
