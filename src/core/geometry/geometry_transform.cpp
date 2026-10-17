#include "geometry_transform.h"

#include "core/geometry/curve_evaluator.h"
#include "core/geometry/shape_mapping.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

Point3D cross(const Point3D &left, const Point3D &right)
{
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

qreal length(const Point3D &vector)
{
    return std::hypot(std::hypot(vector.x, vector.y), vector.z);
}

bool transformedWorkPlaneFrame(const WorkPlaneFrame &source,
                               const AffineTransform3D &transform,
                               WorkPlaneFrame *result)
{
    if (result == nullptr || !isValidWorkPlaneFrame(source)) return false;
    const Point3D transformedX = transformAffineVector(transform, source.xAxis);
    const Point3D transformedY = transformAffineVector(transform, source.yAxis);
    const qreal xLength = length(transformedX);
    Point3D transformedNormal = cross(transformedX, transformedY);
    const qreal normalLength = length(transformedNormal);
    if (!std::isfinite(xLength) || !std::isfinite(normalLength) ||
        xLength <= 1.0e-12 || normalLength <= 1.0e-12) {
        return false;
    }
    WorkPlaneFrame frame;
    frame.origin = transformAffinePoint(transform, source.origin);
    frame.xAxis = {transformedX.x / xLength,
                   transformedX.y / xLength,
                   transformedX.z / xLength};
    transformedNormal = {transformedNormal.x / normalLength,
                         transformedNormal.y / normalLength,
                         transformedNormal.z / normalLength};
    frame.normal = transformedNormal;
    frame.yAxis = cross(frame.normal, frame.xAxis);
    frame.valid = isValidWorkPlaneFrame(frame);
    if (!frame.valid) return false;
    *result = frame;
    return true;
}

bool transformCurveAffine(NurbsCurve3D *curve,
                          const WorkPlaneFrame &sourceFrame,
                          const AffineTransform3D &transform,
                          const WorkPlaneFrame &targetFrame)
{
    if (curve == nullptr || !validateNurbsCurve(*curve)) return false;
    bool requiresDimension3 = curve->dimension == 3;
    QVector<double> normalCoordinates;
    normalCoordinates.reserve(curve->controlPoints.size());
    for (int index = 0; index < curve->controlPoints.size(); ++index) {
        const qreal normal = curve->dimension == 3
            ? curve->normalCoordinates[index] : 0.0;
        const Point3D sourceWorld = workPlaneFramePointToWorld(
            curve->controlPoints[index], normal, sourceFrame);
        const Point3D targetWorld = transformAffinePoint(transform, sourceWorld);
        qreal targetNormal = 0.0;
        const QPointF targetLocal = worldPointToWorkPlaneFrame(
            targetWorld, targetFrame, &targetNormal);
        if (!std::isfinite(targetLocal.x()) ||
            !std::isfinite(targetLocal.y()) ||
            !std::isfinite(targetNormal)) {
            return false;
        }
        const qreal scale = std::max<qreal>(
            1.0, length(targetWorld));
        requiresDimension3 = requiresDimension3 ||
                             std::abs(targetNormal) > scale * 1.0e-10;
        curve->controlPoints[index] = targetLocal;
        normalCoordinates.append(targetNormal);
    }
    if (requiresDimension3) {
        curve->dimension = 3;
        curve->normalCoordinates = std::move(normalCoordinates);
    } else {
        curve->dimension = 2;
        curve->normalCoordinates.clear();
    }
    return validateNurbsCurve(*curve);
}

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

bool transformShapeGeometryAffine(Shape *shape,
                                  const AffineTransform3D &transform)
{
    if (shape == nullptr || !isValidAffineTransform(transform, true)) {
        return false;
    }
    Shape result = *shape;
    const auto transformPointCloud = [&transform](PointCloud3D *cloud) {
        if (cloud == nullptr || !validatePointCloud(*cloud)) return false;
        const double determinant =
            transform[0] * (transform[5] * transform[10] -
                            transform[6] * transform[9]) -
            transform[1] * (transform[4] * transform[10] -
                            transform[6] * transform[8]) +
            transform[2] * (transform[4] * transform[9] -
                            transform[5] * transform[8]);
        const qreal radiusScale = std::cbrt(std::abs(determinant));
        for (Point3D &point : cloud->positions) {
            point = transformAffinePoint(transform, point);
        }
        for (qreal &radius : cloud->radii) radius *= radiusScale;
        return validatePointCloud(*cloud);
    };

    if (result.geometryType == GeometryType::PolygonMesh) {
        return false;
    }
    if (result.geometryType == GeometryType::PointCloud) {
        if (!transformPointCloud(&result.pointCloud)) return false;
        *shape = std::move(result);
        return true;
    }
    if (result.geometryType == GeometryType::ScalarVolume) {
        ScalarVolume3D &volume = result.scalarVolume;
        if (!validateScalarVolume(volume)) return false;
        volume.objectToWorld = multiplyAffineTransforms(
            transform, volume.objectToWorld);
        if (!isValidAffineTransform(volume.objectToWorld, true) ||
            !scalarVolumeDensityScale(volume.renderSpace,
                                      volume.objectToWorld,
                                      volume.displayDensity,
                                      &volume.densityScale) ||
            !validateScalarVolume(volume)) {
            return false;
        }
        *shape = std::move(result);
        return true;
    }

    const auto determinant = [&transform]() {
        return transform[0] * (transform[5] * transform[10] -
                                transform[6] * transform[9]) -
               transform[1] * (transform[4] * transform[10] -
                                transform[6] * transform[8]) +
               transform[2] * (transform[4] * transform[9] -
                                transform[5] * transform[8]);
    };
    if (result.geometryType == GeometryType::NurbsSurface ||
        result.geometryType == GeometryType::NurbsSolid) {
        NurbsSurface3D &baseSurface = shapeBaseSurface(result);
        if (!validateNurbsSurface(baseSurface)) return false;
        for (Point3D &point : baseSurface.controlPoints) {
            point = transformAffinePoint(transform, point);
        }
        if (result.geometryType == GeometryType::NurbsSolid) {
            NurbsExtrusionSolid3D &solid = result.nurbsSolid;
            solid.displacement = transformAffineVector(
                transform, solid.displacement);
            for (NurbsSurface3D &face : solid.boundaryFaces) {
                if (!validateNurbsSurface(face)) return false;
                for (Point3D &point : face.controlPoints) {
                    point = transformAffinePoint(transform, point);
                }
            }
            if (determinant() < 0.0) {
                for (int index = 0;
                     index < solid.boundaryFaceReversed.size(); ++index) {
                    solid.boundaryFaceReversed[index] =
                        !solid.boundaryFaceReversed[index];
                }
            }
        }
        if (!validateNurbsSurface(baseSurface)) return false;
        *shape = std::move(result);
        return true;
    }

    const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(result);
    WorkPlaneFrame targetFrame;
    if (!transformedWorkPlaneFrame(sourceFrame, transform, &targetFrame)) {
        return false;
    }
    for (QPointF &point : result.points) {
        const Point3D sourceWorld = workPlaneFramePointToWorld(point,
                                                               sourceFrame);
        qreal depth = 0.0;
        point = worldPointToWorkPlaneFrame(
            transformAffinePoint(transform, sourceWorld), targetFrame, &depth);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            std::abs(depth) > std::max<qreal>(1.0, length(sourceWorld)) *
                                  1.0e-9) {
            return false;
        }
    }
    if (result.geometryType == GeometryType::PolyCurve &&
        !result.components.isEmpty()) {
        QVector<WorkPlaneFrame> targetComponentFrames;
        targetComponentFrames.reserve(result.components.size());
        for (int index = 0; index < result.components.size(); ++index) {
            const WorkPlaneFrame sourceComponentFrame =
                shapeComponentWorkPlaneFrame(*shape, index);
            WorkPlaneFrame targetComponentFrame;
            if (!transformedWorkPlaneFrame(sourceComponentFrame, transform,
                                           &targetComponentFrame) ||
                !transformCurveAffine(&result.components[index],
                                      sourceComponentFrame, transform,
                                      targetComponentFrame)) {
                return false;
            }
            targetComponentFrames.append(targetComponentFrame);
        }
        result.componentWorkPlaneFrames = std::move(targetComponentFrames);
    } else if (validateNurbsCurve(result.nurbs)) {
        if (!transformCurveAffine(&result.nurbs, sourceFrame, transform,
                                  targetFrame)) {
            return false;
        }
        result.points = result.nurbs.controlPoints;
    }
    result.workPlaneFrame = targetFrame;
    *shape = std::move(result);
    return true;
}

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
    if (shape->geometryType == GeometryType::PointCloud) {
        if (!validatePointCloud(shape->pointCloud)) return false;
        for (Point3D &point : shape->pointCloud.positions) {
            point.x += translation.x;
            point.y += translation.y;
            point.z += translation.z;
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
    if (shape->geometryType == GeometryType::PointCloud) {
        if (!validatePointCloud(shape->pointCloud)) return false;
        for (Point3D &point : shape->pointCloud.positions) {
            point.x += worldDelta.x;
            point.y += worldDelta.y;
            point.z += worldDelta.z;
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
    if (mirrored->geometryType == GeometryType::PointCloud) {
        if (!validatePointCloud(mirrored->pointCloud)) return false;
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(*mirrored);
        for (Point3D &point : mirrored->pointCloud.positions) {
            const qreal depth = signedDistanceFromWorkPlaneFrame(point, frame);
            const QPointF local = mirrorPointAcrossLine(
                worldPointToWorkPlaneFrame(point, frame), axisStart, unitNormal);
            point = workPlaneFramePointToWorld(local, frame);
            point.x += frame.normal.x * depth;
            point.y += frame.normal.y * depth;
            point.z += frame.normal.z * depth;
        }
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
    if (shape->geometryType == GeometryType::PointCloud) {
        if (!validatePointCloud(shape->pointCloud) || !std::isfinite(factor) ||
            !isValidWorkPlaneFrame(surfaceFrame)) return false;
        for (Point3D &point : shape->pointCloud.positions) {
            point = scalePointInFrame(point, base, axisDirection, factor,
                                      oneDimensional, surfaceFrame);
        }
        const qreal radiusScale = std::abs(factor);
        for (qreal &radius : shape->pointCloud.radii) radius *= radiusScale;
        return validatePointCloud(shape->pointCloud);
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
    if (shape->geometryType == GeometryType::PointCloud) {
        if (!validatePointCloud(shape->pointCloud)) return false;
        for (Point3D &point : shape->pointCloud.positions)
            point = rotatePoint(point, pivot, axis, angle);
        return true;
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

Point3D mirrorPointAcrossWorkPlaneLine(const Point3D &point,
                                       const QPointF &axisStart,
                                       const QPointF &axisEnd,
                                       const WorkPlaneFrame &axisFrame)
{
    if (!isValidWorkPlaneFrame(axisFrame)) {
        return point;
    }
    const QPointF axis = axisEnd - axisStart;
    const qreal axisLength = std::hypot(axis.x(), axis.y());
    if (!std::isfinite(axisLength) || axisLength <= 1.0e-12) {
        return point;
    }

    const QPointF unitNormal(-axis.y() / axisLength,
                             axis.x() / axisLength);
    const QPointF local = worldPointToWorkPlaneFrame(point, axisFrame);
    const qreal depth = signedDistanceFromWorkPlaneFrame(point, axisFrame);
    const qreal signedDistance =
        QPointF::dotProduct(local - axisStart, unitNormal);
    const QPointF mirroredLocal = local - unitNormal * (2.0 * signedDistance);
    Point3D mirrored = workPlaneFramePointToWorld(mirroredLocal, axisFrame);
    mirrored.x += axisFrame.normal.x * depth;
    mirrored.y += axisFrame.normal.y * depth;
    mirrored.z += axisFrame.normal.z * depth;
    return mirrored;
}

Point3D mirrorPointAcrossGlobalAxisPlane(const Point3D &point,
                                         const Point3D &pivot,
                                         int axis) noexcept
{
    switch (axis) {
    case 0:
        return {2.0 * pivot.x - point.x, point.y, point.z};
    case 1:
        return {point.x, 2.0 * pivot.y - point.y, point.z};
    case 2:
        return {point.x, point.y, 2.0 * pivot.z - point.z};
    default:
        return point;
    }
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
    if (shape->geometryType == GeometryType::NurbsSolid) {
        // NurbsExtrusionSolid3D is authoritative as baseSurface plus a sweep
        // vector. Its faces are derived; control-point transforms must not
        // turn those derived faces into editable boundary topology.
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

    if (shape->geometryType == GeometryType::PointCloud) {
        if (!validatePointCloud(shape->pointCloud)) return false;
        bool changed = false;
        for (int index = 0; index < shape->pointCloud.positions.size(); ++index) {
            if (!controlPointIndices.contains(index)) continue;
            const Point3D moved = transform(shape->pointCloud.positions[index]);
            if (!finite(moved)) return false;
            shape->pointCloud.positions[index] = moved;
            changed = true;
        }
        return changed && validatePointCloud(shape->pointCloud);
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
