#include "point_construction_tool.h"

#include "core/geometry/arc_curve_factory.h"
#include "core/geometry/curve_evaluator.h"
#include "core/model.h"
#include "tool_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal kTwoPi = 6.28318530717958647692;

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
    return offset <= std::abs(sweepAngle) + 1.0e-5;
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
    points_.clear();
    arcCenters_.clear();
    arcRadii_.clear();
    arcStartAngles_.clear();
    arcSweepAngles_.clear();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    frameBeforeAxisLock_ = {};
    cursorPoint_ = {};
    hasCursorPoint_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    arcStage_ = 0;
    previousArcAngle_ = 0.0;
    hasPreviousArcAngle_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.canCommit = false;
    if (tool_ == ToolId::PointByLine) {
        status_.text = QStringLiteral("Point by Line: click points, right-click to finish");
    } else if (tool_ == ToolId::PointByArcs) {
        status_.text = QStringLiteral("Point by Arcs: pick the first arc center");
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
        if (points_.isEmpty()) {
            if (!planeLocked_) {
                drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                                    ? input.workPlaneFrame
                                    : context.viewportTransform().workPlaneFrame();
            }
            if (!isValidWorkPlaneFrame(drawingFrame_)) {
                return true;
            }
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            planeLocked_ = true;
        }
        points_.append(input.worldPosition);
        cursorPoint_ = input.worldPosition;
        hasCursorPoint_ = true;
        status_.canCommit = !points_.isEmpty();
        status_.text = QStringLiteral("Point by Line: %1 points • right-click to finish")
                           .arg(points_.size());
        publish(context);
        return true;
    }

    if (tool_ == ToolId::PointByArcs && input.button == Qt::RightButton) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }

    if (input.button != Qt::LeftButton) {
        return false;
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
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
                planeLocked_ = true;
            }
            arcCenters_.append(input.worldPosition);
            arcStage_ = arcStage_ == 0 ? 1 : 4;
            status_.text = arcStage_ == 1
                               ? QStringLiteral("Point by Arcs: pick the first arc radius")
                               : QStringLiteral("Point by Arcs: pick the second arc radius");
            publish(context);
            return true;
        }
        if (arcStage_ == 1 || arcStage_ == 4) {
            const int arcIndex = arcStage_ == 1 ? 0 : 1;
            const QPointF delta = input.worldPosition - arcCenters_[arcIndex];
            const qreal radius = std::hypot(delta.x(), delta.y());
            if (radius <= 1.0e-9) {
                status_.text = QStringLiteral("Pick a radius point away from the arc center");
                publish(context);
                return true;
            }
            const qreal angle = std::atan2(delta.y(), delta.x());
            if (arcRadii_.size() <= arcIndex) {
                arcRadii_.append(radius);
                arcStartAngles_.append(angle);
                arcSweepAngles_.append(0.0);
            } else {
                arcRadii_[arcIndex] = radius;
                arcStartAngles_[arcIndex] = angle;
                arcSweepAngles_[arcIndex] = 0.0;
            }
            previousArcAngle_ = angle;
            hasPreviousArcAngle_ = true;
            arcStage_ = arcStage_ == 1 ? 2 : 5;
            status_.text = arcStage_ == 2
                               ? QStringLiteral("Point by Arcs: sweep the first arc, then click")
                               : QStringLiteral("Point by Arcs: sweep the second arc, then click");
            publish(context);
            return true;
        }
        if (arcStage_ == 2 || arcStage_ == 5) {
            handleMouseMove(input, context);
            if (arcStage_ == 2) {
                arcStage_ = 3;
                status_.text = QStringLiteral("Point by Arcs: pick the second arc center");
                publish(context);
                return true;
            }
            QVector<Shape> intersections;
            const QPointF firstCenter = arcCenters_[0];
            const QPointF secondCenter = arcCenters_[1];
            const qreal firstRadius = arcRadii_[0];
            const qreal secondRadius = arcRadii_[1];
            const QPointF offset = secondCenter - firstCenter;
            const qreal distance = std::hypot(offset.x(), offset.y());
            if (distance > 1.0e-12 &&
                distance <= firstRadius + secondRadius + 1.0e-9 &&
                distance >= std::abs(firstRadius - secondRadius) - 1.0e-9) {
                const qreal along = (firstRadius * firstRadius -
                                     secondRadius * secondRadius +
                                     distance * distance) / (2.0 * distance);
                const qreal height = std::sqrt(std::max<qreal>(
                    0.0, firstRadius * firstRadius - along * along));
                const QPointF direction = offset / distance;
                const QPointF base = firstCenter + direction * along;
                const QPointF normal(-direction.y(), direction.x());
                const QPointF candidates[] = {base + normal * height,
                                              base - normal * height};
                QVector<QPointF> unique;
                for (int index = 0; index < (height <= 1.0e-9 ? 1 : 2); ++index) {
                    const QPointF candidate = candidates[index];
                    const qreal firstAngle = std::atan2(candidate.y() - firstCenter.y(),
                                                        candidate.x() - firstCenter.x());
                    const qreal secondAngle = std::atan2(candidate.y() - secondCenter.y(),
                                                         candidate.x() - secondCenter.x());
                    if (angleOnArc(firstAngle, arcStartAngles_[0], arcSweepAngles_[0]) &&
                        angleOnArc(secondAngle, arcStartAngles_[1], arcSweepAngles_[1])) {
                        appendUnique(&unique, candidate);
                    }
                }
                for (const QPointF &intersection : unique) {
                    intersections.append(pointShape(intersection, drawingFrame_));
                }
            }
            if (intersections.isEmpty()) {
                status_.text = QStringLiteral("The arcs do not intersect in their drawn spans");
                publish(context);
                return true;
            }
            if (context.commitShapes(tool_, intersections)) {
                status_.state = ToolLifecycleState::Completed;
                status_.text = QStringLiteral("Created %1 arc intersection point(s)")
                                   .arg(intersections.size());
                publish(context);
                context.finishTool(ToolId::Select);
            }
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
    if (tool_ == ToolId::PointByArcs && (arcStage_ == 2 || arcStage_ == 5) &&
        hasPreviousArcAngle_) {
        const int arcIndex = arcStage_ == 2 ? 0 : 1;
        const QPointF delta = input.worldPosition - arcCenters_[arcIndex];
        if (std::hypot(delta.x(), delta.y()) > 1.0e-10) {
            const qreal angle = std::atan2(delta.y(), delta.x());
            qreal change = angle - previousArcAngle_;
            while (change > 3.14159265358979323846) change -= kTwoPi;
            while (change < -3.14159265358979323846) change += kTwoPi;
            arcSweepAngles_[arcIndex] += change;
            previousArcAngle_ = angle;
            cursorPoint_ = input.worldPosition;
            hasCursorPoint_ = true;
            publish(context);
        }
        return true;
    }
    if (tool_ == ToolId::PointByLine && !points_.isEmpty()) {
        cursorPoint_ = input.worldPosition;
        hasCursorPoint_ = true;
        publish(context);
    } else if (tool_ == ToolId::PointByLine && !planeLocked_ &&
               isValidWorkPlaneFrame(input.workPlaneFrame)) {
        drawingFrame_ = input.workPlaneFrame;
    }
    return false;
}

bool PointConstructionTool::handleKey(const ToolInput &input,
                                       ToolContext &context)
{
    const bool planeSelectable =
        (tool_ == ToolId::PointByLine && points_.isEmpty()) ||
        (tool_ == ToolId::PointByArcs && arcStage_ == 0);
    if (planeSelectable && input.key == Qt::Key_L) {
        if (planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeAxisLock_)
                                ? frameBeforeAxisLock_
                                : input.workPlaneFrame;
            normalAxisLockKey_ = 0;
            planeLocked_ = false;
        } else {
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
    if (planeSelectable &&
        (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
         input.key == Qt::Key_Z)) {
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
            const Point3D origin = workPlaneFramePointToWorld(input.worldPosition,
                                                               input.workPlaneFrame);
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
    if (input.key == Qt::Key_Escape) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }
    if (tool_ == ToolId::PointByLine &&
        (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) &&
        !points_.isEmpty()) {
        points_.removeLast();
        cursorPoint_ = points_.isEmpty() ? QPointF() : points_.last();
        hasCursorPoint_ = !points_.isEmpty();
        status_.canCommit = !points_.isEmpty();
        status_.text = QStringLiteral("Point by Line: %1 points • right-click to finish")
                           .arg(points_.size());
        publish(context);
        return true;
    }
    return false;
}

void PointConstructionTool::cancel(ToolContext &context)
{
    points_.clear();
    arcCenters_.clear();
    arcRadii_.clear();
    arcStartAngles_.clear();
    arcSweepAngles_.clear();
    arcStage_ = 0;
    hasPreviousArcAngle_ = false;
    hasCursorPoint_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview PointConstructionTool::preview() const
{
    ToolPreview result;
    result.points = points_;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = planeLocked_ || !points_.isEmpty() || arcStage_ > 0;
    if (!result.points.isEmpty()) {
        result.shape.geometryType = GeometryType::Point;
        result.shape.points = result.points;
        if (tool_ == ToolId::PointByLine && hasCursorPoint_ &&
            (result.shape.points.isEmpty() ||
             result.shape.points.last() != cursorPoint_)) {
            result.shape.points.append(cursorPoint_);
        }
        result.shape.workPlaneFrame = drawingFrame_;
        result.shape.workPlane = WorkPlane::XY;
        result.hasShape = true;
    }
    if (tool_ == ToolId::PointByArcs && !arcCenters_.isEmpty() &&
        !arcRadii_.isEmpty()) {
        const int arcIndex = arcStage_ == 5 ? 1 : 0;
        if (arcRadii_.size() > arcIndex &&
            arcStartAngles_.size() > arcIndex &&
            arcSweepAngles_.size() > arcIndex) {
            result.shape = arcPreviewShape(arcCenters_[arcIndex],
                                           arcRadii_[arcIndex],
                                           arcStartAngles_[arcIndex],
                                           arcSweepAngles_[arcIndex],
                                           drawingFrame_);
            result.hasShape = validateNurbsCurve(result.shape.nurbs);
        }
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

void PointConstructionTool::finishPointChain(ToolContext &context)
{
    QVector<Shape> pointShapes;
    pointShapes.reserve(points_.size());
    for (const QPointF &point : points_) {
        pointShapes.append(pointShape(point, drawingFrame_));
    }
    if (!pointShapes.isEmpty() && context.commitShapes(tool_, pointShapes)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("Created %1 points").arg(pointShapes.size());
    } else {
        status_.text = QStringLiteral("Point chain discarded: no points to create");
    }
    points_.clear();
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
