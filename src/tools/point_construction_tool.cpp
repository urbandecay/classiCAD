#include "point_construction_tool.h"

#include "core/geometry/arc_curve_factory.h"
#include "core/geometry/curve_evaluator.h"
#include "core/document/document_settings.h"
#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tool_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal kTwoPi = 6.28318530717958647692;
constexpr qreal kPi = 3.14159265358979323846;

Point3D subtract3(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D add3(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D scale3(const Point3D &point, qreal scale)
{
    return {point.x * scale, point.y * scale, point.z * scale};
}

qreal dot3(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D cross3(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

qreal length3(const Point3D &point)
{
    return std::sqrt(dot3(point, point));
}

Point3D normalized3(const Point3D &point)
{
    const qreal magnitude = length3(point);
    return magnitude > 1.0e-12 ? scale3(point, 1.0 / magnitude) : Point3D{};
}

qreal normalizedAngle(qreal angle)
{
    angle = std::fmod(angle, kTwoPi);
    return angle < 0.0 ? angle + kTwoPi : angle;
}

bool angleOnArc(qreal angle, qreal startAngle, qreal sweepAngle)
{
    if (std::abs(sweepAngle) >= kTwoPi - 1.0e-5) {
        return true;
    }
    const qreal offset = sweepAngle >= 0.0
                             ? normalizedAngle(angle - startAngle)
                             : normalizedAngle(startAngle - angle);
    return offset <= std::abs(sweepAngle) + 0.17;
}

QPointF pointOnArc(const QPointF &center,
                   qreal radius,
                   qreal startAngle,
                   qreal sweepAngle,
                   qreal fraction)
{
    const qreal angle = startAngle + sweepAngle * fraction;
    return center + QPointF(radius * std::cos(angle), radius * std::sin(angle));
}

qreal softSnappedAngle(qreal angle, qreal snapStrengthDegrees = 6.0)
{
    const qreal degrees = angle * 180.0 / kPi;
    const qreal nearest = std::round(degrees / 15.0) * 15.0;
    if (std::abs(degrees - nearest) <= snapStrengthDegrees) {
        return nearest * kPi / 180.0;
    }
    return angle;
}

QColor pointLineAxisColor(const Point3D &direction)
{
    const qreal magnitude = length3(direction);
    if (magnitude <= 1.0e-9) {
        return QColor(QStringLiteral("#151515"));
    }
    const qreal x = std::abs(direction.x / magnitude);
    const qreal y = std::abs(direction.y / magnitude);
    const qreal z = std::abs(direction.z / magnitude);
    if (x > 0.9999) return QColor(QStringLiteral("#ff1a1a"));
    if (y > 0.9999) return QColor(QStringLiteral("#1ab31a"));
    if (z > 0.9999) return QColor(QStringLiteral("#3380ff"));
    return QColor(QStringLiteral("#151515"));
}

WorkPlaneFrame pointFrameAt(const Point3D &point,
                           const WorkPlaneFrame &preferredFrame)
{
    WorkPlaneFrame frame = preferredFrame;
    frame.origin = point;
    return frame;
}

WorkPlaneFrame lineSegmentFrame(const Point3D &first,
                                const Point3D &second,
                                const WorkPlaneFrame &preferredFrame)
{
    const Point3D xAxis = normalized3(subtract3(second, first));
    if (length3(xAxis) <= 1.0e-10) return {};
    Point3D normal = normalized3(cross3(xAxis, preferredFrame.yAxis));
    if (length3(normal) <= 1.0e-8) {
        const Point3D helper = std::abs(xAxis.z) < 0.9
                                   ? Point3D{0.0, 0.0, 1.0}
                                   : Point3D{0.0, 1.0, 0.0};
        normal = normalized3(cross3(xAxis, helper));
    }
    return makeWorkPlaneFrameFromNormal(first, normal, xAxis);
}

Shape arcPreviewShape(const QPointF &center,
                      qreal radius,
                      qreal startAngle,
                      qreal sweepAngle,
                      const WorkPlaneFrame &frame)
{
    Shape shape;
    shape.geometryType = GeometryType::Arc;
    shape.workPlaneFrame = frame;
    shape.workPlane = WorkPlane::XY;
    shape.workPlaneOffset = frame.origin.z;
    if (radius <= 1.0e-10 || std::abs(sweepAngle) <= 1.0e-8) {
        return shape;
    }
    if (std::abs(sweepAngle) >= kTwoPi - 1.0e-5) {
        shape.geometryType = GeometryType::Circle;
        shape.points = {center, center + QPointF(radius, 0.0)};
        shape.nurbs = makeCircleNurbs(shape.points);
        return shape;
    }
    const QPointF start = pointOnArc(center, radius, startAngle, sweepAngle, 0.0);
    const QPointF through = pointOnArc(center, radius, startAngle, sweepAngle, 0.5);
    const QPointF end = pointOnArc(center, radius, startAngle, sweepAngle, 1.0);
    CircularArc2D arc;
    if (makeCircularArcThroughPoint(start, end, through, &arc)) {
        shape.points = {start, through, end};
        shape.nurbs = arc.curve;
    }
    return shape;
}

QVector<Shape::NurbsCurve2D> curvesForShape(const Shape &shape)
{
    QVector<Shape::NurbsCurve2D> curves;
    if (validateNurbsCurve(shape.nurbs)) {
        curves.append(shape.nurbs);
    }
    if (curves.isEmpty()) {
        for (const Shape::NurbsCurve2D &component : shape.components) {
            if (validateNurbsCurve(component)) {
                curves.append(component);
            }
        }
    }
    return curves;
}

bool mapCurveToFrame(const Shape &shape,
                     const Shape::NurbsCurve2D &source,
                     const WorkPlaneFrame &destinationFrame,
                     Shape::NurbsCurve2D *mapped)
{
    if (mapped == nullptr || !isValidWorkPlaneFrame(destinationFrame)) {
        return false;
    }
    const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(shape);
    if (!isValidWorkPlaneFrame(sourceFrame)) {
        return false;
    }
    Shape::NurbsCurve2D result = source;
    qreal scale = 1.0;
    for (const QPointF &point : source.controlPoints) {
        scale = std::max(scale, std::hypot(point.x(), point.y()));
    }
    const qreal planeTolerance = 1.0e-7 * scale;
    for (int index = 0; index < source.controlPoints.size(); ++index) {
        const Point3D world = workPlaneFramePointToWorld(source.controlPoints[index],
                                                         sourceFrame);
        if (std::abs(signedDistanceFromWorkPlaneFrame(world, destinationFrame)) >
            planeTolerance) {
            return false;
        }
        result.controlPoints[index] = worldPointToWorkPlaneFrame(world,
                                                                  destinationFrame);
    }
    *mapped = std::move(result);
    return true;
}

QVector<QPointF> sampleCurve(const Shape::NurbsCurve2D &curve, int divisions)
{
    QVector<QPointF> result;
    qreal start = 0.0;
    qreal end = 0.0;
    if (divisions < 1 || !nurbsParameterDomain(curve, &start, &end)) {
        return result;
    }
    result.reserve(divisions + 1);
    for (int index = 0; index <= divisions; ++index) {
        QPointF point;
        const qreal fraction = static_cast<qreal>(index) / divisions;
        if (!evaluateNurbsPoint(curve, start + (end - start) * fraction, &point)) {
            return {};
        }
        result.append(point);
    }
    return result;
}

bool solveCircleCenter(const QVector<QPointF> &points, QPointF *center)
{
    if (center == nullptr || points.size() < 3) {
        return false;
    }
    QPointF origin;
    for (const QPointF &point : points) {
        origin += point;
    }
    origin /= static_cast<qreal>(points.size());

    std::array<std::array<qreal, 4>, 3> matrix{};
    for (const QPointF &point : points) {
        const qreal x = point.x() - origin.x();
        const qreal y = point.y() - origin.y();
        const qreal rhs = -(x * x + y * y);
        matrix[0][0] += x * x;
        matrix[0][1] += x * y;
        matrix[0][2] += x;
        matrix[0][3] += x * rhs;
        matrix[1][0] += x * y;
        matrix[1][1] += y * y;
        matrix[1][2] += y;
        matrix[1][3] += y * rhs;
        matrix[2][0] += x;
        matrix[2][1] += y;
        matrix[2][2] += 1.0;
        matrix[2][3] += rhs;
    }

    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(matrix[row][column]) >
                std::abs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[pivot][column]) <= 1.0e-12) {
            return false;
        }
        std::swap(matrix[pivot], matrix[column]);
        const qreal divisor = matrix[column][column];
        for (int entry = column; entry < 4; ++entry) {
            matrix[column][entry] /= divisor;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == column) {
                continue;
            }
            const qreal factor = matrix[row][column];
            for (int entry = column; entry < 4; ++entry) {
                matrix[row][entry] -= factor * matrix[column][entry];
            }
        }
    }
    const QPointF result(origin.x() - matrix[0][3] * 0.5,
                         origin.y() - matrix[1][3] * 0.5);
    if (!std::isfinite(result.x()) || !std::isfinite(result.y())) {
        return false;
    }
    *center = result;
    return true;
}

Shape pointShape(const QPointF &point, const WorkPlaneFrame &frame)
{
    Shape result;
    result.geometryType = GeometryType::Point;
    result.points.append(point);
    result.workPlaneFrame = frame;
    result.workPlane = WorkPlane::XY;
    result.workPlaneOffset = 0.0;
    if (std::abs(frame.normal.z) >= 1.0 - 1.0e-8) {
        result.workPlaneOffset = frame.origin.z;
    } else if (std::abs(frame.normal.y) >= 1.0 - 1.0e-8) {
        result.workPlane = WorkPlane::XZ;
        result.workPlaneOffset = frame.origin.y;
    } else if (std::abs(frame.normal.x) >= 1.0 - 1.0e-8) {
        result.workPlane = WorkPlane::YZ;
        result.workPlaneOffset = frame.origin.x;
    }
    return result;
}

Shape linePreviewShape(const QPointF &first,
                       const QPointF &second,
                       const WorkPlaneFrame &frame)
{
    Shape shape;
    shape.geometryType = GeometryType::Line;
    shape.points = {first, second};
    shape.workPlaneFrame = frame;
    shape.workPlane = WorkPlane::XY;
    shape.workPlaneOffset = frame.origin.z;
    return shape;
}

Shape polylinePreviewShape(const QVector<QPointF> &points,
                           const WorkPlaneFrame &frame)
{
    Shape shape;
    shape.geometryType = GeometryType::Line;
    shape.points = points;
    shape.nurbs = makeDegreeOneNurbs(points);
    shape.workPlaneFrame = frame;
    shape.workPlane = WorkPlane::XY;
    shape.workPlaneOffset = frame.origin.z;
    return shape;
}

QVector<QPointF> catmullRomPreview(const QVector<QPointF> &points,
                                   bool closed)
{
    QVector<QPointF> result;
    if (points.size() < 3) {
        return points;
    }
    constexpr int stepsPerSegment = 12;
    const int pointCount = static_cast<int>(points.size());
    const int segmentCount = closed ? pointCount : pointCount - 1;
    result.reserve(segmentCount * stepsPerSegment + 1);
    for (int segment = 0; segment < segmentCount; ++segment) {
        const int i1 = segment;
        const int i2 = (segment + 1) % pointCount;
        const int i0 = closed ? (segment - 1 + pointCount) % pointCount
                              : std::max(0, segment - 1);
        const int i3 = closed ? (segment + 2) % pointCount
                              : std::min(pointCount - 1, segment + 2);
        const QPointF p0 = points[i0];
        const QPointF p1 = points[i1];
        const QPointF p2 = points[i2];
        const QPointF p3 = points[i3];
        for (int step = 0; step < stepsPerSegment; ++step) {
            const qreal t = static_cast<qreal>(step) / stepsPerSegment;
            const qreal t2 = t * t;
            const qreal t3 = t2 * t;
            result.append(0.5 * ((2.0 * p1) + (-p0 + p2) * t +
                                 (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                                 (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3));
        }
    }
    result.append(closed ? result.first() : points.last());
    return result;
}

bool fitWorldPlane(const QVector<Point3D> &points,
                   WorkPlaneFrame *frame,
                   QVector<QPointF> *localPoints)
{
    if (frame == nullptr || localPoints == nullptr || points.size() < 3) {
        return false;
    }

    Point3D centroid;
    for (const Point3D &point : points) {
        centroid = add3(centroid, point);
    }
    centroid = scale3(centroid, 1.0 / points.size());

    Point3D bestNormal;
    Point3D preferredXAxis;
    qreal bestAreaSquared = 0.0;
    for (int first = 0; first < points.size(); ++first) {
        const Point3D a = subtract3(points[first], centroid);
        for (int second = first + 1; second < points.size(); ++second) {
            const Point3D b = subtract3(points[second], centroid);
            const Point3D normal = cross3(a, b);
            const qreal areaSquared = dot3(normal, normal);
            if (areaSquared > bestAreaSquared) {
                bestAreaSquared = areaSquared;
                bestNormal = normal;
                preferredXAxis = a;
            }
        }
    }
    if (bestAreaSquared <= 1.0e-18) {
        return false;
    }

    const WorkPlaneFrame candidate = makeWorkPlaneFrameFromNormal(
        centroid, bestNormal, preferredXAxis);
    if (!isValidWorkPlaneFrame(candidate)) {
        return false;
    }
    localPoints->clear();
    localPoints->reserve(points.size());
    for (const Point3D &point : points) {
        // The add-on fits a best-fit plane and projects selected vertices onto
        // it before fitting the circle. Preserve that behavior for slightly
        // non-planar selections instead of rejecting the whole selection.
        localPoints->append(worldPointToWorkPlaneFrame(point, candidate));
    }
    *frame = candidate;
    return true;
}

void appendPointMarker(QVector<Shape> *shapes,
                       const QPointF &point,
                       const WorkPlaneFrame &frame)
{
    if (shapes != nullptr) {
        shapes->append(pointShape(point, frame));
    }
}

bool appendUnique(QVector<QPointF> *points, const QPointF &point)
{
    if (points == nullptr || !std::isfinite(point.x()) ||
        !std::isfinite(point.y())) {
        return false;
    }
    for (const QPointF &existing : *points) {
        if (std::hypot(existing.x() - point.x(), existing.y() - point.y()) <=
            1.0e-6) {
            return false;
        }
    }
    points->append(point);
    return true;
}

} // namespace

PointConstructionTool::PointConstructionTool(ToolId tool)
    : tool_(tool)
{
}

ToolId PointConstructionTool::id() const
{
    return tool_;
}

void PointConstructionTool::begin(ToolContext &context)
{
    pointLineWorldPoints_.clear();
    arcCenters_.clear();
    arcRadii_.clear();
    arcStartAngles_.clear();
    arcSweepAngles_.clear();
    arcFrames_.clear();
    arcOneEndpoints_.clear();
    arcIntersections_.clear();
    pointCenterSourceShapes_.clear();
    pointCenterSourceShapeCount_ = 0;
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    initialDrawingFrame_ = drawingFrame_;
    pointCenterFrame_ = {};
    edgeCenterFrame_ = {};
    frameBeforeAxisLock_ = {};
    cursorPoint_ = {};
    pointCenter_ = {};
    edgeCenter_ = {};
    pointLineWorldCursor_ = {};
    pointLineShiftDirection_ = {};
    pointLineAxisDirection_ = {};
    compassRadius_ = 0.0;
    compassRotation_ = 0.0;
    hasCursorPoint_ = false;
    hasPointCenter_ = false;
    hasEdgeCenter_ = false;
    planeLocked_ = false;
    angleSnapEnabled_ = true;
    perpendicularMode_ = false;
    pointLineShiftActive_ = false;
    hasPointLineAxisDirection_ = false;
    normalAxisLockKey_ = 0;
    pointLineAxisLockKey_ = 0;
    arcStage_ = 0;
    previousArcAngle_ = 0.0;
    hasPreviousArcAngle_ = false;
    numericInputTarget_ = NumericInputTarget::None;
    numericInput_.clear();
    lastToolInput_ = ToolInput{};
    pointLineSnap_ = SnapResult{};
    status_.state = ToolLifecycleState::Active;
    status_.canCommit = false;
    if (tool_ == ToolId::PointByLine) {
        status_.text = QStringLiteral("Point by Line: click points; Enter, Space, or right-click to finish");
    } else if (tool_ == ToolId::PointByArcs) {
        status_.text = QStringLiteral("Point by Arcs: pick the first arc center");
    } else if (tool_ == ToolId::PointCenter) {
        hasPointCenter_ = buildPointCenterPreview(context);
        status_.canCommit = hasPointCenter_;
        status_.text = hasPointCenter_
                           ? QStringLiteral("Point Center: click or press Enter to place the fitted center")
                           : QStringLiteral("Point Center: select at least three points or a curve, then activate the tool");
    } else if (tool_ == ToolId::PointEdgeCenter) {
        status_.text = QStringLiteral("Edge Center: hover an OSnap midpoint, then click");
    } else {
        status_.text = QStringLiteral("%1: click a curve").arg(toolName(tool_));
    }
    publish(context);
}

bool PointConstructionTool::handleMousePress(const ToolInput &input,
                                              ToolContext &context)
{
    if (tool_ == ToolId::PointByLine) {
        if (input.button == Qt::RightButton) {
            finishPointChain(context);
            return true;
        }
        if (input.button != Qt::LeftButton) {
            return false;
        }
        if (pointLineWorldPoints_.isEmpty()) {
            if (!planeLocked_) {
                drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                                    ? input.workPlaneFrame
                                    : context.viewportTransform().workPlaneFrame();
            }
            if (!isValidWorkPlaneFrame(drawingFrame_)) {
                return true;
            }
            initialDrawingFrame_ = drawingFrame_;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            planeLocked_ = true;
        }
        updatePointLineCursor(input, context);
        const Point3D target = pointLineWorldCursor_;
        if (!pointLineWorldPoints_.isEmpty() &&
            length3(subtract3(pointLineWorldPoints_.last(), target)) <= 1.0e-8) {
            return true;
        }
        pointLineWorldPoints_.append(target);
        hasCursorPoint_ = true;
        status_.canCommit = !pointLineWorldPoints_.isEmpty();
        status_.text = QStringLiteral("Point by Line: %1 points • Enter, Space, or right-click to finish")
                           .arg(pointLineWorldPoints_.size());
        pointLineShiftActive_ = false;
        if (pointLineWorldPoints_.size() > 1) {
            pointLineAxisLockKey_ = 0;
            hasPointLineAxisDirection_ = false;
            pointLineAxisDirection_ = {};
        }
        numericInput_.clear();
        numericInputTarget_ = NumericInputTarget::None;
        publish(context);
        return true;
    }

    if (tool_ == ToolId::PointByArcs && input.button == Qt::RightButton) {
        if (!arcIntersections_.isEmpty()) {
            commitArcIntersections(context);
        } else {
            cancel(context);
            context.finishTool(ToolId::Select);
        }
        return true;
    }
    if (tool_ == ToolId::PointCenter && input.button == Qt::RightButton) {
        commitPointCenter(context);
        return true;
    }
    if (tool_ == ToolId::PointEdgeCenter && input.button == Qt::RightButton) {
        handleMouseMove(input, context);
        if (!commitEdgeCenter(context)) {
            status_.state = ToolLifecycleState::Completed;
            status_.canCommit = false;
            status_.text = QStringLiteral("Edge Center finished without a point");
            publish(context);
            context.finishTool(ToolId::Select);
        }
        return true;
    }

    if (input.button != Qt::LeftButton) {
        return false;
    }
    if (tool_ == ToolId::PointCenter) {
        commitPointCenter(context);
        return true;
    }
    if (tool_ == ToolId::PointEdgeCenter) {
        handleMouseMove(input, context);
        commitEdgeCenter(context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs) {
        if (arcStage_ == 0 || arcStage_ == 3) {
            if (arcStage_ == 0) {
                drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                                    ? input.workPlaneFrame
                                    : context.viewportTransform().workPlaneFrame();
                if (!isValidWorkPlaneFrame(drawingFrame_)) {
                    return true;
                }
                initialDrawingFrame_ = drawingFrame_;
                arcFrames_.clear();
                arcFrames_.append(drawingFrame_);
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
                planeLocked_ = true;
            }
            QPointF center;
            if (arcStage_ == 0) {
                arcCenters_.clear();
                arcRadii_.clear();
                arcStartAngles_.clear();
                arcSweepAngles_.clear();
                arcFrames_.clear();
                arcFrames_.append(drawingFrame_);
                arcIntersections_.clear();
                center = input.positionInFrame(drawingFrame_);
            } else {
                const Point3D firstCenterWorld = workPlaneFramePointToWorld(
                    arcCenters_.first(), arcFrames_.first());
                Point3D centerWorld = input.resolvedWorldPoint();
                const Point3D normal = arcFrames_.first().normal;
                qreal planeOffset = dot3(subtract3(centerWorld, firstCenterWorld),
                                         normal);
                if (planeLocked_) {
                    centerWorld = subtract3(centerWorld,
                                            scale3(normal, planeOffset));
                    planeOffset = 0.0;
                }
                center = worldPointToWorkPlaneFrame(centerWorld, drawingFrame_);
                WorkPlaneFrame secondFrame = drawingFrame_;
                secondFrame.origin = add3(secondFrame.origin,
                                          scale3(normal, planeOffset));
                while (arcFrames_.size() <= 1) arcFrames_.append(secondFrame);
                arcFrames_[1] = secondFrame;
            }
            arcCenters_.append(center);
            arcStage_ = arcStage_ == 0 ? 1 : 4;
            status_.text = arcStage_ == 1
                               ? QStringLiteral("Point by Arcs: pick the first arc radius")
                               : QStringLiteral("Point by Arcs: pick the second arc radius");
            status_.canCommit = false;
            publish(context);
            return true;
        }
        if (arcStage_ == 1 || arcStage_ == 4) {
            updateArcPreview(input, context);
            const int arcIndex = arcStage_ == 1 ? 0 : 1;
            const QPointF delta = cursorPoint_ - arcCenters_[arcIndex];
            const qreal radius = std::hypot(delta.x(), delta.y());
            if (radius <= 1.0e-9) {
                status_.text = QStringLiteral("Pick a radius point away from the arc center");
                publish(context);
                return true;
            }
            while (arcRadii_.size() <= arcIndex) arcRadii_.append(0.0);
            while (arcStartAngles_.size() <= arcIndex) arcStartAngles_.append(0.0);
            while (arcSweepAngles_.size() <= arcIndex) arcSweepAngles_.append(0.0);
            arcRadii_[arcIndex] = radius;
            compassRotation_ = arcStartAngles_[arcIndex];
            arcSweepAngles_[arcIndex] = 0.0;
            previousArcAngle_ = arcStartAngles_[arcIndex];
            hasPreviousArcAngle_ = true;
            arcStage_ = arcStage_ == 1 ? 2 : 5;
            status_.text = arcStage_ == 2
                               ? QStringLiteral("Point by Arcs: sweep the first arc, then click")
                               : QStringLiteral("Point by Arcs: sweep the second arc, then click");
            numericInput_.clear();
            numericInputTarget_ = NumericInputTarget::None;
            publish(context);
            return true;
        }
        if (arcStage_ == 2 || arcStage_ == 5) {
            updateArcPreview(input, context);
            if (arcStage_ == 2) {
                const qreal endAngle = arcStartAngles_[0] + arcSweepAngles_[0];
                arcOneEndpoints_ = {
                    pointOnArc(arcCenters_[0], arcRadii_[0],
                               arcStartAngles_[0], 0.0, 0.0),
                    pointOnArc(arcCenters_[0], arcRadii_[0],
                               arcStartAngles_[0], endAngle - arcStartAngles_[0], 1.0)};
                arcStage_ = 3;
                status_.text = QStringLiteral("Point by Arcs: pick the second arc center");
                status_.canCommit = false;
                numericInput_.clear();
                numericInputTarget_ = NumericInputTarget::None;
                publish(context);
                return true;
            }
            updateArcIntersections(false);
            if (arcIntersections_.isEmpty()) {
                status_.text = QStringLiteral("The arcs do not intersect in their drawn spans");
                publish(context);
                return true;
            }
            commitArcIntersections(context);
            return true;
        }
    }
    const int shapeIndex = context.curveHitTester().hitTestShape(
        context.document(), input.screenPosition,
        context.viewportTransform(), input.viewportSize);
    if (shapeIndex < 0) {
        status_.text = QStringLiteral("Click a visible curve");
        publish(context);
        return true;
    }
    const Shape &shape = context.document()[shapeIndex];
    if (curvesForShape(shape).isEmpty()) {
        status_.text = QStringLiteral("That object does not contain a NURBS curve");
        publish(context);
        return true;
    }
    if (tool_ == ToolId::PointCenter &&
        shape.geometryType != GeometryType::Circle &&
        shape.geometryType != GeometryType::Arc) {
        status_.text = QStringLiteral("Point Center requires a circular NURBS curve");
        publish(context);
        return true;
    }

    QPointF point;
    WorkPlaneFrame frame;
    if (!addPointOnCurve(shape, input.worldPosition,
                         tool_ == ToolId::PointEdgeCenter,
                         &point, &frame)) {
        status_.text = tool_ == ToolId::PointCenter
                           ? QStringLiteral("Could not find a stable center for this curve")
                           : QStringLiteral("Could not locate a curve span under the cursor");
        publish(context);
        return true;
    }
    const Shape result = pointShape(point, frame);
    if (context.commitShape(tool_, result)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("%1 created").arg(toolName(tool_));
        publish(context);
        context.finishTool(ToolId::Select);
    }
    return true;
}

bool PointConstructionTool::handleMouseMove(const ToolInput &input,
                                            ToolContext &context)
{
    if (tool_ == ToolId::PointByLine) {
        updatePointLineCursor(input, context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs) {
        if (arcStage_ == 0 && !planeLocked_ &&
            isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
            initialDrawingFrame_ = drawingFrame_;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        if (arcStage_ == 1 || arcStage_ == 2 ||
            arcStage_ == 4 || arcStage_ == 5) {
            updateArcPreview(input, context);
        } else if (isValidWorkPlaneFrame(drawingFrame_)) {
            cursorPoint_ = input.positionInFrame(drawingFrame_);
            hasCursorPoint_ = true;
            const int compassArcIndex = arcStage_ >= 4 ? 1 : 0;
            const bool compassOnCursor = arcStage_ == 0;
            const WorkPlaneFrame compassFrame = !compassOnCursor &&
                                                        arcFrames_.size() > compassArcIndex
                                                    ? arcFrames_[compassArcIndex]
                                                    : drawingFrame_;
            const QPointF compassCenter = compassOnCursor
                                               ? cursorPoint_
                                               : (arcCenters_.size() > compassArcIndex
                                                      ? arcCenters_[compassArcIndex]
                                                      : cursorPoint_);
            const Point3D centerWorld = workPlaneFramePointToWorld(
                compassCenter, compassFrame);
            QPointF centerScreen;
            if (context.viewportTransform().worldPointToScreen(centerWorld,
                    input.viewportSize, &centerScreen)) {
                QPointF compassEdge;
                if (context.viewportTransform().screenToWorkPlane(
                        centerScreen + QPointF(125.0, 0.0), input.viewportSize,
                        compassFrame, &compassEdge)) {
                    compassRadius_ = std::hypot(compassEdge.x() - compassCenter.x(),
                                                compassEdge.y() - compassCenter.y());
                }
            }
            publish(context);
        }
        return true;
    }
    if (tool_ == ToolId::PointEdgeCenter) {
        hasEdgeCenter_ = false;
        if (input.snapType == SnapType::Midpoint && input.snapResult.isValid() &&
            input.snapResult.hasWorldPoint) {
            const Point3D worldPoint = input.snapResult.worldPoint;
            for (int shapeIndex = 0; shapeIndex < context.document().size(); ++shapeIndex) {
                const Shape &shape = context.document()[shapeIndex];
                if (!context.document().isObjectVisible(
                        context.document().objectIdAt(shapeIndex))) {
                    continue;
                }
                const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
                const QVector<SnapCandidate> candidates =
                    context.snapEngine().edgeCenterCandidatesForShape(shape);
                for (const SnapCandidate &candidate : candidates) {
                    if (candidate.type != SnapType::Midpoint) {
                        continue;
                    }
                    const Point3D candidateWorld =
                        workPlaneFramePointToWorld(candidate.point, frame);
                    if (length3(subtract3(candidateWorld, worldPoint)) <= 1.0e-6) {
                        edgeCenterFrame_ = frame;
                        edgeCenter_ = worldPointToWorkPlaneFrame(worldPoint, frame);
                        hasEdgeCenter_ = true;
                        break;
                    }
                }
                if (hasEdgeCenter_) {
                    break;
                }
            }
        }
        status_.canCommit = hasEdgeCenter_;
        status_.text = hasEdgeCenter_
                           ? QStringLiteral("Edge Center: click to place point")
                           : QStringLiteral("Edge Center: hover an OSnap midpoint, then click");
        publish(context);
        return true;
    }
    return false;
}

bool PointConstructionTool::handleKey(const ToolInput &input,
                                       ToolContext &context)
{
    if (tool_ == ToolId::PointByLine && input.key == Qt::Key_L) {
        if (planeLocked_) {
            planeLocked_ = false;
            normalAxisLockKey_ = 0;
        } else {
            const WorkPlaneFrame sourceFrame = isValidWorkPlaneFrame(initialDrawingFrame_)
                                                   ? initialDrawingFrame_
                                                   : input.workPlaneFrame;
            if (!pointLineWorldPoints_.isEmpty() &&
                isValidWorkPlaneFrame(sourceFrame)) {
                drawingFrame_ = makeWorkPlaneFrameFromNormal(
                    pointLineWorldPoints_.last(), sourceFrame.normal,
                    sourceFrame.xAxis);
            } else if (isValidWorkPlaneFrame(input.workPlaneFrame)) {
                drawingFrame_ = input.workPlaneFrame;
            }
            planeLocked_ = isValidWorkPlaneFrame(drawingFrame_);
            if (planeLocked_) {
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
        }
        status_.text = QStringLiteral("Point by Line: drawing plane %1")
                           .arg(planeLocked_ ? QStringLiteral("locked")
                                             : QStringLiteral("unlocked"));
        publish(context);
        return true;
    }

    const bool planeSelectable =
        (tool_ == ToolId::PointByArcs && arcStage_ == 0);
    if (planeSelectable && input.key == Qt::Key_L) {
        if (planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeAxisLock_)
                                ? frameBeforeAxisLock_
                                : input.workPlaneFrame;
            normalAxisLockKey_ = 0;
            planeLocked_ = false;
        } else {
            frameBeforeAxisLock_ = drawingFrame_;
            drawingFrame_ = input.workPlaneFrame;
            planeLocked_ = isValidWorkPlaneFrame(drawingFrame_);
        }
        if (isValidWorkPlaneFrame(drawingFrame_)) {
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        status_.text = QStringLiteral("%1: drawing plane %2")
                           .arg(toolName(tool_),
                                planeLocked_ ? QStringLiteral("locked")
                                             : QStringLiteral("unlocked"));
        publish(context);
        return true;
    }
    const bool axisKey = input.key == Qt::Key_X || input.key == Qt::Key_Y ||
                         input.key == Qt::Key_Z;
    if (planeSelectable && axisKey) {
        if (normalAxisLockKey_ == input.key) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeAxisLock_)
                                ? frameBeforeAxisLock_
                                : input.workPlaneFrame;
            normalAxisLockKey_ = 0;
            planeLocked_ = false;
        } else {
            if (normalAxisLockKey_ == 0) {
                frameBeforeAxisLock_ = drawingFrame_;
            }
            Point3D normal;
            if (input.key == Qt::Key_X) normal = {1.0, 0.0, 0.0};
            if (input.key == Qt::Key_Y) normal = {0.0, 1.0, 0.0};
            if (input.key == Qt::Key_Z) normal = {0.0, 0.0, 1.0};
            const Point3D origin = input.resolvedWorldPoint();
            const WorkPlaneFrame axisFrame = makeWorkPlaneFrameFromNormal(
                origin, normal, drawingFrame_.xAxis);
            if (isValidWorkPlaneFrame(axisFrame)) {
                drawingFrame_ = axisFrame;
                normalAxisLockKey_ = input.key;
                planeLocked_ = true;
            }
        }
        if (isValidWorkPlaneFrame(drawingFrame_)) {
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        status_.text = QStringLiteral("%1: plane constraint updated")
                           .arg(toolName(tool_));
        publish(context);
        return true;
    }

    if (tool_ == ToolId::PointByLine && axisKey) {
        if (pointLineAxisLockKey_ == input.key) {
            pointLineAxisLockKey_ = 0;
            hasPointLineAxisDirection_ = false;
        } else {
            pointLineAxisLockKey_ = input.key;
            Point3D worldAxis;
            if (input.key == Qt::Key_X) worldAxis = {1.0, 0.0, 0.0};
            if (input.key == Qt::Key_Y) worldAxis = {0.0, 1.0, 0.0};
            if (input.key == Qt::Key_Z) worldAxis = {0.0, 0.0, 1.0};
            pointLineAxisDirection_ = worldAxis;
            hasPointLineAxisDirection_ = true;
            pointLineShiftActive_ = false;
            status_.text = QStringLiteral("Point by Line: world %1 direction locked")
                               .arg(QChar(input.key));
        }
        publish(context);
        return true;
    }

    if (tool_ == ToolId::PointByArcs && input.key == Qt::Key_C) {
        angleSnapEnabled_ = !angleSnapEnabled_;
        hasPreviousArcAngle_ = false;
        status_.text = QStringLiteral("Point by Arcs: 15° angle snap %1")
                           .arg(angleSnapEnabled_ ? QStringLiteral("on")
                                                  : QStringLiteral("off"));
        publish(context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs && input.key == Qt::Key_P &&
        (arcStage_ == 1 || arcStage_ == 2 ||
         arcStage_ == 4 || arcStage_ == 5)) {
        togglePerpendicularPlane(input, context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs && input.key == Qt::Key_L) {
        planeLocked_ = !planeLocked_;
        if (planeLocked_ && !arcFrames_.isEmpty()) {
            const int index = arcStage_ >= 4 ? 1 : 0;
            if (arcFrames_.size() > index) {
                context.viewportTransform().setWorkPlaneFrame(arcFrames_[index]);
            }
        }
        status_.text = QStringLiteral("Point by Arcs: drawing plane %1")
                           .arg(planeLocked_ ? QStringLiteral("locked")
                                             : QStringLiteral("unlocked"));
        publish(context);
        return true;
    }

    if (input.key == Qt::Key_Escape) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }

    if (input.key == Qt::Key_Backspace && !numericInput_.isEmpty()) {
        numericInput_.chop(1);
        if (tool_ == ToolId::PointByLine) {
            updatePointLineCursor(input, context);
        } else {
            status_.text = QStringLiteral("Point by Arcs: input %1")
                               .arg(numericInput_);
            publish(context);
        }
        return true;
    }
    if (tool_ == ToolId::PointByLine &&
        (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) &&
        pointLineWorldPoints_.size() > 1) {
        pointLineWorldPoints_.removeLast();
        pointLineWorldCursor_ = pointLineWorldPoints_.last();
        cursorPoint_ = worldPointToWorkPlaneFrame(pointLineWorldCursor_, drawingFrame_);
        hasCursorPoint_ = true;
        status_.canCommit = true;
        status_.text = QStringLiteral("Point by Line: %1 points • Enter, Space, or right-click to finish")
                           .arg(pointLineWorldPoints_.size());
        publish(context);
        return true;
    }

    if ((input.key == Qt::Key_Return || input.key == Qt::Key_Enter) &&
        !numericInput_.isEmpty()) {
        if (applyNumericInput(context)) {
            if (tool_ == ToolId::PointByLine) {
                if (!pointLineWorldPoints_.isEmpty() &&
                    length3(subtract3(pointLineWorldPoints_.last(),
                                      pointLineWorldCursor_)) > 1.0e-8) {
                    pointLineWorldPoints_.append(pointLineWorldCursor_);
                    pointLineAxisLockKey_ = 0;
                    hasPointLineAxisDirection_ = false;
                    pointLineAxisDirection_ = {};
                    pointLineShiftActive_ = false;
                    status_.canCommit = true;
                }
                status_.text = QStringLiteral("Point by Line: %1 points • Enter, Space, or right-click to finish")
                                   .arg(pointLineWorldPoints_.size());
            }
            publish(context);
        }
        return true;
    }

    if (tool_ == ToolId::PointByLine &&
        (input.key == Qt::Key_Return || input.key == Qt::Key_Enter ||
         input.key == Qt::Key_Space)) {
        finishPointChain(context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs &&
        (input.key == Qt::Key_Return || input.key == Qt::Key_Enter ||
         input.key == Qt::Key_Space)) {
        if (!arcIntersections_.isEmpty()) {
            commitArcIntersections(context);
        } else {
            status_.state = ToolLifecycleState::Completed;
            status_.canCommit = false;
            status_.text = QStringLiteral("Point by Arcs finished without intersections");
            publish(context);
            context.finishTool(ToolId::Select);
        }
        return true;
    }
    if (tool_ == ToolId::PointByArcs &&
        (input.key == Qt::Key_R) && (arcStage_ == 1 || arcStage_ == 4)) {
        numericInputTarget_ = NumericInputTarget::Radius;
        numericInput_.clear();
        status_.text = QStringLiteral("Point by Arcs: type a radius, then press Enter");
        publish(context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs &&
        input.key == Qt::Key_A && (arcStage_ == 2 || arcStage_ == 5)) {
        numericInputTarget_ = NumericInputTarget::Angle;
        numericInput_.clear();
        status_.text = QStringLiteral("Point by Arcs: type a sweep angle in degrees, then press Enter");
        publish(context);
        return true;
    }
    if ((input.key == Qt::Key_Return || input.key == Qt::Key_Enter ||
         input.key == Qt::Key_Space) && tool_ == ToolId::PointCenter) {
        commitPointCenter(context);
        return true;
    }
    if ((input.key == Qt::Key_Return || input.key == Qt::Key_Enter ||
         input.key == Qt::Key_Space) && tool_ == ToolId::PointEdgeCenter) {
        if (!commitEdgeCenter(context)) {
            status_.state = ToolLifecycleState::Completed;
            status_.canCommit = false;
            status_.text = QStringLiteral("Edge Center finished without a point");
            publish(context);
            context.finishTool(ToolId::Select);
        }
        return true;
    }

    if (tool_ == ToolId::PointByLine && !pointLineWorldPoints_.isEmpty() &&
        (numericInputTarget_ == NumericInputTarget::Length ||
         numericInputTarget_ == NumericInputTarget::None)) {
        const QString text = input.text.toLower();
        bool accepted = !text.isEmpty();
        for (const QChar character : text) {
            if (!(character.isDigit() || character == QLatin1Char('.') ||
                  character == QLatin1Char(',') || character == QLatin1Char('-') ||
                  character == QLatin1Char('+') || character == QLatin1Char(' ') ||
                  character == QLatin1Char('m') || character == QLatin1Char('i') ||
                  character == QLatin1Char('n') || character == QLatin1Char('c') ||
                  character == QLatin1Char('u') || character == QLatin1Char('f') ||
                  character == QLatin1Char('t') || character == QLatin1Char('"') ||
                  character == QLatin1Char('\''))) {
                accepted = false;
                break;
            }
        }
        if (accepted) {
            numericInputTarget_ = NumericInputTarget::Length;
            numericInput_.append(text == QStringLiteral(",")
                                     ? QStringLiteral(".") : text);
            updatePointLineCursor(input, context);
            return true;
        }
    }
    if (tool_ == ToolId::PointByArcs &&
        (numericInputTarget_ == NumericInputTarget::Radius ||
         numericInputTarget_ == NumericInputTarget::Angle ||
         ((arcStage_ == 1 || arcStage_ == 4 || arcStage_ == 2 || arcStage_ == 5) &&
          numericInputTarget_ == NumericInputTarget::None))) {
        QString text = input.text.toLower();
        bool accepted = !text.isEmpty();
        for (const QChar character : text) {
            if (!(character.isDigit() || character == QLatin1Char('.') ||
                  character == QLatin1Char(',') || character == QLatin1Char('-') ||
                  character == QLatin1Char('+') || character == QLatin1Char(' ') ||
                  character == QLatin1Char('m') || character == QLatin1Char('i') ||
                  character == QLatin1Char('n') || character == QLatin1Char('c') ||
                  character == QLatin1Char('u') || character == QLatin1Char('f') ||
                  character == QLatin1Char('t') || character == QLatin1Char('"') ||
                  character == QLatin1Char('\''))) {
                accepted = false;
                break;
            }
        }
        if (accepted) {
            if (numericInputTarget_ == NumericInputTarget::None) {
                numericInputTarget_ = (arcStage_ == 1 || arcStage_ == 4)
                                          ? NumericInputTarget::Radius
                                          : NumericInputTarget::Angle;
            }
            numericInput_.append(text == QStringLiteral(",")
                                     ? QStringLiteral(".") : text);
            status_.text = QStringLiteral("Point by Arcs: input %1 • Enter to apply")
                               .arg(numericInput_);
            publish(context);
            return true;
        }
    }
    return false;
}

void PointConstructionTool::cancel(ToolContext &context)
{
    pointLineWorldPoints_.clear();
    arcCenters_.clear();
    arcRadii_.clear();
    arcStartAngles_.clear();
    arcSweepAngles_.clear();
    arcFrames_.clear();
    arcOneEndpoints_.clear();
    arcIntersections_.clear();
    pointCenterSourceShapes_.clear();
    pointCenterSourceShapeCount_ = 0;
    arcStage_ = 0;
    hasPreviousArcAngle_ = false;
    hasCursorPoint_ = false;
    hasPointCenter_ = false;
    hasEdgeCenter_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    pointLineAxisLockKey_ = 0;
    pointLineSnap_ = SnapResult{};
    numericInput_.clear();
    numericInputTarget_ = NumericInputTarget::None;
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview PointConstructionTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = planeLocked_;
    if (tool_ == ToolId::PointByArcs && planeLocked_ && !arcFrames_.isEmpty()) {
        const int index = arcStage_ >= 4 ? 1 : 0;
        if (arcFrames_.size() > index &&
            isValidWorkPlaneFrame(arcFrames_[index])) {
            result.workPlaneFrame = arcFrames_[index];
            result.hasWorkPlaneFrame = true;
        }
    }
    result.statusText = status_.text;

    if (tool_ == ToolId::PointByLine) {
        result.snap = pointLineSnap_;
        result.overridesSnap = true;
        result.worldPoints = pointLineWorldPoints_;
        for (const Point3D &point : pointLineWorldPoints_) {
            result.points.append(worldPointToWorkPlaneFrame(point, drawingFrame_));
        }
        for (int index = 1; index < pointLineWorldPoints_.size(); ++index) {
            const Point3D &first = pointLineWorldPoints_[index - 1];
            const Point3D &second = pointLineWorldPoints_[index];
            const WorkPlaneFrame segmentFrame = lineSegmentFrame(first, second,
                                                                  drawingFrame_);
            if (!isValidWorkPlaneFrame(segmentFrame)) continue;
            const qreal segmentLength = length3(subtract3(second, first));
            result.shapes.append(linePreviewShape(QPointF(0.0, 0.0),
                                                  QPointF(segmentLength, 0.0),
                                                  segmentFrame));
        }
        for (const Point3D &point : pointLineWorldPoints_) {
            appendPointMarker(&result.shapes, QPointF(0.0, 0.0),
                              pointFrameAt(point, drawingFrame_));
        }
        if (hasCursorPoint_) {
            if (!pointLineWorldPoints_.isEmpty()) {
                const Point3D &reference = pointLineWorldPoints_.last();
                const qreal segmentLength = length3(subtract3(pointLineWorldCursor_,
                                                               reference));
                if (segmentLength > 1.0e-8) {
                    const WorkPlaneFrame segmentFrame = lineSegmentFrame(
                        reference, pointLineWorldCursor_, drawingFrame_);
                    const Point3D direction = normalized3(subtract3(
                        pointLineWorldCursor_, reference));
                    const QColor color = hasPointLineAxisDirection_
                        ? pointLineAxisColor(direction)
                        : QColor(QStringLiteral("#151515"));
                    if (isValidWorkPlaneFrame(segmentFrame)) {
                        result.guides.append({QLineF(QPointF(0.0, 0.0),
                                                     QPointF(segmentLength, 0.0)),
                                              color, false, segmentFrame, true});
                    }
                }
            }
            const WorkPlaneFrame cursorFrame = pointFrameAt(pointLineWorldCursor_,
                                                            drawingFrame_);
            appendPointMarker(&result.shapes, QPointF(0.0, 0.0), cursorFrame);
            result.worldCursorPoint = pointLineWorldCursor_;
            result.cursorPoint = worldPointToWorkPlaneFrame(pointLineWorldCursor_,
                                                             drawingFrame_);
            result.hasCursorPoint = true;
        }
        result.hudInstructionsLine = QStringLiteral("Click points • Shift locks direction • X/Y/Z axis • type length + Enter • L plane lock • Enter/Space/RMB finishes");
    } else if (tool_ == ToolId::PointByArcs) {
        result.activeStage = arcStage_;
        const auto hasArcData = [this](int index) {
            return arcCenters_.size() > index && arcRadii_.size() > index &&
                   arcStartAngles_.size() > index && arcSweepAngles_.size() > index &&
                   arcRadii_[index] > 1.0e-9;
        };
        const auto appendArc = [&](int index) {
            if (!hasArcData(index)) return;
            const WorkPlaneFrame frame = arcFrames_.size() > index
                                             ? arcFrames_[index]
                                             : drawingFrame_;
            const Shape arc = arcPreviewShape(arcCenters_[index],
                                               arcRadii_[index],
                                               arcStartAngles_[index],
                                               arcSweepAngles_[index],
                                               frame);
            if (validateNurbsCurve(arc.nurbs)) result.shapes.append(arc);
        };

        if (arcStage_ >= 3) appendArc(0);
        if (arcStage_ == 2) appendArc(0);
        if (arcStage_ == 5) appendArc(1);

        for (const QPointF &intersection : arcIntersections_) {
            if (arcStage_ == 5) {
                appendPointMarker(&result.shapes, intersection, drawingFrame_);
            }
            if (arcStage_ == 4) {
                const qreal markerHalfSize = compassRadius_ * 0.2;
                if (markerHalfSize <= 1.0e-8) continue;
                result.guides.append({
                    QLineF(intersection - QPointF(markerHalfSize, 0.0),
                           intersection + QPointF(markerHalfSize, 0.0)),
                    QColor(Qt::black), false});
                result.guides.append({
                    QLineF(intersection - QPointF(0.0, markerHalfSize),
                           intersection + QPointF(0.0, markerHalfSize)),
                    QColor(Qt::black), false});
            }
        }
        if (arcStage_ == 1 || arcStage_ == 4) {
            const int index = arcStage_ == 1 ? 0 : 1;
            if (arcCenters_.size() > index && hasCursorPoint_) {
                const WorkPlaneFrame frame = arcFrames_.size() > index
                                                 ? arcFrames_[index]
                                                 : drawingFrame_;
                result.guides.append({QLineF(arcCenters_[index], cursorPoint_),
                                      QColor(QStringLiteral("#808080")), false,
                                      frame, true});
            }
        } else if (arcStage_ == 2 || arcStage_ == 5) {
            const int index = arcStage_ == 2 ? 0 : 1;
            if (arcCenters_.size() > index && hasArcData(index)) {
                const WorkPlaneFrame frame = arcFrames_.size() > index
                                                 ? arcFrames_[index]
                                                 : drawingFrame_;
                const QPointF start = pointOnArc(arcCenters_[index], arcRadii_[index],
                                                 arcStartAngles_[index], 0.0, 0.0);
                const QPointF end = pointOnArc(arcCenters_[index], arcRadii_[index],
                                               arcStartAngles_[index],
                                               arcSweepAngles_[index], 1.0);
                result.guides.append({QLineF(arcCenters_[index], start),
                                      QColor(QStringLiteral("#808080")), false,
                                      frame, true});
                result.guides.append({QLineF(arcCenters_[index], end),
                                      QColor(QStringLiteral("#808080")), false,
                                      frame, true});
            }
        }
        if (!arcOneEndpoints_.isEmpty()) {
            result.points = arcOneEndpoints_;
            for (const QPointF &endpoint : arcOneEndpoints_) {
                result.worldPoints.append(workPlaneFramePointToWorld(endpoint,
                    arcFrames_.isEmpty() ? drawingFrame_ : arcFrames_.first()));
            }
        }
        result.cursorPoint = cursorPoint_;
        result.hasCursorPoint = hasCursorPoint_;
        const int compassArcIndex = arcStage_ >= 4 ? 1 : 0;
        const bool cursorCompass = arcStage_ == 0 && hasCursorPoint_;
        const bool hasCompassCenter = cursorCompass ||
                                      arcCenters_.size() > compassArcIndex;
        if (hasCompassCenter && compassRadius_ > 1.0e-8) {
            const WorkPlaneFrame compassFrame =
                arcFrames_.size() > compassArcIndex
                    ? arcFrames_[compassArcIndex] : drawingFrame_;
            const QPointF center = cursorCompass
                                       ? cursorPoint_
                                       : arcCenters_[compassArcIndex];
            const qreal cosine = std::cos(compassRotation_);
            const qreal sine = std::sin(compassRotation_);
            const auto rotatedPoint = [&](qreal angle, qreal radius) {
                const qreal x = radius * std::cos(angle);
                const qreal y = radius * std::sin(angle);
                return center + QPointF(x * cosine - y * sine,
                                        x * sine + y * cosine);
            };
            const QColor compassColor(Qt::black);
            const qreal outerRadius = compassRadius_;
            const qreal innerRadius = outerRadius * (80.0 / 120.0);
            const qreal tickLength = outerRadius * (10.0 / 120.0);
            const qreal crossLength = outerRadius * (10.0 / 120.0);
            constexpr int compassSegments = 72;
            for (int index = 0; index < compassSegments; ++index) {
                const qreal firstAngle = kTwoPi * index / compassSegments;
                const qreal secondAngle = kTwoPi * (index + 1) / compassSegments;
                result.guides.append({QLineF(rotatedPoint(firstAngle, outerRadius),
                                             rotatedPoint(secondAngle, outerRadius)),
                                      compassColor, false, compassFrame, true});
            }
            for (int index = 0; index < 24; ++index) {
                const qreal angle = kTwoPi * index / 24.0;
                result.guides.append({QLineF(rotatedPoint(angle, outerRadius - tickLength),
                                             rotatedPoint(angle, outerRadius)),
                                      QColor(Qt::black), false,
                                      compassFrame, true});
            }
            const auto appendInnerArc = [&](qreal startAngle, qreal endAngle) {
                constexpr int arcSegments = 48;
                for (int index = 0; index < arcSegments; ++index) {
                    const qreal firstAngle = startAngle +
                        (endAngle - startAngle) * index / arcSegments;
                    const qreal secondAngle = startAngle +
                        (endAngle - startAngle) * (index + 1) / arcSegments;
                    result.guides.append({QLineF(rotatedPoint(firstAngle, innerRadius),
                                                 rotatedPoint(secondAngle, innerRadius)),
                                          compassColor, false,
                                          compassFrame, true});
                }
            };
            appendInnerArc(200.0 * kPi / 180.0, 340.0 * kPi / 180.0);
            appendInnerArc(20.0 * kPi / 180.0, 160.0 * kPi / 180.0);
            result.guides.append({QLineF(rotatedPoint(kPi, crossLength),
                                         rotatedPoint(0.0, crossLength)),
                                  compassColor, false, compassFrame, true});
            result.guides.append({QLineF(rotatedPoint(1.5 * kPi, crossLength),
                                         rotatedPoint(0.5 * kPi, crossLength)),
                                  compassColor, false, compassFrame, true});
        }
        switch (arcStage_) {
        case 0:
            result.hudInstructionsLine = QStringLiteral("Click first arc center • Esc cancels");
            break;
        case 1:
        case 4:
            result.hudInstructionsLine = QStringLiteral("Radius • R numeric • P perpendicular • L plane lock • C 15° snap");
            break;
        case 2:
        case 5:
            result.hudInstructionsLine = QStringLiteral("Sweep • A angle • P perpendicular • L plane lock • C 15° snap");
            break;
        case 3:
            result.hudInstructionsLine = QStringLiteral("Click arc 2 center • L plane lock • Esc cancels");
            break;
        default:
            result.hudInstructionsLine = QStringLiteral("Enter/Space/RMB finishes • Esc cancels");
            break;
        }
    } else if (tool_ == ToolId::PointCenter && hasPointCenter_) {
        result.workPlaneFrame = pointCenterFrame_;
        result.hasWorkPlaneFrame = isValidWorkPlaneFrame(pointCenterFrame_);
        for (int index = pointCenterSourceShapeCount_;
             index < pointCenterSourceShapes_.size(); ++index) {
            result.shapes.append(pointCenterSourceShapes_[index]);
        }
        appendPointMarker(&result.shapes, pointCenter_, pointCenterFrame_);
        result.hudInstructionsLine = QStringLiteral("Click to place the fitted center • Enter confirms • Esc cancels");
    } else if (tool_ == ToolId::PointEdgeCenter && hasEdgeCenter_) {
        result.workPlaneFrame = edgeCenterFrame_;
        result.hasWorkPlaneFrame = isValidWorkPlaneFrame(edgeCenterFrame_);
        appendPointMarker(&result.shapes, edgeCenter_, edgeCenterFrame_);
        result.hudInstructionsLine = QStringLiteral("Hover midpoint • Click place • Esc cancels");
    }
    result.statusText = status_.text;
    return result;
}

ToolStatus PointConstructionTool::status() const
{
    return status_;
}

bool PointConstructionTool::addPointOnCurve(const Shape &shape,
                                             const QPointF &cursor,
                                             bool edgeCenter,
                                             QPointF *point,
                                             WorkPlaneFrame *frame) const
{
    if (point == nullptr || frame == nullptr) {
        return false;
    }
    *frame = shapeWorkPlaneFrame(shape);
    const QVector<Shape::NurbsCurve2D> sourceCurves = curvesForShape(shape);
    if (!isValidWorkPlaneFrame(*frame) || sourceCurves.isEmpty()) {
        return false;
    }

    QVector<QPointF> allSamples;
    qreal bestDistanceSquared = std::numeric_limits<qreal>::infinity();
    QPointF spanMidpoint;
    for (const Shape::NurbsCurve2D &source : sourceCurves) {
        if (source.dimension != 2) {
            return false;
        }
        Shape::NurbsCurve2D curve;
        if (!mapCurveToFrame(shape, source, *frame, &curve)) {
            return false;
        }
        const QVector<QPointF> samples = sampleCurve(curve, 96);
        if (samples.size() < 3) {
            continue;
        }
        allSamples += samples;
        qreal start = 0.0;
        qreal end = 0.0;
        if (!nurbsParameterDomain(curve, &start, &end)) {
            continue;
        }
        if (edgeCenter) {
            const QVector<double> knots = expandedNurbsKnotVector(curve);
            for (int knotIndex = curve.degree;
                 knotIndex < curve.controlPoints.size(); ++knotIndex) {
                const qreal spanStart = knots[knotIndex];
                const qreal spanEnd = knots[knotIndex + 1];
                if (spanEnd - spanStart <= 1.0e-12) {
                    continue;
                }
                for (int sample = 0; sample <= 12; ++sample) {
                    const qreal parameter = spanStart +
                        (spanEnd - spanStart) * sample / 12.0;
                    QPointF evaluated;
                    if (!evaluateNurbsPoint(curve, parameter, &evaluated)) {
                        continue;
                    }
                    const QPointF delta = evaluated - cursor;
                    const qreal distanceSquared = QPointF::dotProduct(delta, delta);
                    if (distanceSquared < bestDistanceSquared) {
                        bestDistanceSquared = distanceSquared;
                        evaluateNurbsPoint(curve, (spanStart + spanEnd) * 0.5,
                                            &spanMidpoint);
                    }
                }
            }
        } else {
            for (int sample = 1; sample < samples.size(); ++sample) {
                const QPointF startPoint = samples[sample - 1];
                const QPointF segment = samples[sample] - startPoint;
                const qreal lengthSquared = QPointF::dotProduct(segment, segment);
                if (lengthSquared <= 1.0e-18) {
                    continue;
                }
                const qreal fraction = std::clamp(
                    QPointF::dotProduct(cursor - startPoint, segment) / lengthSquared,
                    0.0, 1.0);
                const QPointF candidate = startPoint + segment * fraction;
                const QPointF delta = candidate - cursor;
                const qreal distanceSquared = QPointF::dotProduct(delta, delta);
                if (distanceSquared < bestDistanceSquared) {
                    bestDistanceSquared = distanceSquared;
                }
            }
        }
    }

    if (edgeCenter) {
        if (!std::isfinite(bestDistanceSquared)) {
            return false;
        }
        *point = spanMidpoint;
        return true;
    }
    QPointF fittedCenter;
    if (!solveCircleCenter(allSamples, &fittedCenter)) {
        return false;
    }
    *point = fittedCenter;
    return true;
}

bool PointConstructionTool::buildPointCenterPreview(ToolContext &context)
{
    QVector<Point3D> worldPoints;
    const QVector<ObjectId> selectedIds = context.selection().objectIds();
    const auto appendCurvePoints = [&](const Shape &shape) {
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        if (!isValidWorkPlaneFrame(frame)) return false;
        bool contributed = false;
        for (const Shape::NurbsCurve2D &curve : curvesForShape(shape)) {
            const QVector<QPointF> samples = sampleCurve(curve, 128);
            if (samples.size() < 3) continue;
            contributed = true;
            for (const QPointF &point : samples) {
                worldPoints.append(workPlaneFramePointToWorld(point, frame));
            }
        }
        return contributed;
    };

    // The add-on fits the first selected edge chain. It only falls back to
    // selected vertices when no selected curve can provide a chain.
    bool foundSelectedCurve = false;
    for (const ObjectId objectId : selectedIds) {
        const Shape *shape = context.document().shape(objectId);
        if (shape == nullptr || !context.document().isObjectVisible(objectId) ||
            !context.document().isObjectEditable(objectId) ||
            shape->geometryType == GeometryType::Point) {
            continue;
        }
        if (curvesForShape(*shape).isEmpty()) continue;
        pointCenterSourceShapes_.clear();
        worldPoints.clear();
        if (appendCurvePoints(*shape)) {
            pointCenterSourceShapes_.append(*shape);
            foundSelectedCurve = true;
            break;
        }
    }

    if (!foundSelectedCurve) {
        worldPoints.clear();
        pointCenterSourceShapes_.clear();
        for (const ObjectId objectId : selectedIds) {
            const Shape *shape = context.document().shape(objectId);
            if (shape == nullptr || !context.document().isObjectVisible(objectId) ||
                !context.document().isObjectEditable(objectId) ||
                shape->geometryType != GeometryType::Point) {
                continue;
            }
            const WorkPlaneFrame frame = shapeWorkPlaneFrame(*shape);
            if (!isValidWorkPlaneFrame(frame)) continue;
            for (const QPointF &point : shape->points) {
                worldPoints.append(workPlaneFramePointToWorld(point, frame));
            }
            pointCenterSourceShapes_.append(*shape);
        }
    }
    if (worldPoints.size() < 3) {
        return false;
    }
    pointCenterSourceShapeCount_ = pointCenterSourceShapes_.size();

    QVector<QPointF> localPoints;
    if (!fitWorldPlane(worldPoints, &pointCenterFrame_, &localPoints) ||
        !solveCircleCenter(localPoints, &pointCenter_)) {
        return false;
    }

    qreal radiusSum = 0.0;
    QVector<qreal> angles;
    angles.reserve(localPoints.size());
    for (const QPointF &point : localPoints) {
        const QPointF delta = point - pointCenter_;
        radiusSum += std::hypot(delta.x(), delta.y());
        angles.append(std::atan2(delta.y(), delta.x()));
    }
    const qreal averageRadius = radiusSum / localPoints.size();
    if (averageRadius <= 1.0e-9) {
        return false;
    }
    qreal maximumDeviation = 0.0;
    for (const QPointF &point : localPoints) {
        const QPointF delta = point - pointCenter_;
        maximumDeviation = std::max(maximumDeviation,
                                    std::abs(std::hypot(delta.x(), delta.y()) -
                                             averageRadius));
    }

    std::sort(angles.begin(), angles.end());
    qreal startAngle = angles.first();
    qreal sweepAngle = angles.last() - angles.first();
    const qreal averageGap = kTwoPi / angles.size();
    const qreal wrapGap = kTwoPi - sweepAngle;
    bool isClosed = false;
    for (const Shape &source : pointCenterSourceShapes_) {
        isClosed |= source.geometryType == GeometryType::Circle ||
                    source.geometryType == GeometryType::Rectangle ||
                    source.geometryType == GeometryType::Polygon;
    }
    isClosed |= wrapGap < averageGap * 1.5;
    if (maximumDeviation / averageRadius < 0.001) {
        if (isClosed) {
            startAngle = 0.0;
            sweepAngle = kTwoPi;
        }
        Shape fitted = arcPreviewShape(pointCenter_, averageRadius,
                                       startAngle, sweepAngle, pointCenterFrame_);
        if (validateNurbsCurve(fitted.nurbs)) {
            pointCenterSourceShapes_.append(fitted);
        }
    } else {
        const QVector<QPointF> outline = catmullRomPreview(localPoints, isClosed);
        if (outline.size() >= 2) {
            pointCenterSourceShapes_.append(
                polylinePreviewShape(outline, pointCenterFrame_));
        }
    }
    return true;
}

void PointConstructionTool::updateArcPreview(const ToolInput &input,
                                             ToolContext &context)
{
    if (arcStage_ <= 0 || arcStage_ > 5 || !isValidWorkPlaneFrame(drawingFrame_)) {
        return;
    }
    const int arcIndex = arcStage_ >= 4 ? 1 : 0;
    if (arcCenters_.size() <= arcIndex || arcFrames_.size() <= arcIndex ||
        !isValidWorkPlaneFrame(arcFrames_[arcIndex])) {
        return;
    }
    const WorkPlaneFrame &arcFrame = arcFrames_[arcIndex];
    QPointF target = worldPointToWorkPlaneFrame(input.resolvedWorldPoint(),
                                                 arcFrame);
    const QPointF delta = target - arcCenters_[arcIndex];
    const qreal distance = std::hypot(delta.x(), delta.y());
    if (distance <= 1.0e-10) {
        cursorPoint_ = target;
        hasCursorPoint_ = true;
        publish(context);
        return;
    }
    const bool geometrySnap = input.snapResult.isValid();
    const qreal rawAngle = std::atan2(delta.y(), delta.x());

    if (arcStage_ == 1 || arcStage_ == 4) {
        qreal angle = rawAngle;
        if (angleSnapEnabled_ && !geometrySnap) {
            angle = softSnappedAngle(angle);
        }
        while (arcRadii_.size() <= arcIndex) arcRadii_.append(0.0);
        while (arcStartAngles_.size() <= arcIndex) arcStartAngles_.append(0.0);
        while (arcSweepAngles_.size() <= arcIndex) arcSweepAngles_.append(0.0);
        arcRadii_[arcIndex] = distance;
        arcStartAngles_[arcIndex] = angle;
        compassRotation_ = angle;
        arcSweepAngles_[arcIndex] = 0.0;
        cursorPoint_ = arcCenters_[arcIndex] +
                       QPointF(std::cos(angle), std::sin(angle)) * distance;
        previousArcAngle_ = angle;
        hasPreviousArcAngle_ = true;
        if (arcStage_ == 4) updateArcIntersections(true);
    } else {
        if (angleSnapEnabled_ && !geometrySnap) {
            target = arcCenters_[arcIndex] +
                     QPointF(std::cos(softSnappedAngle(rawAngle)),
                             std::sin(softSnappedAngle(rawAngle))) * distance;
        }
        const QPointF sweepDelta = target - arcCenters_[arcIndex];
        const qreal angle = std::atan2(sweepDelta.y(), sweepDelta.x());
        if (!hasPreviousArcAngle_) {
            previousArcAngle_ = arcStartAngles_[arcIndex];
            hasPreviousArcAngle_ = true;
        }
        qreal change = angle - previousArcAngle_;
        while (change > kPi) change -= kTwoPi;
        while (change < -kPi) change += kTwoPi;
        arcSweepAngles_[arcIndex] += change;
        previousArcAngle_ = angle;
        const qreal endAngle = arcStartAngles_[arcIndex] + arcSweepAngles_[arcIndex];
        cursorPoint_ = arcCenters_[arcIndex] +
                       QPointF(std::cos(endAngle), std::sin(endAngle)) *
                           arcRadii_[arcIndex];
        if (arcStage_ == 5) updateArcIntersections(false);
    }
    hasCursorPoint_ = true;

    const Point3D centerWorld = workPlaneFramePointToWorld(
        arcCenters_[arcIndex], arcFrame);
    QPointF centerScreen;
    if (context.viewportTransform().worldPointToScreen(centerWorld,
                                                       input.viewportSize,
                                                       &centerScreen)) {
        QPointF compassEdge;
        if (context.viewportTransform().screenToWorkPlane(
                centerScreen + QPointF(125.0, 0.0), input.viewportSize,
                arcFrame, &compassEdge)) {
            compassRadius_ = std::hypot(compassEdge.x() - arcCenters_[arcIndex].x(),
                                        compassEdge.y() - arcCenters_[arcIndex].y());
        }
    }
    status_.canCommit = arcStage_ == 5 && !arcIntersections_.isEmpty();
    publish(context);
}

void PointConstructionTool::updateArcIntersections(bool secondArcIsFullCircle)
{
    arcIntersections_.clear();
    if (arcCenters_.size() < 2 || arcRadii_.size() < 2 ||
        arcStartAngles_.size() < 2 || arcSweepAngles_.size() < 2 ||
        arcRadii_[0] <= 1.0e-9 || arcRadii_[1] <= 1.0e-9) {
        return;
    }
    const QPointF offset = arcCenters_[1] - arcCenters_[0];
    const qreal distance = std::hypot(offset.x(), offset.y());
    if (distance <= 1.0e-12 || distance > arcRadii_[0] + arcRadii_[1] + 1.0e-9 ||
        distance < std::abs(arcRadii_[0] - arcRadii_[1]) - 1.0e-9) {
        return;
    }
    const qreal along = (arcRadii_[0] * arcRadii_[0] -
                         arcRadii_[1] * arcRadii_[1] + distance * distance) /
                        (2.0 * distance);
    const qreal height = std::sqrt(std::max<qreal>(
        0.0, arcRadii_[0] * arcRadii_[0] - along * along));
    const QPointF direction = offset / distance;
    const QPointF base = arcCenters_[0] + direction * along;
    const QPointF normal(-direction.y(), direction.x());
    const QPointF candidates[] = {base + normal * height, base - normal * height};
    for (int index = 0; index < (height <= 1.0e-9 ? 1 : 2); ++index) {
        const QPointF candidate = candidates[index];
        const qreal firstAngle = std::atan2(candidate.y() - arcCenters_[0].y(),
                                            candidate.x() - arcCenters_[0].x());
        const qreal secondAngle = std::atan2(candidate.y() - arcCenters_[1].y(),
                                             candidate.x() - arcCenters_[1].x());
        if (angleOnArc(firstAngle, arcStartAngles_[0], arcSweepAngles_[0]) &&
            (secondArcIsFullCircle ||
             angleOnArc(secondAngle, arcStartAngles_[1], arcSweepAngles_[1]))) {
            appendUnique(&arcIntersections_, candidate);
        }
    }
}

bool PointConstructionTool::commitArcIntersections(ToolContext &context)
{
    if (arcIntersections_.isEmpty()) {
        status_.text = QStringLiteral("The arcs do not intersect in their drawn spans");
        status_.canCommit = false;
        publish(context);
        return false;
    }
    QVector<Shape> points;
    points.reserve(arcIntersections_.size());
    for (const QPointF &point : arcIntersections_) {
        points.append(pointShape(point, drawingFrame_));
    }
    if (!context.commitShapes(tool_, points)) {
        return false;
    }
    status_.state = ToolLifecycleState::Completed;
    status_.canCommit = false;
    status_.text = QStringLiteral("Created %1 arc intersection point(s)")
                       .arg(points.size());
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

bool PointConstructionTool::commitPointCenter(ToolContext &context)
{
    if (!hasPointCenter_) {
        status_.state = ToolLifecycleState::Completed;
        status_.canCommit = false;
        status_.text = QStringLiteral("Point Center finished without a valid center");
        publish(context);
        context.finishTool(ToolId::Select);
        return true;
    }
    if (!context.commitShape(tool_, pointShape(pointCenter_, pointCenterFrame_))) {
        return false;
    }
    status_.state = ToolLifecycleState::Completed;
    status_.canCommit = false;
    status_.text = QStringLiteral("Point Center created");
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

bool PointConstructionTool::commitEdgeCenter(ToolContext &context)
{
    if (!hasEdgeCenter_ || !isValidWorkPlaneFrame(edgeCenterFrame_)) {
        status_.text = QStringLiteral("Hover an OSnap midpoint before placing an Edge Center point");
        publish(context);
        return false;
    }
    if (!context.commitShape(tool_, pointShape(edgeCenter_, edgeCenterFrame_))) {
        return false;
    }
    status_.state = ToolLifecycleState::Completed;
    status_.canCommit = false;
    status_.text = QStringLiteral("Edge Center point created");
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

void PointConstructionTool::updatePointLineCursor(const ToolInput &input,
                                                  ToolContext &context)
{
    lastToolInput_ = input;
    if (pointLineWorldPoints_.isEmpty()) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
            initialDrawingFrame_ = drawingFrame_;
        }
        pointLineWorldCursor_ = input.resolvedWorldPoint();
        pointLineSnap_ = input.snapResult;
        if (pointLineSnap_.isValid() && pointLineSnap_.hasWorldPoint) {
            QPointF targetScreen;
            if (context.viewportTransform().worldPointToScreen(
                    pointLineSnap_.worldPoint, input.viewportSize, &targetScreen)) {
                pointLineSnap_.point = context.viewportTransform().screenToWorld(
                    targetScreen, input.viewportSize);
            }
        }
        cursorPoint_ = worldPointToWorkPlaneFrame(pointLineWorldCursor_, drawingFrame_);
        hasCursorPoint_ = true;
        publish(context);
        return;
    }

    const Point3D reference = pointLineWorldPoints_.last();
    const bool hasGeometrySnap = input.snapResult.isValid() &&
                                 input.snapResult.hasWorldPoint;
    Point3D source = input.resolvedWorldPoint();

    const auto inferredWorldAxis = [&]() {
        QPointF referenceScreen;
        if (!context.viewportTransform().worldPointToScreen(
                reference, input.viewportSize, &referenceScreen)) {
            return Point3D{};
        }
        const QPointF screenDelta = input.screenPosition - referenceScreen;
        const qreal screenLength = std::hypot(screenDelta.x(), screenDelta.y());
        if (screenLength <= 1.0) return Point3D{};
        const qreal threshold = std::cos(6.0 * kPi / 180.0);
        qreal bestAlignment = threshold;
        Point3D bestAxis;
        for (const Point3D axis : {Point3D{1.0, 0.0, 0.0},
                                   Point3D{0.0, 1.0, 0.0},
                                   Point3D{0.0, 0.0, 1.0}}) {
            QPointF axisScreen;
            const Point3D axisPoint = add3(reference, axis);
            if (!context.viewportTransform().worldPointToScreen(
                    axisPoint, input.viewportSize, &axisScreen)) {
                continue;
            }
            const QPointF projectedAxis = axisScreen - referenceScreen;
            const qreal projectedLength = std::hypot(projectedAxis.x(),
                                                      projectedAxis.y());
            if (projectedLength <= 1.0e-8) continue;
            const qreal alignment = std::abs(QPointF::dotProduct(
                screenDelta, projectedAxis) / (screenLength * projectedLength));
            if (alignment >= bestAlignment) {
                bestAlignment = alignment;
                bestAxis = axis;
            }
        }
        return bestAxis;
    };

    Point3D constraintDirection;
    if (pointLineAxisLockKey_ != 0 && hasPointLineAxisDirection_) {
        constraintDirection = pointLineAxisDirection_;
    } else if (input.modifiers.testFlag(Qt::ShiftModifier)) {
        if (!pointLineShiftActive_) {
            pointLineShiftDirection_ = inferredWorldAxis();
            if (length3(pointLineShiftDirection_) <= 1.0e-8) {
                pointLineShiftDirection_ = normalized3(subtract3(source, reference));
            }
            pointLineShiftActive_ = length3(pointLineShiftDirection_) > 1.0e-8;
            if (pointLineShiftActive_) {
                pointLineAxisDirection_ = pointLineShiftDirection_;
                hasPointLineAxisDirection_ = true;
            }
        }
        constraintDirection = pointLineShiftDirection_;
    } else {
        pointLineShiftActive_ = false;
        pointLineShiftDirection_ = {};
        if (!hasGeometrySnap) {
            constraintDirection = inferredWorldAxis();
            if (pointLineAxisLockKey_ == 0) {
                hasPointLineAxisDirection_ = length3(constraintDirection) > 1.0e-8;
                pointLineAxisDirection_ = constraintDirection;
            }
        } else if (pointLineAxisLockKey_ == 0) {
            hasPointLineAxisDirection_ = false;
            pointLineAxisDirection_ = {};
        }
    }

    Point3D target = source;
    const bool explicitAxisConstraint = pointLineAxisLockKey_ != 0 &&
                                        hasPointLineAxisDirection_;
    const bool activeDirectionConstraint = explicitAxisConstraint ||
                                           pointLineShiftActive_;
    if (length3(constraintDirection) > 1.0e-8 &&
        (activeDirectionConstraint || !hasGeometrySnap)) {
        constraintDirection = normalized3(constraintDirection);
        bool foundEdgeIntersection = false;
        const SnapSettings &snapSettings = context.snapEngine().settings();
        if (activeDirectionConstraint && hasGeometrySnap &&
            (snapSettings.near || snapSettings.intersection)) {
            foundEdgeIntersection =
                context.snapEngine().findAxisIntersectionWithHoveredEdge(
                    context.document(), input.screenPosition, reference,
                    constraintDirection, source, context.viewportTransform(),
                    input.viewportSize, &target, 12.0);
        }
        if (activeDirectionConstraint && hasGeometrySnap &&
            !foundEdgeIntersection) {
            const qreal distance = dot3(subtract3(source, reference),
                                        constraintDirection);
            target = add3(reference, scale3(constraintDirection, distance));
        } else {
            Point3D axisPoint;
            if (context.viewportTransform().screenToWorldAxis(
                    input.screenPosition, input.viewportSize, reference,
                    constraintDirection, &axisPoint)) {
                target = axisPoint;
            } else {
                const qreal distance = dot3(subtract3(source, reference),
                                            constraintDirection);
                target = add3(reference, scale3(constraintDirection, distance));
            }
        }
    }

    if (numericInputTarget_ == NumericInputTarget::Length && !numericInput_.isEmpty()) {
        qreal length = 0.0;
        if (parseDocumentLengthInput(numericInput_,
                                     context.document().settings().lengthUnit,
                                     &length)) {
            Point3D unitDirection = normalized3(subtract3(target, reference));
            if (length3(unitDirection) <= 1.0e-10 &&
                length3(constraintDirection) > 1.0e-10) {
                unitDirection = normalized3(constraintDirection);
            }
            if (length3(unitDirection) <= 1.0e-10 &&
                pointLineWorldPoints_.size() > 1) {
                unitDirection = normalized3(subtract3(
                    reference, pointLineWorldPoints_[pointLineWorldPoints_.size() - 2]));
            }
            if (length3(unitDirection) <= 1.0e-10) {
                unitDirection = drawingFrame_.xAxis;
            }
            target = add3(reference, scale3(unitDirection, std::abs(length)));
        }
    }

    pointLineWorldCursor_ = target;
    pointLineSnap_ = SnapResult{};
    if (hasGeometrySnap && numericInput_.isEmpty()) {
        pointLineSnap_ = input.snapResult;
        pointLineSnap_.worldPoint = target;
        pointLineSnap_.hasWorldPoint = true;
        QPointF targetScreen;
        if (context.viewportTransform().worldPointToScreen(
                target, input.viewportSize, &targetScreen)) {
            pointLineSnap_.point = context.viewportTransform().screenToWorld(
                targetScreen, input.viewportSize);
        }
    }
    cursorPoint_ = worldPointToWorkPlaneFrame(target, drawingFrame_);
    hasCursorPoint_ = true;
    status_.canCommit = true;
    if (numericInputTarget_ == NumericInputTarget::Length && !numericInput_.isEmpty()) {
        status_.text = QStringLiteral("Point by Line: input %1 (Enter) • %2 points")
                           .arg(numericInput_).arg(pointLineWorldPoints_.size());
    } else {
        status_.text = QStringLiteral("Point by Line: %1 points • Shift locks direction • Enter/Space/right-click finishes")
                           .arg(pointLineWorldPoints_.size());
    }
    publish(context);
}

void PointConstructionTool::togglePerpendicularPlane(const ToolInput &input,
                                                      ToolContext &context)
{
    const int activeIndex = arcStage_ >= 4 ? 1 : 0;
    if (arcCenters_.size() <= activeIndex ||
        arcFrames_.size() <= activeIndex ||
        !isValidWorkPlaneFrame(drawingFrame_)) {
        return;
    }
    if (perpendicularMode_) {
        // The add-on's P key clears its plane lock but keeps the current
        // perpendicular basis. It does not restore the original plane.
        perpendicularMode_ = false;
    } else {
        const QVector<WorkPlaneFrame> oldFrames = arcFrames_;
        const WorkPlaneFrame oldFrame = oldFrames.size() > activeIndex
                                            ? oldFrames[activeIndex]
                                            : drawingFrame_;
        const Point3D pivotWorld = workPlaneFramePointToWorld(
            arcCenters_[activeIndex], oldFrame);
        QPointF bridgePoint = input.positionInFrame(oldFrame);
        if ((arcStage_ == 1 || arcStage_ == 4) && hasCursorPoint_) {
            // updateArcPreview stores the visible, angle-snapped radius end
            // here. Use the same bridge the add-on uses (self.current), not
            // the unsnapped mouse position from this key event.
            bridgePoint = cursorPoint_;
        } else if ((arcStage_ == 2 || arcStage_ == 5) &&
            arcCenters_.size() > activeIndex && arcRadii_.size() > activeIndex &&
            arcStartAngles_.size() > activeIndex) {
            bridgePoint = pointOnArc(arcCenters_[activeIndex],
                                     arcRadii_[activeIndex],
                                     arcStartAngles_[activeIndex], 0.0, 0.0);
        }
        const Point3D bridgeWorld = subtract3(
            workPlaneFramePointToWorld(bridgePoint, oldFrame), pivotWorld);
        // Match SurfaceDrawTool's lock state: the first P press uses the
        // captured plane normal; after P unlocks, another P press falls back
        // to world Z unless L has locked the current plane again.
        const Point3D floorNormal = planeLocked_
                                        ? drawingFrame_.normal
                                        : Point3D{0.0, 0.0, 1.0};
        const Point3D newNormal = normalized3(cross3(bridgeWorld, floorNormal));
        if (length3(newNormal) <= 1.0e-8) {
            status_.text = QStringLiteral("Point by Arcs: move away from the plane axis before using P");
            publish(context);
            return;
        }
        const Point3D newXAxis = normalized3(cross3(floorNormal, newNormal));
        const WorkPlaneFrame newFrame = makeWorkPlaneFrameFromNormal(
            pivotWorld, newNormal, newXAxis);
        if (!isValidWorkPlaneFrame(newFrame)) {
            return;
        }
        const auto remapArc = [&](int index) {
            if (arcCenters_.size() <= index || arcRadii_.size() <= index ||
                arcStartAngles_.size() <= index || arcSweepAngles_.size() <= index ||
                arcRadii_[index] <= 1.0e-9) return;
            const WorkPlaneFrame sourceFrame = oldFrames.size() > index
                                                   ? oldFrames[index]
                                                   : oldFrame;
            const Point3D centerWorld = workPlaneFramePointToWorld(
                arcCenters_[index], sourceFrame);
            const QPointF startOld = pointOnArc(arcCenters_[index], arcRadii_[index],
                                                arcStartAngles_[index], 0.0, 0.0);
            const QPointF endOld = pointOnArc(arcCenters_[index], arcRadii_[index],
                                              arcStartAngles_[index],
                                              arcSweepAngles_[index], 1.0);
            const QPointF center = worldPointToWorkPlaneFrame(centerWorld, newFrame);
            const QPointF start = worldPointToWorkPlaneFrame(
                workPlaneFramePointToWorld(startOld, sourceFrame), newFrame);
            const QPointF end = worldPointToWorkPlaneFrame(
                workPlaneFramePointToWorld(endOld, sourceFrame), newFrame);
            const QPointF startVector = start - center;
            const QPointF endVector = end - center;
            arcCenters_[index] = center;
            arcRadii_[index] = std::hypot(startVector.x(), startVector.y());
            arcStartAngles_[index] = std::atan2(startVector.y(), startVector.x());
            const qreal endAngle = std::atan2(endVector.y(), endVector.x());
            if (std::abs(arcSweepAngles_[index]) >= kTwoPi - 1.0e-5) {
                arcSweepAngles_[index] = std::copysign(kTwoPi, arcSweepAngles_[index]);
            } else if (arcSweepAngles_[index] >= 0.0) {
                arcSweepAngles_[index] = normalizedAngle(endAngle - arcStartAngles_[index]);
            } else {
                arcSweepAngles_[index] = -normalizedAngle(arcStartAngles_[index] - endAngle);
            }
            while (arcFrames_.size() <= index) arcFrames_.append(newFrame);
            arcFrames_[index] = newFrame;
        };
        remapArc(0);
        remapArc(1);
        drawingFrame_ = newFrame;
        perpendicularMode_ = true;
    }
    arcOneEndpoints_.clear();
    if (arcCenters_.size() > 0 && arcRadii_.size() > 0 &&
        arcStartAngles_.size() > 0 && arcSweepAngles_.size() > 0 &&
        arcRadii_[0] > 1.0e-9) {
        const qreal endAngle = arcStartAngles_[0] + arcSweepAngles_[0];
        arcOneEndpoints_ = {
            pointOnArc(arcCenters_[0], arcRadii_[0],
                       arcStartAngles_[0], 0.0, 0.0),
            pointOnArc(arcCenters_[0], arcRadii_[0],
                       arcStartAngles_[0], endAngle - arcStartAngles_[0], 1.0)};
    }
    if (arcRadii_.size() > activeIndex && arcStartAngles_.size() > activeIndex &&
        arcRadii_[activeIndex] > 1.0e-9) {
        compassRotation_ = arcStartAngles_[activeIndex];
    }
    planeLocked_ = perpendicularMode_;
    if (planeLocked_) {
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    }
    arcIntersections_.clear();
    if (arcStage_ == 4) updateArcIntersections(true);
    if (arcStage_ == 5) updateArcIntersections(false);
    hasPreviousArcAngle_ = false;
    status_.text = perpendicularMode_
                       ? QStringLiteral("Point by Arcs: perpendicular plane on")
                       : QStringLiteral("Point by Arcs: perpendicular mode off; current plane unlocked");
    publish(context);
}

bool PointConstructionTool::applyNumericInput(ToolContext &context)
{
    if (numericInput_.isEmpty()) {
        return false;
    }
    if (tool_ == ToolId::PointByLine &&
        numericInputTarget_ == NumericInputTarget::Length) {
        qreal requestedLength = 0.0;
        if (!parseDocumentLengthInput(numericInput_,
                                      context.document().settings().lengthUnit,
                                      &requestedLength)) {
            status_.text = QStringLiteral("Point by Line: invalid length");
            publish(context);
            return false;
        }
        updatePointLineCursor(lastToolInput_, context);
        numericInput_.clear();
        numericInputTarget_ = NumericInputTarget::None;
        hasCursorPoint_ = true;
        return true;
    }
    if (tool_ == ToolId::PointByArcs &&
        numericInputTarget_ == NumericInputTarget::Radius &&
        (arcStage_ == 1 || arcStage_ == 4)) {
        qreal radius = 0.0;
        if (!parseDocumentLengthInput(numericInput_,
                                      context.document().settings().lengthUnit,
                                      &radius) || std::abs(radius) <= 1.0e-9) {
            status_.text = QStringLiteral("Point by Arcs: enter a positive radius");
            publish(context);
            return false;
        }
        const int index = arcStage_ == 1 ? 0 : 1;
        const QPointF delta = cursorPoint_ - arcCenters_[index];
        qreal angle = std::hypot(delta.x(), delta.y()) > 1.0e-10
                          ? std::atan2(delta.y(), delta.x()) : 0.0;
        if (angleSnapEnabled_ && !lastToolInput_.snapResult.isValid()) {
            angle = softSnappedAngle(angle);
        }
        while (arcRadii_.size() <= index) arcRadii_.append(0.0);
        while (arcStartAngles_.size() <= index) arcStartAngles_.append(0.0);
        while (arcSweepAngles_.size() <= index) arcSweepAngles_.append(0.0);
        arcRadii_[index] = std::abs(radius);
        arcStartAngles_[index] = angle;
        compassRotation_ = angle;
        arcSweepAngles_[index] = 0.0;
        cursorPoint_ = arcCenters_[index] +
                       QPointF(std::cos(angle), std::sin(angle)) * arcRadii_[index];
        previousArcAngle_ = angle;
        hasPreviousArcAngle_ = true;
        numericInput_.clear();
        numericInputTarget_ = NumericInputTarget::None;
        status_.text = QStringLiteral("Point by Arcs: radius set; click to begin the sweep");
        publish(context);
        return true;
    }
    if (tool_ == ToolId::PointByArcs &&
        numericInputTarget_ == NumericInputTarget::Angle &&
        (arcStage_ == 2 || arcStage_ == 5)) {
        bool valid = false;
        const qreal degrees = numericInput_.toDouble(&valid);
        if (!valid || !std::isfinite(degrees)) {
            status_.text = QStringLiteral("Point by Arcs: enter a valid sweep angle");
            publish(context);
            return false;
        }
        const int index = arcStage_ == 2 ? 0 : 1;
        arcSweepAngles_[index] = degrees * kPi / 180.0;
        const qreal endAngle = arcStartAngles_[index] + arcSweepAngles_[index];
        cursorPoint_ = arcCenters_[index] +
                       QPointF(std::cos(endAngle), std::sin(endAngle)) * arcRadii_[index];
        previousArcAngle_ = endAngle;
        hasPreviousArcAngle_ = true;
        numericInput_.clear();
        numericInputTarget_ = NumericInputTarget::None;
        if (arcStage_ == 5) updateArcIntersections(false);
        status_.text = QStringLiteral("Point by Arcs: sweep set; click to continue");
        publish(context);
        return true;
    }
    return false;
}

void PointConstructionTool::finishPointChain(ToolContext &context)
{
    QVector<Shape> pointShapes;
    pointShapes.reserve(pointLineWorldPoints_.size());
    for (const Point3D &point : pointLineWorldPoints_) {
        pointShapes.append(pointShape(QPointF(0.0, 0.0),
                                      pointFrameAt(point, drawingFrame_)));
    }
    if (!pointShapes.isEmpty() && context.commitShapes(tool_, pointShapes)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("Created %1 points").arg(pointShapes.size());
    } else {
        status_.text = QStringLiteral("Point chain discarded: no points to create");
    }
    pointLineWorldPoints_.clear();
    hasCursorPoint_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    publish(context);
    context.finishTool(ToolId::Select);
}

void PointConstructionTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
