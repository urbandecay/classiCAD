/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_workbench_scene_pass_coordinator.h"

#include <cmath>

namespace classiCAD {

ViewportWorkbenchScenePassPacket
ViewportWorkbenchScenePassPacket::fromFrame(
    const ViewportRenderFrame &frame,
    const QVector<ViewportRenderObject> &drawObjects,
    const ViewportTransform &sampleCamera,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportRenderTargets *sharedTargets,
    bool sceneLinearOutput,
    bool smoothWiresOverlay)
{
    return fromScene(drawObjects, sampleCamera, frame.viewportSize,
                     devicePixelRatio, settings, sharedTargets,
                     sceneLinearOutput, smoothWiresOverlay);
}

ViewportWorkbenchScenePassPacket
ViewportWorkbenchScenePassPacket::fromScene(
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &camera,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportRenderTargets *sharedTargets,
    bool sceneLinearOutput,
    bool smoothWiresOverlay)
{
    ViewportWorkbenchScenePassPacket packet;
    packet.objects = objects;
    packet.camera = camera;
    packet.viewportSize = viewportSize;
    packet.devicePixelRatio = devicePixelRatio;
    packet.settings = settings;
    packet.sharedTargets = sharedTargets;
    packet.sceneLinearOutput = sceneLinearOutput;
    packet.smoothWiresOverlay = smoothWiresOverlay;
    return packet;
}

bool ViewportWorkbenchScenePassPacket::isValid() const noexcept
{
    if (viewportSize.isEmpty() || !std::isfinite(devicePixelRatio) ||
        devicePixelRatio <= 0.0) {
        return false;
    }
    if (sharedTargets == nullptr) return true;
    const QSize expectedPixelSize(
        qRound(viewportSize.width() * devicePixelRatio),
        qRound(viewportSize.height() * devicePixelRatio));
    return !expectedPixelSize.isEmpty() &&
           sharedTargets->pixelSize == expectedPixelSize;
}

ViewportWorkbenchSceneColorPassResult
ViewportWorkbenchScenePassCoordinator::renderSceneColor(
    ViewportSurfaceRenderer &surfaceRenderer,
    const ViewportWorkbenchScenePassPacket &packet)
{
    ViewportWorkbenchSceneColorPassResult result;
    if (!packet.isValid()) return result;
    result.rendered = surfaceRenderer.draw(
        packet.objects, packet.camera, packet.viewportSize,
        packet.devicePixelRatio, packet.settings,
        false, true, false, packet.sharedTargets, &result.report,
        packet.sceneLinearOutput, packet.smoothWiresOverlay);
    return result;
}

ViewportWorkbenchSceneColorPassResult
ViewportWorkbenchScenePassCoordinator::renderFrameSceneColor(
    ViewportSurfaceRenderer &surfaceRenderer,
    const ViewportRenderFrame &frame,
    const QVector<ViewportRenderObject> &drawObjects,
    const ViewportTransform &sampleCamera,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportRenderTargets *sharedTargets,
    bool sceneLinearOutput,
    bool smoothWiresOverlay)
{
    const ViewportWorkbenchScenePassPacket packet =
        ViewportWorkbenchScenePassPacket::fromFrame(
            frame, drawObjects, sampleCamera, devicePixelRatio, settings,
            sharedTargets, sceneLinearOutput, smoothWiresOverlay);
    return renderSceneColor(surfaceRenderer, packet);
}

} // namespace classiCAD
