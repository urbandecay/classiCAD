/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "blender_grid_frame.h"
#include "blender_grid_scale.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D add(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D multiply(const Point3D &point, qreal scalar)
{
    return {point.x * scalar, point.y * scalar, point.z * scalar};
}

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

WorkPlane planeForAxisView(ViewportViewPreset preset)
{
    switch (preset) {
    case ViewportViewPreset::Front:
    case ViewportViewPreset::Back:
        return WorkPlane::XZ;
    case ViewportViewPreset::Right:
    case ViewportViewPreset::Left:
        return WorkPlane::YZ;
    case ViewportViewPreset::Top:
    case ViewportViewPreset::Bottom:
    case ViewportViewPreset::Isometric:
    case ViewportViewPreset::Perspective:
    case ViewportViewPreset::Custom:
        return WorkPlane::XY;
    }
    return WorkPlane::XY;
}

void setVisiblePlaneAxes(BlenderGridFrame *frame)
{
    frame->visibleAxes = {{false, false, false}};
    switch (frame->plane) {
    case WorkPlane::XY:
        frame->visibleAxes[0] = true;
        frame->visibleAxes[1] = true;
        break;
    case WorkPlane::XZ:
        frame->visibleAxes[0] = true;
        // Blender hides the Z axis by default (V3D_SHOW_Z is not enabled).
        break;
    case WorkPlane::YZ:
        frame->visibleAxes[1] = true;
        break;
    }
}

} // namespace

BlenderGridFrame resolveBlenderGridFrame(const ViewportTransform &transform,
                                         const QSize &viewportSize)
{
    BlenderGridFrame frame;
    const ViewportCameraState camera = transform.cameraState();
    frame.fixedAxisOrthographic =
        !camera.perspective && isBlenderAxisAlignedView(camera.preset);
    frame.plane = frame.fixedAxisOrthographic
                      ? planeForAxisView(camera.preset)
                      : WorkPlane::XY;
    frame.planeOffset = 0.0;
    setVisiblePlaneAxes(&frame);
    const Point3D planeOrigin = workPlanePointToWorld({}, frame.plane,
                                                       frame.planeOffset);

    if (camera.perspective) {
        const Point3D cameraPosition = transform.cameraPosition(viewportSize);
        const Point3D target = transform.viewTarget();
        const Point3D cameraOut = transform.viewDirection();
        const Point3D planeNormal = workPlaneNormal(frame.plane);
        const qreal cameraDistance = std::sqrt(dot(
            subtract(cameraPosition, target),
            subtract(cameraPosition, target)));
        const qreal cameraHeight = dot(subtract(cameraPosition, planeOrigin),
                                       planeNormal);
        const qreal forwardAgainstPlane = dot(cameraOut, planeNormal);
        const qreal absoluteForward = std::clamp(std::abs(forwardAgainstPlane),
                                                 0.0,
                                                 1.0);
        const qreal rayToPlaneDistance = absoluteForward > 1.0e-8
                                             ? std::abs(cameraHeight /
                                                        forwardAgainstPlane)
                                             : std::abs(cameraHeight);
        const qreal focusDistance = rayToPlaneDistance +
            (std::abs(cameraHeight) - rayToPlaneDistance) *
                (1.0 - absoluteForward);
        // Blender uses the unbounded camera-to-floor estimate for level
        // selection; the former clamp caused the grid to fade too early.
        frame.focusDistance = std::isfinite(focusDistance)
                                  ? std::max<qreal>(focusDistance, 0.001)
                                  : cameraDistance;
        const Point3D gridFocus = subtract(cameraPosition,
                                           multiply(cameraOut,
                                                    frame.focusDistance));
        frame.cameraRelativeOffset = worldPointToWorkPlane(gridFocus,
                                                            frame.plane);
    } else {
        // Find the work-plane point under the orthographic view center with
        // an infinite camera ray. screenToWorkPlane() applies scene clip
        // planes, which must not affect grid placement.
        const Point3D target = transform.viewTarget();
        const Point3D rayDirection = multiply(transform.viewDirection(), -1.0);
        const Point3D planePoint = workPlanePointToWorld(
            {}, frame.plane, frame.planeOffset);
        const Point3D planeNormal = workPlaneNormal(frame.plane);
        const qreal denominator = dot(planeNormal, rayDirection);
        if (std::abs(denominator) > 1.0e-8) {
            const qreal distance = dot(planeNormal,
                                       subtract(planePoint, target)) /
                                   denominator;
            const Point3D centerOnPlane = add(
                target, multiply(rayDirection, distance));
            frame.cameraRelativeOffset = worldPointToWorkPlane(
                centerOnPlane, frame.plane);
        } else {
            frame.cameraRelativeOffset = worldPointToWorkPlane(
                target, frame.plane);
        }
        // Orthographic zoom updates gridViewDistance independently of the
        // scene clip range; use it for LOD selection in oblique views.
        frame.focusDistance = camera.gridViewDistance;
    }

    if (!std::isfinite(frame.focusDistance) || frame.focusDistance <= 0.0) {
        frame.focusDistance = 1.0;
    }
    if (!std::isfinite(frame.cameraRelativeOffset.x()) ||
        !std::isfinite(frame.cameraRelativeOffset.y())) {
        frame.cameraRelativeOffset = worldPointToWorkPlane(transform.viewTarget(),
                                                           frame.plane);
    }
    return frame;
}

} // namespace classiCAD
