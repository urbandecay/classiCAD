#include "drawing_plane_resolver.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

WorkPlane principalPlaneForView(const DrawingPlaneResolutionRequest &request)
{
    if (request.perspectiveEnabled) {
        return WorkPlane::XY;
    }

    switch (request.viewPreset) {
    case ViewportViewPreset::Top:
    case ViewportViewPreset::Bottom:
        return WorkPlane::XY;
    case ViewportViewPreset::Front:
    case ViewportViewPreset::Back:
        return WorkPlane::XZ;
    case ViewportViewPreset::Right:
    case ViewportViewPreset::Left:
        return WorkPlane::YZ;
    case ViewportViewPreset::Isometric:
    case ViewportViewPreset::Perspective:
    case ViewportViewPreset::Custom:
        break;
    }

    const Point3D normal = request.viewDirection;
    const qreal xAlignment = std::abs(normal.x);
    const qreal yAlignment = std::abs(normal.y);
    const qreal zAlignment = std::abs(normal.z);
    return xAlignment >= yAlignment && xAlignment >= zAlignment
               ? WorkPlane::YZ
               : (yAlignment >= zAlignment ? WorkPlane::XZ : WorkPlane::XY);
}

} // namespace

DrawingPlaneResolution DrawingPlaneResolver::resolve(
    const DrawingPlaneResolutionRequest &request)
{
    if (request.hasHoveredPlane &&
        request.selectingObject &&
        !request.controlPointsVisible) {
        const qreal facing = std::abs(dot(request.hoveredPlane.normal,
                                           request.viewDirection));
        if (facing < 0.15) {
            return {DrawingPlaneResolutionKind::EdgeOnSelectionDrag,
                    WorkPlane::XY,
                    viewAlignedFrame(request)};
        }
    }

    if (request.hasHoveredPlane &&
        (request.selectingObject || request.drawingShape) &&
        (!request.drawingShape || request.hoveredPlaneUsable)) {
        return {DrawingPlaneResolutionKind::HoveredObject,
                WorkPlane::XY,
                request.hoveredPlane};
    }

    if (!request.drawingShape) {
        return {};
    }

    const WorkPlane plane = principalPlaneForView(request);
    if (!request.perspectiveEnabled &&
        (request.viewPreset == ViewportViewPreset::Isometric ||
         request.viewPreset == ViewportViewPreset::Perspective ||
         request.viewPreset == ViewportViewPreset::Custom) &&
        request.activeTool == ToolId::Line) {
        const Point3D normal = request.viewDirection;
        const qreal largestAlignment = std::max({std::abs(normal.x),
                                                 std::abs(normal.y),
                                                 std::abs(normal.z)});
        if (largestAlignment <= 0.99) {
            const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
                {0.0, 0.0, 0.0},
                {-normal.x, -normal.y, -normal.z});
            return {DrawingPlaneResolutionKind::CameraFacingLineFallback,
                    WorkPlane::XY,
                    frame};
        }
    }

    return {DrawingPlaneResolutionKind::PrincipalFallback,
            plane,
            makeWorkPlaneFrame(plane, 0.0)};
}

WorkPlaneFrame DrawingPlaneResolver::viewAlignedFrame(
    const DrawingPlaneResolutionRequest &request)
{
    switch (request.viewPreset) {
    case ViewportViewPreset::Top:
    case ViewportViewPreset::Bottom:
        return makeWorkPlaneFrame(WorkPlane::XY, request.viewTarget.z);
    case ViewportViewPreset::Front:
    case ViewportViewPreset::Back:
        return makeWorkPlaneFrame(WorkPlane::XZ, request.viewTarget.y);
    case ViewportViewPreset::Right:
    case ViewportViewPreset::Left:
        return makeWorkPlaneFrame(WorkPlane::YZ, request.viewTarget.x);
    case ViewportViewPreset::Isometric:
    case ViewportViewPreset::Perspective:
    case ViewportViewPreset::Custom:
        break;
    }

    const Point3D normal = request.viewDirection;
    const Point3D up = request.viewUp;
    const Point3D screenRight{up.y * normal.z - up.z * normal.y,
                              up.z * normal.x - up.x * normal.z,
                              up.x * normal.y - up.y * normal.x};
    return makeWorkPlaneFrameFromNormal(request.viewTarget,
                                        normal,
                                        screenRight);
}

} // namespace classiCAD
