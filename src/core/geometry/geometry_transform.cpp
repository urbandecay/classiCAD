#include "geometry_transform.h"

#include "core/geometry/curve_evaluator.h"
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

void scaleCurveInFrame(Shape::NurbsCurve3D *curve,
                       const WorkPlaneFrame &curveFrame,
                       const QPointF &base,
                       const QPointF &axisDirection,
                       qreal factor,
                       bool oneDimensional,
                       const WorkPlaneFrame &surfaceFrame)
{
    if (curve == nullptr || !validateNurbsCurve(*curve) ||
        !isValidWorkPlaneFrame(curveFrame) ||
        !isValidWorkPlaneFrame(surfaceFrame)) {
        return;
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

    QVector<double> transformedNormalCoordinates;
    transformedNormalCoordinates.reserve(curve->controlPoints.size());
    bool requiresDimension3 = curve->dimension == 3;
    for (int index = 0; index < curve->controlPoints.size(); ++index) {
        const qreal oldNormalCoordinate = curve->dimension == 3
            ? curve->normalCoordinates[index] : 0.0;
        Point3D worldPoint = workPlaneFramePointToWorld(
            curve->controlPoints[index], oldNormalCoordinate, curveFrame);
        qreal depth = 0.0;
        const QPointF surfacePoint =
            worldPointToWorkPlaneFrame(worldPoint, surfaceFrame, &depth);
        const QPointF transformedSurfacePoint = scaledPoint(surfacePoint);
        if (!oneDimensional) {
            depth *= factor;
        }
        worldPoint = workPlaneFramePointToWorld(transformedSurfacePoint,
                                                depth,
                                                surfaceFrame);
        qreal newNormalCoordinate = 0.0;
        curve->controlPoints[index] = worldPointToWorkPlaneFrame(
            worldPoint, curveFrame, &newNormalCoordinate);
        transformedNormalCoordinates.append(newNormalCoordinate);
        requiresDimension3 = requiresDimension3 ||
                             std::abs(newNormalCoordinate) > 1.0e-9;
    }

    if (requiresDimension3) {
        curve->dimension = 3;
        curve->normalCoordinates = std::move(transformedNormalCoordinates);
    } else {
        curve->dimension = 2;
        curve->normalCoordinates.clear();
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
    if (shape == nullptr || !std::isfinite(translation.x) ||
        !std::isfinite(translation.y) ||
        !std::isfinite(translation.z)) {
        return false;
    }
    if (translation.x == 0.0 && translation.y == 0.0 && translation.z == 0.0) {
        return true;
    }
    if (shape->geometryType == GeometryType::NurbsSurface ||
        shape->geometryType == GeometryType::NurbsSolid) {
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

    WorkPlaneFrame frame = shapeWorkPlaneFrame(*shape);
    frame.origin.x += translation.x;
    frame.origin.y += translation.y;
    frame.origin.z += translation.z;
    if (!isValidWorkPlaneFrame(frame)) {
        return false;
    }
    shape->workPlaneFrame = frame;
    if (shape->componentWorkPlaneFrames.size() == shape->components.size()) {
        for (WorkPlaneFrame &componentFrame : shape->componentWorkPlaneFrames) {
            componentFrame.origin.x += translation.x;
            componentFrame.origin.y += translation.y;
            componentFrame.origin.z += translation.z;
            if (!isValidWorkPlaneFrame(componentFrame)) {
                return false;
            }
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
    const Point3D worldOrigin = workPlaneFramePointToWorld({}, inputFrame);
    const Point3D worldEnd = workPlaneFramePointToWorld(delta, inputFrame);
    const Point3D worldDelta{worldEnd.x - worldOrigin.x,
                             worldEnd.y - worldOrigin.y,
                             worldEnd.z - worldOrigin.z};
    const auto translateCurve = [&worldDelta](Shape::NurbsCurve3D *curve,
                                               const WorkPlaneFrame &frame) {
        if (curve == nullptr || !validateNurbsCurve(*curve)) {
            return;
        }
        for (int index = 0; index < curve->controlPoints.size(); ++index) {
            const qreal normalCoordinate = curve->dimension == 3
                ? curve->normalCoordinates[index] : 0.0;
            Point3D world = workPlaneFramePointToWorld(
                curve->controlPoints[index], normalCoordinate, frame);
            world.x += worldDelta.x;
            world.y += worldDelta.y;
            world.z += worldDelta.z;
            qreal newNormalCoordinate = 0.0;
            curve->controlPoints[index] = worldPointToWorkPlaneFrame(
                world, frame, &newNormalCoordinate);
            if (curve->dimension == 2 &&
                std::abs(newNormalCoordinate) > 1.0e-9) {
                curve->dimension = 3;
                curve->normalCoordinates.fill(0.0,
                                             curve->controlPoints.size());
            }
            if (curve->dimension == 3) {
                curve->normalCoordinates[index] = newNormalCoordinate;
            }
        }
    };
    translateCurve(&shape->nurbs, shapeWorkPlaneFrame(*shape));
    for (int index = 0; index < shape->components.size(); ++index) {
        translateCurve(&shape->components[index],
                       shapeComponentWorkPlaneFrame(*shape, index));
    }
    if (validateNurbsSurface(shapeBaseSurface(*shape))) {
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
    if (shape == nullptr || !validateNurbsCurve(shape->nurbs) ||
        shape->nurbs.controlPoints.size() < 2) {
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
    const qreal seamNormalDelta = shape->nurbs.dimension == 3
        ? shape->nurbs.normalCoordinates.first() -
              shape->nurbs.normalCoordinates.last()
        : 0.0;
    const bool alreadyClosed =
        QPointF::dotProduct(seamDelta, seamDelta) +
                seamNormalDelta * seamNormalDelta <= 1.0e-18;
    if (!alwaysClosedType && !alreadyClosed) {
        return false;
    }

    shape->nurbs.controlPoints[0] = position;
    shape->nurbs.controlPoints[lastIndex] = position;
    if (shape->nurbs.dimension == 3) {
        const qreal seamNormalCoordinate =
            controlPointIndex == 0
                ? shape->nurbs.normalCoordinates.first()
                : shape->nurbs.normalCoordinates.last();
        shape->nurbs.normalCoordinates[0] = seamNormalCoordinate;
        shape->nurbs.normalCoordinates[lastIndex] = seamNormalCoordinate;
    }
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
    scaleCurveInFrame(&shape->nurbs,
                      shapeWorkPlaneFrame(*shape),
                      base,
                      axisDirection,
                      factor,
                      oneDimensional,
                      surfaceFrame);
    for (int index = 0; index < shape->components.size(); ++index) {
        scaleCurveInFrame(&shape->components[index],
                          shapeComponentWorkPlaneFrame(*shape, index),
                          base,
                          axisDirection,
                          factor,
                          oneDimensional,
                          surfaceFrame);
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
    if (shape->componentWorkPlaneFrames.size() == shape->components.size()) {
        for (const WorkPlaneFrame &componentFrame :
             shape->componentWorkPlaneFrames) {
            if (!isValidWorkPlaneFrame(componentFrame)) {
                return false;
            }
        }
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
    if (shape->componentWorkPlaneFrames.size() == shape->components.size()) {
        for (WorkPlaneFrame &componentFrame : shape->componentWorkPlaneFrames) {
            componentFrame = rotateFrame(componentFrame, pivot, axis, angle);
            if (!isValidWorkPlaneFrame(componentFrame)) {
                return false;
            }
        }
    }
    return true;
}

Point3D rotatePointAboutAxis(const Point3D &point,
                             const Point3D &pivot,
                             const Point3D &axis,
                             qreal angle)
{
    return rotatePoint(point, pivot, axis, angle);
}

Point3D scalePointInFrame(const Point3D &point,
                          const QPointF &base,
                          const QPointF &axisDirection,
                          qreal factor,
                          bool oneDimensional,
                          const WorkPlaneFrame &surfaceFrame)
{
    if (!isValidWorkPlaneFrame(surfaceFrame)) {
        return point;
    }
    const QPointF local = worldPointToWorkPlaneFrame(point, surfaceFrame);
    const QPointF offset = local - base;
    QPointF transformedLocal;
    qreal depth = signedDistanceFromWorkPlaneFrame(point, surfaceFrame);
    if (oneDimensional) {
        const qreal alongAxis = QPointF::dotProduct(offset, axisDirection);
        transformedLocal = base + offset +
                           axisDirection * (alongAxis * (factor - 1.0));
    } else {
        transformedLocal = base + offset * factor;
        depth *= factor;
    }
    Point3D result = workPlaneFramePointToWorld(transformedLocal, surfaceFrame);
    result.x += surfaceFrame.normal.x * depth;
    result.y += surfaceFrame.normal.y * depth;
    result.z += surfaceFrame.normal.z * depth;
    return result;
}

bool transformShapeControlPoints(
    Shape *shape,
    const QSet<int> &controlPointIndices,
    const std::function<Point3D(const Point3D &)> &transform)
{
    if (shape == nullptr || controlPointIndices.isEmpty() || !transform) {
        return false;
    }

    const auto close = [](const Point3D &first, const Point3D &second) {
        const qreal dx = first.x - second.x;
        const qreal dy = first.y - second.y;
        const qreal dz = first.z - second.z;
        return dx * dx + dy * dy + dz * dz <= 1.0e-16;
    };
    const auto finite = [](const Point3D &point) {
        return std::isfinite(point.x) && std::isfinite(point.y) &&
               std::isfinite(point.z);
    };

    if (shape->geometryType == GeometryType::Point) {
        if (!controlPointIndices.contains(0) || shape->points.size() != 1) {
            return false;
        }
        WorkPlaneFrame frame = shapeWorkPlaneFrame(*shape);
        if (!isValidWorkPlaneFrame(frame)) {
            return false;
        }
        const QPointF localPoint = shape->points.first();
        const Point3D worldPoint = shapePointToWorld(*shape, localPoint);
        const Point3D moved = transform(worldPoint);
        if (!finite(moved)) {
            return false;
        }
        const Point3D localOffset{
            frame.xAxis.x * localPoint.x() + frame.yAxis.x * localPoint.y(),
            frame.xAxis.y * localPoint.x() + frame.yAxis.y * localPoint.y(),
            frame.xAxis.z * localPoint.x() + frame.yAxis.z * localPoint.y()};
        frame.origin = {moved.x - localOffset.x,
                        moved.y - localOffset.y,
                        moved.z - localOffset.z};
        if (!isValidWorkPlaneFrame(frame)) {
            return false;
        }
        shape->workPlaneFrame = frame;
        return true;
    }

    if (shape->geometryType == GeometryType::NurbsSurface) {
        if (!validateNurbsSurface(shape->nurbsSurface)) {
            return false;
        }
        bool changed = false;
        for (int index = 0; index < shape->nurbsSurface.controlPoints.size(); ++index) {
            if (!controlPointIndices.contains(index)) {
                continue;
            }
            const Point3D moved = transform(
                shape->nurbsSurface.controlPoints[index]);
            if (!finite(moved)) {
                return false;
            }
            shape->nurbsSurface.controlPoints[index] = moved;
            changed = true;
        }
        return changed && validateNurbsSurface(shape->nurbsSurface);
    }

    if (shape->geometryType == GeometryType::NurbsSolid) {
        const QVector<NurbsSurface3D> visibleFaces = shapeSurfaceFaces(*shape);
        QVector<Point3D> selectedPositions;
        QVector<Point3D> uniquePositions;
        for (const NurbsSurface3D &face : visibleFaces) {
            for (const Point3D &point : face.controlPoints) {
                int pointIndex = -1;
                for (int index = 0; index < uniquePositions.size(); ++index) {
                    if (close(uniquePositions[index], point)) {
                        pointIndex = index;
                        break;
                    }
                }
                if (pointIndex < 0) {
                    pointIndex = uniquePositions.size();
                    uniquePositions.append(point);
                }
                if (controlPointIndices.contains(pointIndex) &&
                    !std::any_of(selectedPositions.cbegin(),
                                 selectedPositions.cend(),
                                 [&point, &close](const Point3D &existing) {
                                     return close(existing, point);
                                 })) {
                    selectedPositions.append(point);
                }
            }
        }
        if (selectedPositions.isEmpty() ||
            !materializeNurbsSolidBoundary(&shape->nurbsSolid)) {
            return false;
        }
        for (NurbsSurface3D &face : shape->nurbsSolid.boundaryFaces) {
            for (Point3D &point : face.controlPoints) {
                const bool selected = std::any_of(
                    selectedPositions.cbegin(), selectedPositions.cend(),
                    [&point, &close](const Point3D &candidate) {
                        return close(point, candidate);
                    });
                if (!selected) {
                    continue;
                }
                const Point3D moved = transform(point);
                if (!finite(moved)) {
                    return false;
                }
                point = moved;
            }
        }
        return validateNurbsSolid(shape->nurbsSolid);
    }

    QSet<int> indicesToTransform = controlPointIndices;
    for (const int index : controlPointIndices) {
        if (index >= 0 && index < shape->controlPointWeldGroups.size()) {
            const quint64 group = shape->controlPointWeldGroups[index];
            if (group == 0) {
                continue;
            }
            for (int candidate = 0;
                 candidate < shape->controlPointWeldGroups.size(); ++candidate) {
                if (shape->controlPointWeldGroups[candidate] == group) {
                    indicesToTransform.insert(candidate);
                }
            }
        }
    }

    bool changed = false;
    const auto transformCurve = [&](NurbsCurve3D *curve,
                                    const WorkPlaneFrame &frame,
                                    int firstGlobalIndex) {
        if (curve == nullptr || !validateNurbsCurve(*curve) ||
            !isValidWorkPlaneFrame(frame)) {
            return false;
        }
        const int lastGlobalIndex =
            firstGlobalIndex + curve->controlPoints.size() - 1;
        const bool rectangleHiddenClosure =
            shape->geometryType == GeometryType::Rectangle &&
            firstGlobalIndex == 0 && curve->degree == 1 &&
            curve->controlPoints.size() == 5;
        if (rectangleHiddenClosure) {
            // Rectangle tools expose four handles but the clamped closed
            // degree-one curve stores CV0 again as CV4. Treat this structural
            // alias as one handle, even if a prior edit left the seam open.
            bool repairedClosure =
                curve->controlPoints.last() != curve->controlPoints.first();
            if (curve->dimension == 3 &&
                curve->normalCoordinates.size() == 5) {
                repairedClosure = repairedClosure ||
                    curve->normalCoordinates.last() !=
                        curve->normalCoordinates.first();
            }
            if (curve->rational && curve->weights.size() == 5) {
                repairedClosure = repairedClosure ||
                    curve->weights.last() != curve->weights.first();
            }
            curve->controlPoints.last() = curve->controlPoints.first();
            if (curve->dimension == 3 &&
                curve->normalCoordinates.size() == 5) {
                curve->normalCoordinates.last() =
                    curve->normalCoordinates.first();
            }
            if (curve->rational && curve->weights.size() == 5) {
                curve->weights.last() = curve->weights.first();
            }
            if (indicesToTransform.contains(firstGlobalIndex) ||
                indicesToTransform.contains(lastGlobalIndex)) {
                indicesToTransform.insert(firstGlobalIndex);
                indicesToTransform.insert(lastGlobalIndex);
            }
            changed = changed || repairedClosure;
        }
        if (curve->controlPoints.size() > 1) {
            const qreal firstNormal = curve->dimension == 3
                ? curve->normalCoordinates.first() : 0.0;
            const qreal lastNormal = curve->dimension == 3
                ? curve->normalCoordinates.last() : 0.0;
            const Point3D firstWorld = workPlaneFramePointToWorld(
                curve->controlPoints.first(), firstNormal, frame);
            const Point3D lastWorld = workPlaneFramePointToWorld(
                curve->controlPoints.last(), lastNormal, frame);
            if (close(firstWorld, lastWorld) &&
                (indicesToTransform.contains(firstGlobalIndex) ||
                 indicesToTransform.contains(lastGlobalIndex))) {
                indicesToTransform.insert(firstGlobalIndex);
                indicesToTransform.insert(lastGlobalIndex);
            }
        }

        for (int index = 0; index < curve->controlPoints.size(); ++index) {
            if (!indicesToTransform.contains(firstGlobalIndex + index)) {
                continue;
            }
            const qreal normal = curve->dimension == 3
                ? curve->normalCoordinates[index] : 0.0;
            const Point3D moved = transform(workPlaneFramePointToWorld(
                curve->controlPoints[index], normal, frame));
            if (!finite(moved)) {
                return false;
            }
            qreal newNormal = 0.0;
            curve->controlPoints[index] = worldPointToWorkPlaneFrame(
                moved, frame, &newNormal);
            if (curve->dimension == 2 && std::abs(newNormal) > 1.0e-9) {
                curve->dimension = 3;
                curve->normalCoordinates.fill(0.0,
                                              curve->controlPoints.size());
            }
            if (curve->dimension == 3) {
                curve->normalCoordinates[index] = newNormal;
            }
            changed = true;
        }
        return true;
    };

    int globalIndex = 0;
    if (shape->geometryType == GeometryType::PolyCurve &&
        !shape->components.isEmpty()) {
        for (int componentIndex = 0;
             componentIndex < shape->components.size(); ++componentIndex) {
            NurbsCurve3D &component = shape->components[componentIndex];
            if (!transformCurve(&component,
                                shapeComponentWorkPlaneFrame(*shape,
                                                             componentIndex),
                                globalIndex)) {
                return false;
            }
            globalIndex += component.controlPoints.size();
        }
        if (changed) {
            shape->points.clear();
            for (int index = 0; index < shape->components.size(); ++index) {
                QPointF start;
                QPointF end;
                if (!nurbsCurveEndpoints(shape->components[index],
                                         &start, &end)) {
                    continue;
                }
                if (index == 0) {
                    shape->points.append(start);
                }
                shape->points.append(end);
            }
        }
        return changed;
    }

    if (!validateNurbsCurve(shape->nurbs)) {
        return false;
    }
    if (!transformCurve(&shape->nurbs, shapeWorkPlaneFrame(*shape), 0) ||
        !changed) {
        return false;
    }
    shape->points = shape->nurbs.controlPoints;
    return true;
}

} // namespace classiCAD
