/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_render_frame.h"
#include "viewport_shading.h"
#include "viewport_surface_renderer.h"

namespace classiCAD {

// Shared entry point for the Workbench scene-color pass in the interactive
// viewport and offscreen render result. Background/grid drawing, AA sample
// lifecycle, attachment validity, and CAD overlays remain with their callers.
// The surface renderer still owns the individual GPU subpasses.
struct ViewportWorkbenchSceneColorPassResult {
    bool rendered = false;
    quint32 populatedAttachments = 0;
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
        const ViewportRenderTargets *sharedTargets,
        bool sceneLinearOutput);
};

} // namespace classiCAD
