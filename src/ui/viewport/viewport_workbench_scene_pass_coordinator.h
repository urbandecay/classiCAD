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
    bool smoothWiresOverlay = true;
    qreal themeOutlineWidth = 1.0;

    static ViewportWorkbenchScenePassPacket fromFrame(
        const ViewportRenderFrame &frame,
        const QVector<ViewportRenderObject> &drawObjects,
        const ViewportTransform &sampleCamera,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets *sharedTargets,
        bool sceneLinearOutput,
        bool smoothWiresOverlay = true,
        qreal themeOutlineWidth = 1.0);

    static ViewportWorkbenchScenePassPacket fromScene(
        const QVector<ViewportRenderObject> &objects,
        const ViewportTransform &camera,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets *sharedTargets,
        bool sceneLinearOutput,
        bool smoothWiresOverlay = true,
        qreal themeOutlineWidth = 1.0);

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
    // Shared production entry point for both live viewport frames and the
    // Workbench render result. The caller supplies the active sample camera
    // and target so temporal AA uses the same immutable frame contract.
    static ViewportWorkbenchSceneColorPassResult renderFrameSceneColor(
        ViewportSurfaceRenderer &surfaceRenderer,
        const ViewportRenderFrame &frame,
        const QVector<ViewportRenderObject> &drawObjects,
        const ViewportTransform &sampleCamera,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets *sharedTargets,
        bool sceneLinearOutput,
        bool smoothWiresOverlay = true,
        qreal themeOutlineWidth = 1.0);

    static ViewportWorkbenchSceneColorPassResult renderSceneColor(
        ViewportSurfaceRenderer &surfaceRenderer,
        const ViewportWorkbenchScenePassPacket &packet);
};

} // namespace classiCAD
