#include "geometry_transform.h"

#include <cmath>

namespace classiCAD {
namespace {

QPointF mirrorPointAcrossLine(const QPointF &point,
                              const QPointF &axisStart,
                              const QPointF &unitNormal)
{
    const QPointF offset = point - axisStart;
    const qreal signedDistance = offset.x() * unitNormal.x() +
                                 offset.y() * unitNormal.y();
    return point - unitNormal * (2.0 * signedDistance);
}

QVector<QPointF> rectangleVertices(const Shape &shape)
{
    if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
        return {};
    }
    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

void mirrorCurve(Shape::NurbsCurve2D *curve,
                 const QPointF &axisStart,
                 const QPointF &unitNormal)
{
    if (curve == nullptr) {
        return;
    }
    for (QPointF &controlPoint : curve->controlPoints) {
        controlPoint = mirrorPointAcrossLine(controlPoint, axisStart, unitNormal);
    }
}

} // namespace

bool mirrorShapeAcrossLine(const Shape &source,
                           const QPointF &axisStart,
                           const QPointF &axisEnd,
                           Shape *mirrored)
{
    if (mirrored == nullptr) {
        return false;
    }

    const QPointF axis = axisEnd - axisStart;
    const qreal axisLength = std::hypot(axis.x(), axis.y());
    if (axisLength <= 1.0e-12) {
        return false;
    }

    const QPointF unitNormal(-axis.y() / axisLength, axis.x() / axisLength);
    *mirrored = source;

    // A two-point rectangle is a construction record. Expand it before
    // transforming so the mirrored object retains its complete geometry.
    if (mirrored->geometryType == GeometryType::Rectangle &&
        mirrored->points.size() == 2) {
        mirrored->points = rectangleVertices(*mirrored);
    }
    for (QPointF &point : mirrored->points) {
        point = mirrorPointAcrossLine(point, axisStart, unitNormal);
    }
    mirrorCurve(&mirrored->nurbs, axisStart, unitNormal);
    for (Shape::NurbsCurve2D &component : mirrored->components) {
        mirrorCurve(&component, axisStart, unitNormal);
    }

    // Reflection reverses the orientation of center-defined arcs. The stored
    // NURBS is already transformed above; keep the construction metadata
    // consistent for legacy/fallback paths as well.
    if (mirrored->geometryType == GeometryType::Arc) {
        mirrored->arcSweep = -mirrored->arcSweep;
    }

    return true;
}

} // namespace classiCAD
