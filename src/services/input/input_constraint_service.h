#pragma once

#include "core/geometry/work_plane.h"

#include <QPointF>
#include <QSize>

namespace classiCAD {

class ViewportTransform;

// Shared axis and normal constraint math. Tools retain modifier/state policy;
// this service handles projection, screen-axis inference, and ray placement.
class InputConstraintService final {
public:
    static Point3D worldAxisDirection(int key);
    static Point3D inferProjectedWorldAxis(
        const Point3D &reference,
        const QPointF &screenPosition,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal toleranceDegrees = 6.0);
    static bool worldPointOnScreenAxis(
        const QPointF &screenPosition,
        const QSize &viewportSize,
        const Point3D &origin,
        const Point3D &direction,
        const ViewportTransform &transform,
        Point3D *worldPoint);
    static Point3D projectOntoWorldAxis(const Point3D &point,
                                       const Point3D &origin,
                                       const Point3D &direction);
    static QPointF nearestPlanarAxis(const QPointF &point,
                                     const QPointF &origin);
};

} // namespace classiCAD
