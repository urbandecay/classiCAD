/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_render_frame.h"
#include "viewport_shading.h"
#include "viewport_surface_renderer.h"

namespace classiCAD {

// A scene-color request shared by the interactive viewport and offscreen
// render result. Background/grid drawing, AA sample lifecycle, and CAD
// overlays remain with their callers; the surface renderer owns GL details.
struct ViewportWorkbenchSceneColorPassRequest {
    const ViewportRenderTargets *sharedTargets = nullptr;
    bool sceneLinearOutput = false;
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
        const QVector<ViewportRenderObject> &objects,
        const ViewportTransform &camera,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportWorkbenchSceneColorPassRequest &request);
};

} // namespace classiCAD
