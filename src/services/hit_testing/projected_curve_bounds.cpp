#include "projected_curve_bounds.h"

#include "core/geometry/nurbs_curve.h"
#include "core/geometry/work_plane.h"
#include "services/viewport/viewport_transform.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

bool projectedNurbsControlHullBounds(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    QRectF *screenBounds,
    const Point3D &worldOffset)
{
    if (screenBounds == nullptr || !validateNurbsCurve(curve) ||
        !isValidWorkPlaneFrame(workPlaneFrame) || curve.controlPoints.isEmpty()) {
        return false;
    }

    // Positive NURBS weights keep the curve inside its Euclidean control
    // hull. For a perspective camera, projecting all hull vertices in front
    // of the clip planes preserves that screen-space containment.
    qreal minX = 0.0;
    qreal maxX = 0.0;
    qreal minY = 0.0;
    qreal maxY = 0.0;
    bool initialized = false;
    WorkPlaneFrame placedFrame = workPlaneFrame;
    placedFrame.origin.x += worldOffset.x;
    placedFrame.origin.y += worldOffset.y;
    placedFrame.origin.z += worldOffset.z;
    for (int index = 0; index < curve.controlPoints.size(); ++index) {
        const QPointF &controlPoint = curve.controlPoints[index];
        const qreal normalCoordinate = curve.dimension == 3
            ? curve.normalCoordinates[index] : 0.0;
        QPointF screenPoint;
        const Point3D worldPoint = workPlaneFramePointToWorld(
            controlPoint, normalCoordinate, placedFrame);
        if (!viewportTransform.worldPointToScreen(worldPoint,
                                                  viewportSize,
                                                  &screenPoint)) {
            return false;
        }
        if (!initialized) {
            minX = maxX = screenPoint.x();
            minY = maxY = screenPoint.y();
            initialized = true;
        } else {
            minX = std::min(minX, screenPoint.x());
            maxX = std::max(maxX, screenPoint.x());
            minY = std::min(minY, screenPoint.y());
            maxY = std::max(maxY, screenPoint.y());
        }
    }

    if (!initialized) {
        return false;
    }
    *screenBounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
    return true;
}

bool screenBoundsOverlap(const QRectF &first, const QRectF &second)
{
    if (!std::isfinite(first.left()) || !std::isfinite(first.top()) ||
        !std::isfinite(first.right()) || !std::isfinite(first.bottom()) ||
        !std::isfinite(second.left()) || !std::isfinite(second.top()) ||
        !std::isfinite(second.right()) || !std::isfinite(second.bottom())) {
        return true;
    }
    const QRectF a = first.normalized();
    const QRectF b = second.normalized();
    return a.left() <= b.right() && a.right() >= b.left() &&
           a.top() <= b.bottom() && a.bottom() >= b.top();
}

} // namespace classiCAD
