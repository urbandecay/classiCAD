#include "geometry_transform.h"

#include "core/geometry/shape_mapping.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

template <typename Map>
bool transformSpatialGeometry(Shape *shape, const Map &map)
{
    NurbsSurface3D &surface = shapeBaseSurface(*shape);
    if (!validateNurbsSurface(surface)) return false;
    const auto handedness = [](const NurbsExtrusionSolid3D &solid) {
        const auto &cv = solid.baseSurface.controlPoints;
        if (cv.size() < 4) return 0.0;
        const Point3D u{cv[2].x-cv[0].x, cv[2].y-cv[0].y, cv[2].z-cv[0].z};
        const Point3D v{cv[1].x-cv[0].x, cv[1].y-cv[0].y, cv[1].z-cv[0].z};
        const Point3D &d = solid.displacement;
        return (u.y*v.z-u.z*v.y)*d.x + (u.z*v.x-u.x*v.z)*d.y +
               (u.x*v.y-u.y*v.x)*d.z;
    };
    const qreal before = shape->geometryType == GeometryType::NurbsSolid
                             ? handedness(shape->nurbsSolid) : 0.0;
    if (shape->geometryType == GeometryType::NurbsSolid) {
        const Point3D origin = surface.controlPoints.first();
        const Point3D offset = shape->nurbsSolid.displacement;
        const Point3D start = map(origin);
        const Point3D end = map({origin.x + offset.x, origin.y + offset.y,
                                 origin.z + offset.z});
        shape->nurbsSolid.displacement = {end.x-start.x, end.y-start.y, end.z-start.z};
    }
    for (Point3D &point : surface.controlPoints) point = map(point);
    if (shape->geometryType == GeometryType::NurbsSolid) {
        for (NurbsSurface3D &face : shape->nurbsSolid.boundaryFaces)
            for (Point3D &point : face.controlPoints) point = map(point);
        if ((before < 0.0) != (handedness(shape->nurbsSolid) < 0.0)) {
            for (int i = 0; i < shape->nurbsSolid.boundaryFaceReversed.size(); ++i)
                shape->nurbsSolid.boundaryFaceReversed[i] =
                    !shape->nurbsSolid.boundaryFaceReversed[i];
        }
    }
    return true;
}

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

Point3D rotateVector(const Point3D &vector, const Point3D &axis, qreal angle)
{
    const qreal axisLength = std::hypot(std::hypot(axis.x, axis.y), axis.z);
    if (axisLength <= 1.0e-12 || std::abs(angle) <= 1.0e-15) {
        return vector;
    }
    const Point3D unitAxis{axis.x / axisLength,
                           axis.y / axisLength,
                           axis.z / axisLength};
    const qreal cosine = std::cos(angle);
    const qreal sine = std::sin(angle);
    const Point3D cross{unitAxis.y * vector.z - unitAxis.z * vector.y,
                        unitAxis.z * vector.x - unitAxis.x * vector.z,
                        unitAxis.x * vector.y - unitAxis.y * vector.x};
    const qreal dot = unitAxis.x * vector.x + unitAxis.y * vector.y +
                      unitAxis.z * vector.z;
    return {vector.x * cosine + cross.x * sine + unitAxis.x * dot * (1.0 - cosine),
            vector.y * cosine + cross.y * sine + unitAxis.y * dot * (1.0 - cosine),
            vector.z * cosine + cross.z * sine + unitAxis.z * dot * (1.0 - cosine)};
}

Point3D rotatePoint(const Point3D &point,
                    const Point3D &pivot,
                    const Point3D &axis,
                    qreal angle)
{
    const Point3D offset{point.x - pivot.x,
                         point.y - pivot.y,
                         point.z - pivot.z};
    const Point3D rotated = rotateVector(offset, axis, angle);
    return {pivot.x + rotated.x, pivot.y + rotated.y, pivot.z + rotated.z};
}

WorkPlaneFrame rotateFrame(const WorkPlaneFrame &frame,
                           const Point3D &pivot,
                           const Point3D &axis,
                           qreal angle)
{
    WorkPlaneFrame result = frame;
    result.origin = rotatePoint(frame.origin, pivot, axis, angle);
    result.xAxis = rotateVector(frame.xAxis, axis, angle);
    result.yAxis = rotateVector(frame.yAxis, axis, angle);
    result.normal = rotateVector(frame.normal, axis, angle);
    result.valid = isValidWorkPlaneFrame(result);
    return result;
}

} // namespace

bool bakeShapePlacementTranslation(Shape *shape,
                                   const Point3D &translation)
{
    if (shape == nullptr ||
        (shape->geometryType != GeometryType::NurbsSurface &&
         shape->geometryType != GeometryType::NurbsSolid) ||
        !std::isfinite(translation.x) || !std::isfinite(translation.y) ||
        !std::isfinite(translation.z)) {
        return false;
    }
    if (translation.x == 0.0 && translation.y == 0.0 && translation.z == 0.0) {
        return true;
    }
    NurbsSurface3D &surface = shapeBaseSurface(*shape);
    if (!validateNurbsSurface(surface)) {
        return false;
    }
    for (Point3D &point : surface.controlPoints) {
        point.x += translation.x;
        point.y += translation.y;
        point.z += translation.z;
    }
    for (NurbsSurface3D &face : shape->nurbsSolid.boundaryFaces) {
        for (Point3D &point : face.controlPoints) {
            point.x += translation.x;
            point.y += translation.y;
            point.z += translation.z;
        }
    }
    return true;
}

bool translateShapeGeometry(Shape *shape,
                            const QPointF &delta,
                            const WorkPlaneFrame &inputFrame)
{
    if (shape == nullptr || !isValidWorkPlaneFrame(inputFrame)) {
        return false;
    }

    if (shape->geometryType == GeometryType::PolyCurve &&
        shape->componentWorkPlaneFrames.size() == shape->components.size()) {
        const Point3D worldDelta{
            inputFrame.xAxis.x * delta.x() + inputFrame.yAxis.x * delta.y(),
            inputFrame.xAxis.y * delta.x() + inputFrame.yAxis.y * delta.y(),
            inputFrame.xAxis.z * delta.x() + inputFrame.yAxis.z * delta.y()};
        shape->workPlaneFrame.origin.x += worldDelta.x;
        shape->workPlaneFrame.origin.y += worldDelta.y;
        shape->workPlaneFrame.origin.z += worldDelta.z;
        for (WorkPlaneFrame &componentFrame : shape->componentWorkPlaneFrames) {
            componentFrame.origin.x += worldDelta.x;
            componentFrame.origin.y += worldDelta.y;
            componentFrame.origin.z += worldDelta.z;
        }
        return true;
    }

    for (QPointF &point : shape->points) {
        point += delta;
    }
    for (QPointF &point : shape->nurbs.controlPoints) {
        point += delta;
    }
    for (NurbsCurve2D &component : shape->components) {
        for (QPointF &point : component.controlPoints) {
            point += delta;
        }
    }
    if (validateNurbsSurface(shapeBaseSurface(*shape))) {
        const Point3D origin = workPlaneFramePointToWorld({}, inputFrame);
        const Point3D end = workPlaneFramePointToWorld(delta, inputFrame);
        const Point3D worldDelta{end.x - origin.x,
                                 end.y - origin.y,
                                 end.z - origin.z};
        // Translation changes placement only. Recomputing the solid's vector
        // by subtracting translated endpoints introduces cancellation error
        // and makes an unchanged extrusion appear deformed to display caches.
        for (Point3D &point : shapeBaseSurface(*shape).controlPoints) {
            point.x += worldDelta.x;
            point.y += worldDelta.y;
            point.z += worldDelta.z;
        }
        for (NurbsSurface3D &face : shape->nurbsSolid.boundaryFaces) {
            for (Point3D &point : face.controlPoints) {
                point.x += worldDelta.x;
                point.y += worldDelta.y;
                point.z += worldDelta.z;
            }
        }
    }
    return true;
}

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
    if (validateNurbsSurface(shapeBaseSurface(*mirrored))) {
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(*mirrored);
        transformSpatialGeometry(mirrored, [&](Point3D point) {
            const qreal depth = signedDistanceFromWorkPlaneFrame(point, frame);
            const QPointF local = mirrorPointAcrossLine(
                worldPointToWorkPlaneFrame(point, frame), axisStart, unitNormal);
            point = workPlaneFramePointToWorld(local, frame);
            point.x += frame.normal.x * depth;
            point.y += frame.normal.y * depth;
            point.z += frame.normal.z * depth;
            return point;
        });
    }

    // Reflection reverses the orientation of center-defined arcs. The stored
    // NURBS is already transformed above; keep the construction metadata
    // consistent for legacy/fallback paths as well.
    if (mirrored->geometryType == GeometryType::Arc) {
        mirrored->arcSweep = -mirrored->arcSweep;
    }

    return true;
}

bool setClosedNurbsSeamControlPoint(Shape *shape,
                                    int controlPointIndex,
                                    const QPointF &position)
{
    if (shape == nullptr || shape->nurbs.controlPoints.size() < 2) {
        return false;
    }
    const int lastIndex = shape->nurbs.controlPoints.size() - 1;
    if (controlPointIndex != 0 && controlPointIndex != lastIndex) {
        return false;
    }

    const bool alwaysClosedType = shape->geometryType == GeometryType::Circle ||
                                  shape->geometryType == GeometryType::Ellipse;
    const QPointF seamDelta = shape->nurbs.controlPoints.first() -
                              shape->nurbs.controlPoints.last();
    const bool alreadyClosed = QPointF::dotProduct(seamDelta, seamDelta) <= 1.0e-18;
    if (!alwaysClosedType && !alreadyClosed) {
        return false;
    }

    shape->nurbs.controlPoints[0] = position;
    shape->nurbs.controlPoints[lastIndex] = position;
    return true;
}

bool scaleShapeGeometry(Shape *shape,
                        const QPointF &base,
                        const QPointF &axisDirection,
                        qreal factor,
                        bool oneDimensional,
                        const WorkPlaneFrame &surfaceFrame)
{
    if (shape == nullptr) {
        return false;
    }
    if (oneDimensional && shape->geometryType == GeometryType::Rectangle &&
        shape->points.size() == 2) {
        shape->points = rectangleVertices(*shape);
    }

    const auto scaledPoint = [base, axisDirection, factor, oneDimensional](
                                 const QPointF &point) {
        const QPointF offset = point - base;
        if (!oneDimensional) {
            return base + offset * factor;
        }
        const qreal alongAxis = QPointF::dotProduct(offset, axisDirection);
        return base + offset + axisDirection * (alongAxis * (factor - 1.0));
    };
    for (QPointF &point : shape->points) {
        point = scaledPoint(point);
    }
    for (QPointF &point : shape->nurbs.controlPoints) {
        point = scaledPoint(point);
    }
    for (Shape::NurbsCurve2D &component : shape->components) {
        for (QPointF &point : component.controlPoints) {
            point = scaledPoint(point);
        }
    }
    if (validateNurbsSurface(shapeBaseSurface(*shape))) {
        transformSpatialGeometry(shape, [&](Point3D point) {
            qreal depth = signedDistanceFromWorkPlaneFrame(point, surfaceFrame);
            if (shape->geometryType == GeometryType::NurbsSolid && !oneDimensional)
                depth *= factor;
            const QPointF local = scaledPoint(
                worldPointToWorkPlaneFrame(point, surfaceFrame));
            point = workPlaneFramePointToWorld(local, surfaceFrame);
            point.x += surfaceFrame.normal.x * depth;
            point.y += surfaceFrame.normal.y * depth;
            point.z += surfaceFrame.normal.z * depth;
            return point;
        });
    }
    return true;
}

bool rotateShapeGeometry(Shape *shape,
                         const Point3D &pivot,
                         const Point3D &axis,
                         qreal angle)
{
    if (shape == nullptr) {
        return false;
    }
    if (validateNurbsSurface(shapeBaseSurface(*shape))) {
        transformSpatialGeometry(shape, [&](const Point3D &point) {
            return rotatePoint(point, pivot, axis, angle);
        });
        return true;
    }
    WorkPlaneFrame frame = shapeWorkPlaneFrame(*shape);
    frame = rotateFrame(frame, pivot, axis, angle);
    if (!isValidWorkPlaneFrame(frame)) {
        return false;
    }
    shape->workPlaneFrame = frame;
    return true;
}

} // namespace classiCAD
