/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_workbench_scene_pass_coordinator.h"

namespace classiCAD {

ViewportWorkbenchSceneColorPassResult
ViewportWorkbenchScenePassCoordinator::renderSceneColor(
    ViewportSurfaceRenderer &surfaceRenderer,
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &camera,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportWorkbenchSceneColorPassRequest &request)
{
    ViewportWorkbenchSceneColorPassResult result;
    result.rendered = surfaceRenderer.draw(
        objects, camera, viewportSize, devicePixelRatio, settings,
        false, true, false, request.sharedTargets, &result.report,
        request.sceneLinearOutput);
    return result;
}

} // namespace classiCAD
