#include "curve_hit_tester.h"

#include "core/document/document.h"
#include "core/geometry/shape_mapping.h"

#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "services/dimensions/dimension_layout.h"
#include "services/hit_testing/projected_curve_bounds.h"
#include "services/viewport/viewport_transform.h"

#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

bool shapeHasCurveControlHull(const Shape &shape)
{
    switch (shape.geometryType) {
    case GeometryType::Line:
    case GeometryType::Arc:
    case GeometryType::Bezier:
    case GeometryType::Nurbs:
    case GeometryType::Rectangle:
    case GeometryType::Circle:
    case GeometryType::PolyCurve:
    case GeometryType::Ellipse:
    case GeometryType::Polygon:
        return true;
    default:
        return false;
    }
}

void includeScreenBounds(const QRectF &candidate,
                        QRectF *bounds,
                        bool *initialized)
{
    if (bounds == nullptr || initialized == nullptr) {
        return;
    }
    const QRectF normalized = candidate.normalized();
    if (!*initialized) {
        *bounds = normalized;
        *initialized = true;
        return;
    }
    *bounds = QRectF(QPointF(std::min(bounds->left(), normalized.left()),
                             std::min(bounds->top(), normalized.top())),
                     QPointF(std::max(bounds->right(), normalized.right()),
                             std::max(bounds->bottom(), normalized.bottom())));
}

bool projectedNurbsSurfaceControlHullBounds(
    const NurbsSurface3D &surface,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const Point3D &worldOffset,
    QRectF *bounds)
{
    if (bounds == nullptr || !validateNurbsSurface(surface) ||
        surface.controlPoints.isEmpty()) {
        return false;
    }
    bool initialized = false;
    for (const Point3D &sourcePoint : surface.controlPoints) {
        const Point3D point{sourcePoint.x + worldOffset.x,
                            sourcePoint.y + worldOffset.y,
                            sourcePoint.z + worldOffset.z};
        QPointF screenPoint;
        if (!transform.worldPointToScreenUnclipped(point,
                                                   viewportSize,
                                                   &screenPoint)) {
            return false;
        }
        includeScreenBounds(QRectF(screenPoint, QSizeF(0.0, 0.0)),
                            bounds,
                            &initialized);
    }
    return initialized;
}

bool screenPointOnSurfaceTriangle(const QPointF &screenPosition,
                                  const PreparedNurbsSurfaceTessellation &mesh,
                                  const PreparedNurbsSurfaceTessellation::Triangle &triangle,
                                  const Point3D &worldOffset,
                                  const ViewportTransform &transform,
                                  const QSize &viewportSize,
                                  Point3D *worldPoint,
                                  Point3D *worldNormal = nullptr)
{
    if (worldPoint == nullptr) {
        return false;
    }

    const auto placedVertex = [&](int vertexIndex, Point3D *point) {
        if (point == nullptr || vertexIndex < 0 ||
            vertexIndex >= mesh.vertices().size()) {
            return false;
        }
        const Point3D &source = mesh.vertices()[vertexIndex];
        *point = {source.x + worldOffset.x,
                  source.y + worldOffset.y,
                  source.z + worldOffset.z};
        return true;
    };

    Point3D a;
    Point3D b;
    Point3D c;
    if (!placedVertex(triangle[0], &a) || !placedVertex(triangle[1], &b) ||
        !placedVertex(triangle[2], &c)) {
        return false;
    }

    const Point3D ab{b.x - a.x, b.y - a.y, b.z - a.z};
    const Point3D ac{c.x - a.x, c.y - a.y, c.z - a.z};
    const qreal abLength = std::sqrt(ab.x * ab.x + ab.y * ab.y + ab.z * ab.z);
    if (!std::isfinite(abLength) || abLength <= 1.0e-12) {
        return false;
    }
    const Point3D xAxis{ab.x / abLength, ab.y / abLength, ab.z / abLength};
    Point3D normal{ab.y * ac.z - ab.z * ac.y,
                   ab.z * ac.x - ab.x * ac.z,
                   ab.x * ac.y - ab.y * ac.x};
    const qreal normalLength = std::sqrt(normal.x * normal.x +
                                         normal.y * normal.y +
                                         normal.z * normal.z);
    if (!std::isfinite(normalLength) || normalLength <= 1.0e-12) {
        return false;
    }
    normal = {normal.x / normalLength, normal.y / normalLength,
              normal.z / normalLength};
    if (worldNormal != nullptr) {
        *worldNormal = normal;
    }
    const Point3D yAxis{normal.y * xAxis.z - normal.z * xAxis.y,
                        normal.z * xAxis.x - normal.x * xAxis.z,
                        normal.x * xAxis.y - normal.y * xAxis.x};

    const qreal bX = abLength;
    const qreal bY = 0.0;
    const qreal cX = ac.x * xAxis.x + ac.y * xAxis.y + ac.z * xAxis.z;
    const qreal cY = ac.x * yAxis.x + ac.y * yAxis.y + ac.z * yAxis.z;
    const qreal denominator = bX * cY - bY * cX;
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-12) {
        return false;
    }

    WorkPlaneFrame frame;
    frame.origin = a;
    frame.xAxis = xAxis;
    frame.yAxis = yAxis;
    frame.normal = normal;
    frame.valid = true;
    QPointF cursorOnPlane;
    if (!transform.screenToWorkPlane(screenPosition, viewportSize, frame,
                                     &cursorOnPlane)) {
        return false;
    }

    const qreal beta = (cursorOnPlane.x() * cY - cursorOnPlane.y() * cX) /
                       denominator;
    const qreal gamma = (bX * cursorOnPlane.y() - bY * cursorOnPlane.x()) /
                        denominator;
    const qreal alpha = 1.0 - beta - gamma;
    constexpr qreal barycentricTolerance = 1.0e-8;
    if (alpha < -barycentricTolerance || beta < -barycentricTolerance ||
        gamma < -barycentricTolerance) {
        return false;
    }

    *worldPoint = workPlaneFramePointToWorld(cursorOnPlane, frame);
    return true;
}

} // namespace

CurveHitTester::CurveHitTester(
    const SurfaceTessellationCache *surfaceTessellationCache)
    : surfaceTessellationCache_(surfaceTessellationCache)
{
}

void CurveHitTester::setSurfaceTessellationCache(
    const SurfaceTessellationCache *surfaceTessellationCache)
{
    surfaceTessellationCache_ = surfaceTessellationCache;
}

void CurveHitTester::setArchitecturalDimensionFont(bool enabled)
{
    architecturalDimensionFont_ = enabled;
}

qreal CurveHitTester::distanceToSegment(const QPointF &point,
                                        const QPointF &start,
                                        const QPointF &end) const
{
    const QPointF direction = end - start;
    const QPointF fromStart = point - start;
    const qreal lengthSquared = QPointF::dotProduct(direction, direction);
    if (lengthSquared <= 1.0e-12) {
        return std::hypot(point.x() - start.x(), point.y() - start.y());
    }

    const qreal projection = std::clamp(
        QPointF::dotProduct(fromStart, direction) / lengthSquared,
        0.0,
        1.0);
    const QPointF closest = start + direction * projection;
    return std::hypot(point.x() - closest.x(), point.y() - closest.y());
}

QVector<QPointF> CurveHitTester::rectangleVertices(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::Rectangle &&
        validateNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() == 5) {
        return shape.nurbs.controlPoints.mid(0, 4);
    }
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

qreal CurveHitTester::distanceToNurbsCurve(
    const QPointF &screenPosition,
    const Shape::NurbsCurve2D &curve,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const WorkPlaneFrame *frameOverride,
    const Point3D &worldOffset) const
{
    const WorkPlaneFrame &frame = frameOverride != nullptr
                                      ? *frameOverride
                                      : transform.workPlaneFrame();
    if (!validateNurbsCurve(curve) || !isValidWorkPlaneFrame(frame)) {
        return 1.0e9;
    }

    qreal firstParameter = 0.0;
    qreal lastParameter = 0.0;
    if (!nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
        return 1.0e9;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }

    const int sampleCount = std::max(64, nonZeroSpans * 32);
    Point3D previousLocal;
    if (!evaluateNurbsPoint3D(curve, firstParameter, &previousLocal)) {
        return 1.0e9;
    }

    qreal closestDistance = 1.0e9;
    const auto project = [&](const Point3D &local, QPointF *screen) {
        Point3D world = workPlaneFramePointToWorld(
            {local.x, local.y}, local.z, frame);
        world.x += worldOffset.x;
        world.y += worldOffset.y;
        world.z += worldOffset.z;
        return transform.worldPointToScreenUnclipped(world, viewportSize, screen);
    };
    QPointF previous;
    if (!project(previousLocal, &previous)) {
        return 1.0e9;
    }
    for (int sample = 1; sample <= sampleCount; ++sample) {
        const qreal fraction = static_cast<qreal>(sample) / sampleCount;
        const qreal parameter = firstParameter +
                                (lastParameter - firstParameter) * fraction;
        Point3D currentLocal;
        QPointF current;
        if (!evaluateNurbsPoint3D(curve, parameter, &currentLocal) ||
            !project(currentLocal, &current)) {
            continue;
        }
        closestDistance = std::min(closestDistance,
                                   distanceToSegment(screenPosition,
                                                    previous,
                                                    current));
        previous = current;
    }
    return closestDistance;
}

bool CurveHitTester::makeCircularArcGeometry(
    const QPointF &startWorld,
    const QPointF &endWorld,
    const QPointF &throughWorld,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    QPointF *center,
    qreal *radius,
    qreal *startAngle,
    qreal *sweepAngle) const
{
    const QPointF start = transform.worldToScreen(startWorld, viewportSize);
    const QPointF end = transform.worldToScreen(endWorld, viewportSize);
    const QPointF through = transform.worldToScreen(throughWorld, viewportSize);
    const qreal startSquared = QPointF::dotProduct(start, start);
    const qreal endSquared = QPointF::dotProduct(end, end);
    const qreal throughSquared = QPointF::dotProduct(through, through);
    const qreal denominator = 2.0 *
        (start.x() * (end.y() - through.y()) +
         end.x() * (through.y() - start.y()) +
         through.x() * (start.y() - end.y()));
    if (std::abs(denominator) < 1.0e-9) {
        return false;
    }

    const QPointF circleCenter(
        (startSquared * (end.y() - through.y()) +
         endSquared * (through.y() - start.y()) +
         throughSquared * (start.y() - end.y())) / denominator,
        (startSquared * (through.x() - end.x()) +
         endSquared * (start.x() - through.x()) +
         throughSquared * (end.x() - start.x())) / denominator);
    const qreal circleRadius = std::hypot(start.x() - circleCenter.x(),
                                          start.y() - circleCenter.y());
    if (circleRadius <= 1.0e-9) {
        return false;
    }

    constexpr qreal twoPi = 6.28318530717958647692;
    const auto normalizeAngle = [twoPi](qreal angle) {
        angle = std::fmod(angle, twoPi);
        if (angle < 0.0) {
            angle += twoPi;
        }
        return angle;
    };

    const qreal first = std::atan2(start.y() - circleCenter.y(),
                                   start.x() - circleCenter.x());
    const qreal second = std::atan2(end.y() - circleCenter.y(),
                                    end.x() - circleCenter.x());
    const qreal throughAngle = std::atan2(through.y() - circleCenter.y(),
                                          through.x() - circleCenter.x());
    const qreal counterClockwiseSweep = normalizeAngle(second - first);
    const qreal throughSweep = normalizeAngle(throughAngle - first);
    if (counterClockwiseSweep <= 1.0e-9) {
        return false;
    }

    const qreal selectedSweep = throughSweep <= counterClockwiseSweep + 1.0e-7
                                    ? counterClockwiseSweep
                                    : -(twoPi - counterClockwiseSweep);
    if (center != nullptr) {
        *center = circleCenter;
    }
    if (radius != nullptr) {
        *radius = circleRadius;
    }
    if (startAngle != nullptr) {
        *startAngle = first;
    }
    if (sweepAngle != nullptr) {
        *sweepAngle = selectedSweep;
    }
    return true;
}

bool CurveHitTester::arcAngleIsOnSweep(qreal startAngle,
                                       qreal sweepAngle,
                                       qreal angle) const
{
    constexpr qreal twoPi = 6.28318530717958647692;
    constexpr qreal epsilon = 1.0e-7;
    if (std::abs(sweepAngle) >= twoPi - epsilon) {
        return true;
    }

    const auto positiveAngle = [twoPi](qreal value) {
        value = std::fmod(value, twoPi);
        return value < 0.0 ? value + twoPi : value;
    };
    if (sweepAngle >= 0.0) {
        return positiveAngle(angle - startAngle) <= sweepAngle + epsilon;
    }
    return positiveAngle(startAngle - angle) <= -sweepAngle + epsilon;
}

qreal CurveHitTester::distanceToArc(const QPointF &screenPosition,
                                    const Shape &shape,
                                    const ViewportTransform &transform,
                                    const QSize &viewportSize) const
{
    if (shape.points.size() < 3) {
        return 1.0e9;
    }

    QPointF center;
    qreal radius = 0.0;
    qreal startAngle = 0.0;
    qreal sweepAngle = 0.0;
    if (shape.arcMode != ArcMode::OnePoint) {
        if (!makeCircularArcGeometry(shape.points[0],
                                     shape.points[1],
                                     shape.points[2],
                                     transform,
                                     viewportSize,
                                     &center,
                                     &radius,
                                     &startAngle,
                                     &sweepAngle)) {
            return 1.0e9;
        }
    } else {
        center = transform.worldToScreen(shape.points[0], viewportSize);
        const QPointF start = transform.worldToScreen(shape.points[1], viewportSize);
        const QPointF end = transform.worldToScreen(shape.points[2], viewportSize);
        radius = std::hypot(start.x() - center.x(), start.y() - center.y());
        if (radius <= 1.0e-9) {
            return 1.0e9;
        }
        startAngle = std::atan2(start.y() - center.y(), start.x() - center.x());
        sweepAngle = shape.arcSweep;
        if (std::abs(sweepAngle) <= 1.0e-9) {
            const qreal endAngle = std::atan2(end.y() - center.y(), end.x() - center.x());
            sweepAngle = endAngle - startAngle;
            constexpr qreal pi = 3.14159265358979323846;
            if (sweepAngle > pi) {
                sweepAngle -= 2.0 * pi;
            } else if (sweepAngle < -pi) {
                sweepAngle += 2.0 * pi;
            }
        }
    }

    const QPointF fromCenter = screenPosition - center;
    const qreal distanceFromCenter = std::hypot(fromCenter.x(), fromCenter.y());
    if (arcAngleIsOnSweep(startAngle,
                          sweepAngle,
                          std::atan2(fromCenter.y(), fromCenter.x()))) {
        return std::abs(distanceFromCenter - radius);
    }

    const QPointF startPoint = center +
                                QPointF(radius * std::cos(startAngle),
                                        radius * std::sin(startAngle));
    const QPointF endPoint = center +
                              QPointF(radius * std::cos(startAngle + sweepAngle),
                                      radius * std::sin(startAngle + sweepAngle));
    return std::min(distanceToSegment(screenPosition, startPoint, startPoint),
                    distanceToSegment(screenPosition, endPoint, endPoint));
}

qreal CurveHitTester::distanceToCubicCurve(const QPointF &screenPosition,
                                           const Shape &shape,
                                           const ViewportTransform &transform,
                                           const QSize &viewportSize) const
{
    if (shape.points.size() < 4) {
        return 1.0e9;
    }

    const QPointF first = transform.worldToScreen(shape.points[0], viewportSize);
    const QPointF second = transform.worldToScreen(shape.points[1], viewportSize);
    const QPointF third = transform.worldToScreen(shape.points[2], viewportSize);
    const QPointF fourth = transform.worldToScreen(shape.points[3], viewportSize);
    constexpr int sampleCount = 64;
    qreal closestDistance = 1.0e9;
    QPointF previous = first;
    for (int sample = 1; sample <= sampleCount; ++sample) {
        const qreal t = static_cast<qreal>(sample) / sampleCount;
        const qreal inverse = 1.0 - t;
        const QPointF current =
            first * (inverse * inverse * inverse) +
            second * (3.0 * inverse * inverse * t) +
            third * (3.0 * inverse * t * t) +
            fourth * (t * t * t);
        closestDistance = std::min(closestDistance,
                                   distanceToSegment(screenPosition,
                                                    previous,
                                                    current));
        previous = current;
    }
    return closestDistance;
}

qreal CurveHitTester::distanceToShape(const QPointF &screenPosition,
                                      const Shape &shape,
                                      const ViewportTransform &transform,
                                      const QSize &viewportSize,
                                      ObjectId objectId,
                                      quint64 geometryRevision,
                                      const Point3D &worldOffset) const
{
    const WorkPlaneFrame shapeFrame = shapeWorkPlaneFrame(shape);
    if (shape.geometryType == GeometryType::NurbsSolid) {
        qreal closest = 1.0e9;
        const auto faces = shapeSurfaceFaces(shape);
        for (int index = 0; index < faces.size(); ++index) {
            QRectF bounds;
            if (projectedNurbsSurfaceControlHullBounds(faces[index], transform,
                                                       viewportSize, worldOffset,
                                                       &bounds) &&
                !bounds.adjusted(-12, -12, 12, 12).contains(screenPosition)) continue;
            closest = std::min(closest, distanceToNurbsSurface(
                screenPosition, faces[index], transform, viewportSize,
                objectId, geometryRevision, index, worldOffset));
        }
        return closest;
    }
    if (shape.geometryType == GeometryType::NurbsSurface) {
        QRectF bounds;
        if (projectedNurbsSurfaceControlHullBounds(
                shape.nurbsSurface, transform, viewportSize, worldOffset,
                &bounds) &&
            !bounds.adjusted(-12, -12, 12, 12).contains(screenPosition)) {
            return 1.0e9;
        }
        return distanceToNurbsSurface(screenPosition,
                                      shape.nurbsSurface,
                                      transform,
                                      viewportSize,
                                      objectId,
                                      geometryRevision,
                                      0,
                                      worldOffset);
    }
    if (isDimensionGeometryType(shape.geometryType)) {
        return distanceToDimensionLayout(
            screenPosition,
            buildDimensionScreenLayout(
                shape,
                transform,
                viewportSize,
                architecturalDimensionFont_ ? DimensionFontStyle::Architectural
                                            : DimensionFontStyle::Standard));
    }
    if (shape.geometryType == GeometryType::PolyCurve && !shape.components.isEmpty()) {
        qreal distance = 1.0e9;
        for (int index = 0; index < shape.components.size(); ++index) {
            const WorkPlaneFrame componentFrame =
                shapeComponentWorkPlaneFrame(shape, index);
            ViewportTransform componentTransform = transform;
            componentTransform.setWorkPlaneFrame(componentFrame);
            distance = std::min(distance,
                                distanceToNurbsCurve(screenPosition,
                                                     shape.components[index],
                                                     componentTransform,
                                                     viewportSize,
                                                     &componentFrame,
                                                     worldOffset));
        }
        return distance;
    }
    if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
        const QPointF point = transform.worldToScreen(shape.points.first(), viewportSize);
        return std::hypot(screenPosition.x() - point.x(), screenPosition.y() - point.y());
    }
    if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        if (validateNurbsCurve(shape.nurbs)) {
            return distanceToNurbsCurve(screenPosition,
                                       shape.nurbs,
                                       transform,
                                       viewportSize,
                                       &shapeFrame,
                                       worldOffset);
        }
        const QPointF center = transform.worldToScreen(shape.points[0], viewportSize);
        const QPointF edge = transform.worldToScreen(shape.points[1], viewportSize);
        const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
        return std::abs(std::hypot(screenPosition.x() - center.x(),
                                   screenPosition.y() - center.y()) - radius);
    }
    if (shape.geometryType == GeometryType::Ellipse) {
        return validateNurbsCurve(shape.nurbs)
                   ? distanceToNurbsCurve(screenPosition,
                                         shape.nurbs,
                                         transform,
                                         viewportSize,
                                         &shapeFrame,
                                         worldOffset)
                   : 1.0e9;
    }
    if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        return validateNurbsCurve(shape.nurbs)
                   ? distanceToNurbsCurve(screenPosition,
                                         shape.nurbs,
                                         transform,
                                         viewportSize,
                                         &shapeFrame,
                                         worldOffset)
                   : distanceToArc(screenPosition, shape, transform, viewportSize);
    }
    if (shape.geometryType == GeometryType::Bezier ||
        shape.geometryType == GeometryType::Nurbs) {
        if (validateNurbsCurve(shape.nurbs)) {
            return distanceToNurbsCurve(screenPosition,
                                       shape.nurbs,
                                       transform,
                                       viewportSize,
                                       &shapeFrame,
                                       worldOffset);
        }
        return shape.points.size() >= 4
                   ? distanceToCubicCurve(screenPosition,
                                          shape,
                                          transform,
                                          viewportSize)
                   : 1.0e9;
    }
    if (shape.geometryType == GeometryType::Rectangle) {
        if (validateNurbsCurve(shape.nurbs)) {
            return distanceToNurbsCurve(screenPosition, shape.nurbs, transform,
                                        viewportSize, &shapeFrame,
                                        worldOffset);
        }
        const QVector<QPointF> vertices = rectangleVertices(shape);
        if (vertices.size() < 4) {
            return 1.0e9;
        }
        qreal distance = 1.0e9;
        for (int index = 0; index < vertices.size(); ++index) {
            distance = std::min(
                distance,
                distanceToSegment(screenPosition,
                                  transform.worldToScreen(vertices[index], viewportSize),
                                  transform.worldToScreen(vertices[(index + 1) % vertices.size()],
                                                         viewportSize)));
        }
        return distance;
    }
    if (shape.geometryType == GeometryType::Picture) {
        const QVector<QPointF> vertices = pictureFrameCorners(shape);
        if (vertices.size() != 4) {
            return 1.0e9;
        }
        QPolygonF frame;
        for (const QPointF &vertex : vertices) {
            frame.append(transform.worldToScreen(vertex, viewportSize));
        }
        if (frame.containsPoint(screenPosition, Qt::OddEvenFill)) {
            // The bitmap's face must remain selectable, but vector geometry
            // drawn over it should win when the cursor is actually on a curve.
            return 8.0;
        }
        qreal distance = 1.0e9;
        for (int index = 0; index < frame.size(); ++index) {
            distance = std::min(
                distance,
                distanceToSegment(screenPosition,
                                  frame[index],
                                  frame[(index + 1) % frame.size()]));
        }
        return distance;
    }
    if (shape.geometryType == GeometryType::Polygon &&
        validateNurbsCurve(shape.nurbs)) {
        return distanceToNurbsCurve(screenPosition,
                                   shape.nurbs,
                                   transform,
                                   viewportSize,
                                   &shapeFrame,
                                   worldOffset);
    }
    if (shape.geometryType == GeometryType::Line) {
        if (validateNurbsCurve(shape.nurbs)) {
            return distanceToNurbsCurve(screenPosition,
                                       shape.nurbs,
                                       transform,
                                       viewportSize,
                                       &shapeFrame,
                                       worldOffset);
        }
        qreal distance = 1.0e9;
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            distance = std::min(
                distance,
                distanceToSegment(screenPosition,
                                  transform.worldToScreen(shape.points[index], viewportSize),
                                  transform.worldToScreen(shape.points[index + 1], viewportSize)));
        }
        return distance;
    }
    return 1.0e9;
}

qreal CurveHitTester::distanceToNurbsSurface(
    const QPointF &screenPosition,
    const Shape::NurbsSurface3D &surface,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    ObjectId objectId,
    quint64 geometryRevision,
    int faceIndex,
    const Point3D &worldOffset) const
{
    PreparedNurbsSurfaceTessellation localTessellation;
    QSharedPointer<const PreparedNurbsSurfaceTessellation> cachedTessellation;
    const PreparedNurbsSurfaceTessellation *tessellation = nullptr;
    if (surfaceTessellationCache_ != nullptr && objectId.isValid()) {
        cachedTessellation = surfaceTessellationCache_->acquire(
            objectId, geometryRevision, surface, faceIndex);
        tessellation = cachedTessellation.data();
    } else if (localTessellation.prepare(surface)) {
        tessellation = &localTessellation;
    }
    if (tessellation == nullptr) {
        return 1.0e9;
    }
    qreal distance = 1.0e9;
    for (const PreparedNurbsSurfaceTessellation::Polyline &polyline :
         tessellation->wireframe()) {
        QPointF previous;
        bool havePrevious = false;
        for (const Point3D &sourcePoint : polyline.points) {
            const Point3D worldPoint{sourcePoint.x + worldOffset.x,
                                     sourcePoint.y + worldOffset.y,
                                     sourcePoint.z + worldOffset.z};
            QPointF screenPoint;
            if (!transform.worldPointToScreenUnclipped(worldPoint,
                                                       viewportSize,
                                                       &screenPoint)) {
                havePrevious = false;
                continue;
            }
            if (havePrevious) {
                distance = std::min(
                    distance,
                    distanceToSegment(screenPosition, previous, screenPoint));
            }
            previous = screenPoint;
            havePrevious = true;
        }
    }
    for (const PreparedNurbsSurfaceTessellation::Triangle &triangle :
         tessellation->triangles()) {
        QPolygonF projectedTriangle;
        bool validTriangle = true;
        for (const int vertexIndex : triangle) {
            QPointF screenPoint;
            if (!transform.worldPointToScreenUnclipped(
                    Point3D{tessellation->vertices()[vertexIndex].x + worldOffset.x,
                            tessellation->vertices()[vertexIndex].y + worldOffset.y,
                            tessellation->vertices()[vertexIndex].z + worldOffset.z},
                    viewportSize,
                    &screenPoint)) {
                validTriangle = false;
                break;
            }
            projectedTriangle.append(screenPoint);
        }
        if (validTriangle &&
            projectedTriangle.containsPoint(screenPosition, Qt::OddEvenFill)) {
            distance = std::min<qreal>(distance, 8.0);
        }
    }
    return distance;
}

QVector<QPointF> CurveHitTester::controlPointsForShape(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::Rectangle) return rectangleVertices(shape);
    if (shape.geometryType == GeometryType::Point ||
        isDimensionGeometryType(shape.geometryType)) {
        return {};
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        QVector<QPointF> controlPoints;
        for (const Shape::NurbsCurve2D &component : shape.components) {
            controlPoints += component.controlPoints;
        }
        return controlPoints;
    }
    if (shape.geometryType == GeometryType::Polygon) {
        return polygonVerticesForShape(shape);
    }
    if (!shape.nurbs.controlPoints.isEmpty()) {
        return shape.nurbs.controlPoints;
    }
    return shape.points;
}

bool CurveHitTester::hitTestSelectedControlPoint(
    const Document &document,
    const QVector<int> &selectedShapeIndices,
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    int *shapeIndex,
    int *controlPointIndex) const
{
    constexpr qreal hitRadiusPixels = 10.0;
    int closestShapeIndex = -1;
    int closestControlPointIndex = -1;
    qreal closestDistance = hitRadiusPixels;

    for (const int candidateShapeIndex : selectedShapeIndices) {
        if (candidateShapeIndex < 0 || candidateShapeIndex >= document.size()) {
            continue;
        }
        const Shape &shape = document[candidateShapeIndex];
        const ObjectId objectId = document.objectIdAt(candidateShapeIndex);
        const SceneObject *sceneObject = document.object(objectId);
        const Point3D placement = sceneObject != nullptr
                                      ? sceneObject->placementTranslation
                                      : Point3D{};
        const auto considerControlPoint = [&](const QPointF &controlPoint,
                                              qreal normalCoordinate,
                                              WorkPlaneFrame frame,
                                              int candidateControlPointIndex) {
            frame.origin.x += placement.x;
            frame.origin.y += placement.y;
            frame.origin.z += placement.z;
            const Point3D worldPoint = workPlaneFramePointToWorld(
                controlPoint, normalCoordinate, frame);
            QPointF screenPoint;
            if (!transform.worldPointToScreenUnclipped(
                    worldPoint, viewportSize, &screenPoint)) {
                return;
            }
            const qreal distance = std::hypot(
                screenPosition.x() - screenPoint.x(),
                screenPosition.y() - screenPoint.y());
            if (distance <= closestDistance) {
                closestDistance = distance;
                closestShapeIndex = candidateShapeIndex;
                closestControlPointIndex = candidateControlPointIndex;
            }
        };

        int globalIndex = 0;
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (int componentIndex = 0;
                 componentIndex < shape.components.size();
                 ++componentIndex) {
                const WorkPlaneFrame frame =
                    shapeComponentWorkPlaneFrame(shape, componentIndex);
                const Shape::NurbsCurve3D &curve =
                    shape.components[componentIndex];
                for (int pointIndex = 0;
                     pointIndex < curve.controlPoints.size(); ++pointIndex) {
                    const qreal normalCoordinate = curve.dimension == 3
                        ? curve.normalCoordinates[pointIndex] : 0.0;
                    considerControlPoint(curve.controlPoints[pointIndex],
                                         normalCoordinate, frame, globalIndex);
                    ++globalIndex;
                }
            }
            continue;
        }
        const QVector<QPointF> controlPoints = controlPointsForShape(shape);
        for (int candidateControlPointIndex = 0;
             candidateControlPointIndex < controlPoints.size();
             ++candidateControlPointIndex) {
            considerControlPoint(controlPoints[candidateControlPointIndex],
                                 shape.nurbs.dimension == 3 &&
                                         candidateControlPointIndex <
                                             shape.nurbs.normalCoordinates.size()
                                     ? shape.nurbs.normalCoordinates[
                                           candidateControlPointIndex]
                                     : 0.0,
                                 shapeWorkPlaneFrame(shape),
                                 candidateControlPointIndex);
        }
    }

    if (shapeIndex != nullptr) {
        *shapeIndex = closestShapeIndex;
    }
    if (controlPointIndex != nullptr) {
        *controlPointIndex = closestControlPointIndex;
    }
    return closestShapeIndex >= 0 && closestControlPointIndex >= 0;
}

int CurveHitTester::hitTestShape(const Document &document,
                                 const QPointF &screenPosition,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 bool editableOnly) const
{
    constexpr qreal hitRadiusPixels = 9.0;
    int closestShape = -1;
    qreal closestDistance = hitRadiusPixels;
    for (int index = 0; index < document.size(); ++index) {
        const ObjectId objectId = document.objectIdAt(index);
        if (!document.isObjectVisible(objectId) ||
            (editableOnly && !document.isObjectEditable(objectId))) {
            continue;
        }
        const Shape &shape = document[index];
        const SceneObject *sceneObject = document.object(objectId);
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        ViewportTransform shapeTransform = transform;
        shapeTransform.setWorkPlaneFrame(shapeWorkPlaneFrame(shape));
        const QVector<ShapeNurbsCurveComponent> curveComponents =
            shapeHasCurveControlHull(shape)
                ? nurbsCurveComponentsForShape(shape)
                : QVector<ShapeNurbsCurveComponent>{};
        if (!curveComponents.isEmpty()) {
            bool hullIsKnown = true;
            QRectF projectedHull;
            bool hasProjectedHull = false;
            for (const ShapeNurbsCurveComponent &component : curveComponents) {
                QRectF componentBounds;
                if (!projectedNurbsControlHullBounds(component.curve,
                                                      component.workPlaneFrame,
                                                      transform,
                                                      viewportSize,
                                                      &componentBounds,
                                                      worldOffset)) {
                    hullIsKnown = false;
                    break;
                }
                includeScreenBounds(componentBounds,
                                    &projectedHull,
                                    &hasProjectedHull);
            }
            if (hullIsKnown && hasProjectedHull) {
                const QRectF hitBounds(
                    screenPosition - QPointF(hitRadiusPixels, hitRadiusPixels),
                    QSizeF(2.0 * hitRadiusPixels, 2.0 * hitRadiusPixels));
                if (!screenBoundsOverlap(projectedHull, hitBounds)) {
                    continue;
                }
            }
        }
        const qreal distance = distanceToShape(screenPosition,
                                               shape,
                                               shapeTransform,
                                               viewportSize,
                                               objectId,
                                               document.objectGeometryRevision(objectId),
                                               worldOffset);
        if (distance <= closestDistance) {
            closestDistance = distance;
            closestShape = index;
        }
    }
    return closestShape;
}

int CurveHitTester::hitTestShapeOnAnyWorkPlane(
    const Document &document,
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    // Match OSnap's capture radius so a candidate on an inherited plane can
    // still be acquired anywhere inside the existing snap tolerance.
    constexpr qreal hitRadiusPixels = 12.0;
    int closestShape = -1;
    qreal closestDistance = hitRadiusPixels;
    qreal closestDepth = -std::numeric_limits<qreal>::infinity();
    for (int index = 0; index < document.size(); ++index) {
        if (!document.isObjectVisible(document.objectIdAt(index))) {
            continue;
        }
        const Shape &shape = document[index];
        if (shape.geometryType == GeometryType::NurbsSurface ||
            shape.geometryType == GeometryType::NurbsSolid) {
            // Spatial faces have no single drawing plane to inherit. They
            // remain selectable in the regular hit test above.
            continue;
        }
        const ObjectId objectId = document.objectIdAt(index);
        const SceneObject *sceneObject = document.object(objectId);
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        ViewportTransform shapeTransform = transform;
        const QVector<ShapeNurbsCurveComponent> curveComponents =
            shapeHasCurveControlHull(shape)
                ? nurbsCurveComponentsForShape(shape)
                : QVector<ShapeNurbsCurveComponent>{};
        WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        if (!curveComponents.isEmpty()) {
            frame = curveComponents.first().workPlaneFrame;
            bool planarCurveBundle = true;
            bool firstPlane = true;
            for (const ShapeNurbsCurveComponent &component : curveComponents) {
                if (!isNurbsCurvePlanarInWorkPlane(component.curve)) {
                    planarCurveBundle = false;
                    break;
                }
                const qreal normalCoordinate =
                    component.curve.dimension == 3 &&
                            !component.curve.normalCoordinates.isEmpty()
                        ? component.curve.normalCoordinates.first() : 0.0;
                WorkPlaneFrame componentPlane = component.workPlaneFrame;
                componentPlane.origin.x +=
                    componentPlane.normal.x * normalCoordinate;
                componentPlane.origin.y +=
                    componentPlane.normal.y * normalCoordinate;
                componentPlane.origin.z +=
                    componentPlane.normal.z * normalCoordinate;
                if (firstPlane) {
                    frame = componentPlane;
                    firstPlane = false;
                } else if (!workPlaneFramesCoplanar(frame, componentPlane)) {
                    planarCurveBundle = false;
                    break;
                }
            }
            if (!planarCurveBundle) {
                // Spatial curves that do not share one plane cannot supply a
                // drawing frame to a planar tool.
                continue;
            }
        }
        frame.origin.x += worldOffset.x;
        frame.origin.y += worldOffset.y;
        frame.origin.z += worldOffset.z;
        shapeTransform.setWorkPlaneFrame(frame);
        if (!curveComponents.isEmpty()) {
            bool hullIsKnown = true;
            QRectF projectedHull;
            bool hasProjectedHull = false;
            for (const ShapeNurbsCurveComponent &component : curveComponents) {
                QRectF componentBounds;
                if (!projectedNurbsControlHullBounds(component.curve,
                                                      component.workPlaneFrame,
                                                      transform,
                                                      viewportSize,
                                                      &componentBounds,
                                                      worldOffset)) {
                    hullIsKnown = false;
                    break;
                }
                includeScreenBounds(componentBounds,
                                    &projectedHull,
                                    &hasProjectedHull);
            }
            if (hullIsKnown && hasProjectedHull) {
                const QRectF hitBounds(
                    screenPosition - QPointF(hitRadiusPixels, hitRadiusPixels),
                    QSizeF(2.0 * hitRadiusPixels, 2.0 * hitRadiusPixels));
                if (!screenBoundsOverlap(projectedHull, hitBounds)) {
                    continue;
                }
            }
        }
        const qreal distance = distanceToShape(screenPosition,
                                               shape,
                                               shapeTransform,
                                               viewportSize,
                                               objectId,
                                               document.objectGeometryRevision(objectId),
                                               worldOffset);
        if (!std::isfinite(distance) || distance > hitRadiusPixels) {
            continue;
        }
        QPointF planePoint;
        if (!transform.screenToWorkPlane(screenPosition,
                                         viewportSize,
                                         frame,
                                         &planePoint)) {
            continue;
        }
        const Point3D worldPoint = workPlaneFramePointToWorld(planePoint, frame);
        const qreal depth = transform.worldDirectionToView(worldPoint).towardCamera;
        if (std::isfinite(depth) &&
            (depth > closestDepth + 1.0e-6 ||
             (std::abs(depth - closestDepth) <= 1.0e-6 &&
              distance < closestDistance))) {
            closestDistance = distance;
            closestDepth = depth;
            closestShape = index;
        }
    }
    return closestShape;
}

bool CurveHitTester::hitTestVisibleDepth(const Document &document,
                                         const QPointF &screenPosition,
                                         const ViewportTransform &transform,
                                         const QSize &viewportSize,
                                         Point3D *worldPoint) const
{
    if (worldPoint == nullptr) {
        return false;
    }
    constexpr qreal hitRadiusPixels = 9.0;
    qreal nearestDepth = -std::numeric_limits<qreal>::infinity();
    qreal nearestScreenDistance = hitRadiusPixels;
    bool found = false;
    for (int index = 0; index < document.size(); ++index) {
        if (!document.isObjectVisible(document.objectIdAt(index))) {
            continue;
        }
        const Shape &shape = document[index];
        ViewportTransform shapeTransform = transform;
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        shapeTransform.setWorkPlaneFrame(frame);
        const ObjectId objectId = document.objectIdAt(index);
        const SceneObject *sceneObject = document.object(objectId);
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        const qreal screenDistance = distanceToShape(screenPosition,
                                                      shape,
                                                      shapeTransform,
                                                      viewportSize,
                                                      objectId,
                                                      document.objectGeometryRevision(objectId),
                                                      worldOffset);
        if (!std::isfinite(screenDistance) || screenDistance > hitRadiusPixels) {
            continue;
        }
        Point3D candidate;
        const bool spatialSurface =
            shape.geometryType == GeometryType::NurbsSurface ||
            shape.geometryType == GeometryType::NurbsSolid;
        if (spatialSurface) {
            qreal nearestSurfaceDepth = -std::numeric_limits<qreal>::infinity();
            bool foundSurfacePoint = false;
            const auto considerSurface = [&](const NurbsSurface3D &surface,
                                             int faceIndex) {
                PreparedNurbsSurfaceTessellation localTessellation;
                QSharedPointer<const PreparedNurbsSurfaceTessellation>
                    cachedTessellation;
                const PreparedNurbsSurfaceTessellation *mesh = nullptr;
                if (surfaceTessellationCache_ != nullptr && objectId.isValid()) {
                    cachedTessellation = surfaceTessellationCache_->acquire(
                        objectId, document.objectGeometryRevision(objectId),
                        surface, faceIndex);
                    mesh = cachedTessellation.data();
                } else if (localTessellation.prepare(surface)) {
                    mesh = &localTessellation;
                }
                if (mesh == nullptr) {
                    return;
                }
                for (const PreparedNurbsSurfaceTessellation::Triangle &triangle :
                     mesh->triangles()) {
                    Point3D surfacePoint;
                    if (!screenPointOnSurfaceTriangle(
                            screenPosition, *mesh, triangle, worldOffset,
                            transform, viewportSize, &surfacePoint)) {
                        continue;
                    }
                    const qreal depth = transform.worldDirectionToView(
                        surfacePoint).towardCamera;
                    if (std::isfinite(depth) &&
                        (!foundSurfacePoint || depth > nearestSurfaceDepth)) {
                        nearestSurfaceDepth = depth;
                        candidate = surfacePoint;
                        foundSurfacePoint = true;
                    }
                }
            };
            if (shape.geometryType == GeometryType::NurbsSurface) {
                considerSurface(shape.nurbsSurface, 0);
            } else {
                const QVector<NurbsSurface3D> faces = shapeSurfaceFaces(shape);
                for (int faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
                    considerSurface(faces[faceIndex], faceIndex);
                }
            }
            if (!foundSurfacePoint) {
                continue;
            }
        } else {
            QPointF planePoint;
            if (!transform.screenToWorkPlane(screenPosition,
                                             viewportSize,
                                             frame,
                                             &planePoint)) {
                continue;
            }
            candidate = workPlaneFramePointToWorld(planePoint, frame);
        }
        // Curves are unfilled geometry: the closest sampled stroke point is
        // their actual scene depth, while a picture face uses the ray/plane hit.
        qreal closestStrokeDistance = hitRadiusPixels;
        const auto considerCurve = [&](const Shape::NurbsCurve2D &curve,
                                       const WorkPlaneFrame &curveFrame) {
            qreal firstParameter = 0.0;
            qreal lastParameter = 0.0;
            if (!validateNurbsCurve(curve) ||
                !nurbsParameterDomain(curve, &firstParameter, &lastParameter)) {
                return;
            }
            constexpr int samples = 128;
            Point3D previousLocal;
            if (!evaluateNurbsPoint3D(curve, firstParameter, &previousLocal)) {
                return;
            }
            const auto toWorld = [&](const Point3D &local) {
                Point3D world = workPlaneFramePointToWorld(
                    {local.x, local.y}, local.z, curveFrame);
                world.x += worldOffset.x;
                world.y += worldOffset.y;
                world.z += worldOffset.z;
                return world;
            };
            Point3D previousWorld = toWorld(previousLocal);
            QPointF previousScreen;
            if (!transform.worldPointToScreenUnclipped(
                    previousWorld, viewportSize, &previousScreen)) {
                return;
            }
            for (int sample = 1; sample <= samples; ++sample) {
                Point3D currentLocal;
                const qreal parameter = firstParameter +
                    (lastParameter - firstParameter) * sample / samples;
                if (!evaluateNurbsPoint3D(curve, parameter, &currentLocal)) {
                    continue;
                }
                const Point3D currentWorld = toWorld(currentLocal);
                QPointF currentScreen;
                if (!transform.worldPointToScreenUnclipped(
                        currentWorld, viewportSize, &currentScreen)) {
                    continue;
                }
                const QPointF segment = currentScreen - previousScreen;
                const qreal lengthSquared = QPointF::dotProduct(segment, segment);
                const qreal fraction = lengthSquared > 1.0e-12
                    ? std::clamp(QPointF::dotProduct(
                                     screenPosition - previousScreen, segment) /
                                     lengthSquared, 0.0, 1.0)
                    : 0.0;
                const QPointF closestScreen = previousScreen + segment * fraction;
                const qreal distance = std::hypot(
                    closestScreen.x() - screenPosition.x(),
                    closestScreen.y() - screenPosition.y());
                if (std::isfinite(distance) && distance <= closestStrokeDistance) {
                    closestStrokeDistance = distance;
                    candidate = {
                        previousWorld.x + (currentWorld.x - previousWorld.x) * fraction,
                        previousWorld.y + (currentWorld.y - previousWorld.y) * fraction,
                        previousWorld.z + (currentWorld.z - previousWorld.z) * fraction};
                }
                previousLocal = currentLocal;
                previousWorld = currentWorld;
                previousScreen = currentScreen;
            }
        };
        if (shape.geometryType == GeometryType::PolyCurve) {
            for (int componentIndex = 0;
                 componentIndex < shape.components.size();
                 ++componentIndex) {
                considerCurve(shape.components[componentIndex],
                              shapeComponentWorkPlaneFrame(shape, componentIndex));
            }
        } else {
            considerCurve(shape.nurbs, frame);
        }
        if (shape.geometryType == GeometryType::Point &&
            !shape.points.isEmpty()) {
            candidate = shapePointToWorld(shape, shape.points.first());
        }
        const qreal depth = transform.worldDirectionToView(candidate).towardCamera;
        if (!found || depth > nearestDepth + 1.0e-6 ||
            (std::abs(depth - nearestDepth) <= 1.0e-6 &&
             screenDistance < nearestScreenDistance)) {
            *worldPoint = candidate;
            nearestDepth = depth;
            nearestScreenDistance = screenDistance;
            found = true;
        }
    }
    return found;
}

bool CurveHitTester::hitTestVisibleSurface(
    const Document &document,
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    Point3D *worldPoint,
    Point3D *worldNormal,
    int *hitFaceIndex,
    int *hitShapeIndex,
    const QVector<int> &excludedShapeIndices,
    bool backfaceCulling) const
{
    if (worldPoint == nullptr || worldNormal == nullptr) {
        return false;
    }

    qreal nearestDepth = -std::numeric_limits<qreal>::infinity();
    bool found = false;
    const Point3D viewDirection = transform.viewDirection();
    for (int objectIndex = 0; objectIndex < document.size(); ++objectIndex) {
        const ObjectId objectId = document.objectIdAt(objectIndex);
        if (excludedShapeIndices.contains(objectIndex) ||
            !document.isObjectVisible(objectId)) {
            continue;
        }
        const Shape &shape = document[objectIndex];
        if (shape.geometryType == GeometryType::Picture) {
            const SceneObject *sceneObject = document.object(objectId);
            WorkPlaneFrame placedFrame = shapeWorkPlaneFrame(shape);
            if (sceneObject != nullptr) {
                placedFrame.origin.x += sceneObject->placementTranslation.x;
                placedFrame.origin.y += sceneObject->placementTranslation.y;
                placedFrame.origin.z += sceneObject->placementTranslation.z;
            }
            QPointF localPoint;
            if (!transform.screenToWorkPlane(screenPosition,
                                             viewportSize,
                                             placedFrame,
                                             &localPoint)) {
                continue;
            }
            const QPolygonF polygon(pictureFrameCorners(shape));
            if (polygon.size() < 3 ||
                !polygon.containsPoint(localPoint, Qt::OddEvenFill)) {
                continue;
            }
            const Point3D candidate =
                workPlaneFramePointToWorld(localPoint, placedFrame);
            if (backfaceCulling &&
                (placedFrame.normal.x * viewDirection.x +
                 placedFrame.normal.y * viewDirection.y +
                 placedFrame.normal.z * viewDirection.z) <= 0.0) {
                continue;
            }
            const qreal depth = transform.worldDirectionToView(candidate)
                                    .towardCamera;
            if (std::isfinite(depth) && (!found || depth > nearestDepth)) {
                *worldPoint = candidate;
                *worldNormal = placedFrame.normal;
                if (hitFaceIndex != nullptr) {
                    *hitFaceIndex = 0;
                }
                if (hitShapeIndex != nullptr) {
                    *hitShapeIndex = objectIndex;
                }
                nearestDepth = depth;
                found = true;
            }
            continue;
        }
        if (shape.geometryType != GeometryType::NurbsSurface &&
            shape.geometryType != GeometryType::NurbsSolid) {
            continue;
        }

        const SceneObject *sceneObject = document.object(objectId);
        const Point3D worldOffset = sceneObject != nullptr
                                        ? sceneObject->placementTranslation
                                        : Point3D{};
        const QVector<NurbsSurface3D> faces =
            shape.geometryType == GeometryType::NurbsSurface
                ? QVector<NurbsSurface3D>{shape.nurbsSurface}
                : shapeSurfaceFaces(shape);
        for (int faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const NurbsSurface3D &surface = faces[faceIndex];
            QRectF projectedBounds;
            if (!projectedNurbsSurfaceControlHullBounds(
                    surface, transform, viewportSize, worldOffset,
                    &projectedBounds) ||
                !projectedBounds.adjusted(-1.0, -1.0, 1.0, 1.0)
                     .contains(screenPosition)) {
                continue;
            }

            PreparedNurbsSurfaceTessellation localTessellation;
            QSharedPointer<const PreparedNurbsSurfaceTessellation>
                cachedTessellation;
            const PreparedNurbsSurfaceTessellation *mesh = nullptr;
            if (surfaceTessellationCache_ != nullptr && objectId.isValid()) {
                cachedTessellation = surfaceTessellationCache_->acquire(
                    objectId, document.objectGeometryRevision(objectId),
                    surface, faceIndex);
                mesh = cachedTessellation.data();
            } else if (localTessellation.prepare(surface)) {
                mesh = &localTessellation;
            }
            if (mesh == nullptr) {
                continue;
            }

            const bool reverseFace =
                shape.geometryType == GeometryType::NurbsSolid &&
                nurbsSolidFaceReversed(shape.nurbsSolid, faceIndex);
            for (const PreparedNurbsSurfaceTessellation::Triangle &triangle :
                 mesh->triangles()) {
                Point3D candidate;
                Point3D normal;
                if (!screenPointOnSurfaceTriangle(
                        screenPosition, *mesh, triangle, worldOffset,
                        transform, viewportSize, &candidate, &normal)) {
                    continue;
                }
                if (reverseFace) {
                    normal = {-normal.x, -normal.y, -normal.z};
                }
                if (backfaceCulling &&
                    (normal.x * viewDirection.x +
                     normal.y * viewDirection.y +
                     normal.z * viewDirection.z) <= 0.0) {
                    continue;
                }
                const qreal depth = transform.worldDirectionToView(candidate)
                                        .towardCamera;
                if (std::isfinite(depth) && (!found || depth > nearestDepth)) {
                    *worldPoint = candidate;
                    *worldNormal = normal;
                    if (hitFaceIndex != nullptr) {
                        *hitFaceIndex = faceIndex;
                    }
                    if (hitShapeIndex != nullptr) {
                        *hitShapeIndex = objectIndex;
                    }
                    nearestDepth = depth;
                    found = true;
                }
            }
        }
    }
    return found;
}

} // namespace classiCAD
