#include "line_tool.h"
#include "tool_context.h"
#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {
Point3D subtract(const Point3D &a, const Point3D &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
qreal dot(const Point3D &a, const Point3D &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
qreal length(const Point3D &v) { return std::sqrt(dot(v, v)); }
Point3D normalized(const Point3D &v)
{
    const qreal magnitude = length(v);
    return magnitude > 1.0e-12
        ? Point3D{v.x / magnitude, v.y / magnitude, v.z / magnitude} : Point3D{};
}
Point3D axisDirection(int key)
{
    switch (key) {
    case Qt::Key_X: return {1.0, 0.0, 0.0};
    case Qt::Key_Y: return {0.0, 1.0, 0.0};
    case Qt::Key_Z: return {0.0, 0.0, 1.0};
    default: return {};
    }
}
// Preserve the longest planar runs. Changing planes creates component curves
// rather than storing nonplanar points in a NurbsCurve2D.
QVector<Shape> planarRuns(const QVector<Point3D> &points,
                         const WorkPlaneFrame &preferredFrame)
{
    QVector<Shape> result;
    for (int first = 0; first + 1 < points.size();) {
        WorkPlaneFrame inherited = preferredFrame;
        const qreal planeOffset = signedDistanceFromWorkPlaneFrame(points[first], inherited);
        inherited.origin.x += inherited.normal.x * planeOffset;
        inherited.origin.y += inherited.normal.y * planeOffset;
        inherited.origin.z += inherited.normal.z * planeOffset;
        const Point3D direction = normalized(subtract(points[first + 1], points[first]));
        const qreal normalAlongDirection = dot(preferredFrame.normal, direction);
        Point3D segmentNormal{
            preferredFrame.normal.x - direction.x * normalAlongDirection,
            preferredFrame.normal.y - direction.y * normalAlongDirection,
            preferredFrame.normal.z - direction.z * normalAlongDirection};
        if (length(segmentNormal) <= 1.0e-8) {
            const Point3D helper = std::abs(direction.z) < 0.9
                ? Point3D{0.0, 0.0, 1.0} : Point3D{0.0, 1.0, 0.0};
            const qreal along = dot(helper, direction);
            segmentNormal = {helper.x - direction.x * along,
                             helper.y - direction.y * along,
                             helper.z - direction.z * along};
        }
        const WorkPlaneFrame candidates[] = {
            inherited,
            makeWorkPlaneFrame(WorkPlane::XY, points[first].z),
            makeWorkPlaneFrame(WorkPlane::XZ, points[first].y),
            makeWorkPlaneFrame(WorkPlane::YZ, points[first].x),
            makeWorkPlaneFrameFromNormal(points[first], segmentNormal, direction)};
        int last = first;
        WorkPlaneFrame frame;
        for (const WorkPlaneFrame &candidate : candidates) {
            if (!isValidWorkPlaneFrame(candidate)) continue;
            int end = first;
            while (end + 1 < points.size() &&
                   std::abs(signedDistanceFromWorkPlaneFrame(points[end + 1], candidate)) <= 1.0e-8) {
                ++end;
            }
            if (end > last) {
                last = end;
                frame = candidate;
            }
        }
        if (last == first) return {};
        Shape shape;
        shape.geometryType = GeometryType::Line;
        shape.workPlaneFrame = frame;
        for (const WorkPlane plane : {WorkPlane::XY, WorkPlane::XZ, WorkPlane::YZ}) {
            if (std::abs(dot(frame.normal, workPlaneNormal(plane))) > 1.0 - 1.0e-8) {
                shape.workPlane = plane;
                shape.workPlaneOffset = plane == WorkPlane::XY ? frame.origin.z
                    : plane == WorkPlane::XZ ? frame.origin.y : frame.origin.x;
                break;
            }
        }
        for (int index = first; index <= last; ++index) {
            shape.points.append(worldPointToWorkPlaneFrame(points[index], frame));
        }
        shape.nurbs = makeDegreeOneNurbs(shape.points);
        if (!validateNurbsCurve(shape.nurbs)) return {};
        result.append(shape);
        first = last;
    }
    return result;
}
} // namespace

ToolId LineTool::id() const { return ToolId::Line; }
void LineTool::reset()
{
    points_.clear();
    drawingFrame_ = {};
    cursorPoint_ = {};
    hasCursorPoint_ = false;
    constraintAxisKey_ = 0;
    normalLock_ = false;
    planeLocked_ = false;
    shiftLockDirection_ = {};
    shiftLockActive_ = false;
    lastInput_ = {};
    snap_ = {};
    status_.canCommit = false;
}
void LineTool::begin(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Line: pick first point");
    publish(context);
}
bool LineTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        commit(context);
        return true;
    }
    if (input.button != Qt::LeftButton) return false;
    lastInput_ = input;
    if (points_.isEmpty()) {
        if (!planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                ? input.workPlaneFrame : context.viewportTransform().workPlaneFrame();
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) return false;
        snap_ = context.snapEngine().findSpatialSnapPoint(context.document(),
            input.screenPosition, nullptr, context.viewportTransform(), input.viewportSize);
        cursorPoint_ = snap_.hasWorldPoint ? snap_.worldPoint
            : workPlaneFramePointToWorld(input.worldPosition, drawingFrame_);
        planeLocked_ = true;
    } else {
        cursorPoint_ = resolveCursorPoint(input, context);
        if (length(subtract(cursorPoint_, points_.back())) <= 1.0e-8) return true;
    }
    points_.append(cursorPoint_);
    // The addon locks the normal, while moving the plane through each new pivot.
    const qreal offset = signedDistanceFromWorkPlaneFrame(cursorPoint_, drawingFrame_);
    drawingFrame_.origin.x += drawingFrame_.normal.x * offset;
    drawingFrame_.origin.y += drawingFrame_.normal.y * offset;
    drawingFrame_.origin.z += drawingFrame_.normal.z * offset;
    context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    if (snap_.hasWorldPoint) {
        QPointF snapScreen;
        if (context.viewportTransform().worldPointToScreen(snap_.worldPoint,
                input.viewportSize, &snapScreen)) {
            snap_.point = context.viewportTransform().screenToWorld(snapScreen, input.viewportSize);
        }
    }
    hasCursorPoint_ = true;
    if (points_.size() > 1) {
        constraintAxisKey_ = 0;
        normalLock_ = false;
    }
    shiftLockActive_ = false;
    status_.canCommit = points_.size() >= 2;
    status_.text = QStringLiteral("Line: %1 points  •  Right-click to finish").arg(points_.size());
    publish(context);
    return true;
}
bool LineTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    lastInput_ = input;
    if (!planeLocked_) {
        drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
            ? input.workPlaneFrame : context.viewportTransform().workPlaneFrame();
    }
    if (points_.isEmpty()) {
        cursorPoint_ = workPlaneFramePointToWorld(input.worldPosition, drawingFrame_);
        return false;
    }
    cursorPoint_ = resolveCursorPoint(input, context);
    hasCursorPoint_ = true;
    publish(context);
    return true;
}
bool LineTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key == Qt::Key_L) {
        planeLocked_ = !planeLocked_;
        if (planeLocked_) {
            if (!isValidWorkPlaneFrame(drawingFrame_)) {
                drawingFrame_ = context.viewportTransform().workPlaneFrame();
            }
            const qreal offset = signedDistanceFromWorkPlaneFrame(cursorPoint_, drawingFrame_);
            drawingFrame_.origin.x += drawingFrame_.normal.x * offset;
            drawingFrame_.origin.y += drawingFrame_.normal.y * offset;
            drawingFrame_.origin.z += drawingFrame_.normal.z * offset;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        status_.text = planeLocked_ ? QStringLiteral("Line: plane locked")
                                  : QStringLiteral("Line: plane unlocked");
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) {
        if (points_.size() <= 1) return false;
        points_.removeLast();
        const qreal offset = signedDistanceFromWorkPlaneFrame(points_.back(), drawingFrame_);
        drawingFrame_.origin.x += drawingFrame_.normal.x * offset;
        drawingFrame_.origin.y += drawingFrame_.normal.y * offset;
        drawingFrame_.origin.z += drawingFrame_.normal.z * offset;
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        constraintAxisKey_ = 0;
        normalLock_ = false;
        shiftLockActive_ = false;
        cursorPoint_ = resolveCursorPoint(lastInput_, context);
        status_.canCommit = points_.size() >= 2;
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
        input.key == Qt::Key_Z || input.key == Qt::Key_N) {
        if (input.key == Qt::Key_N) {
            if (points_.isEmpty()) return true;
            normalLock_ = !normalLock_;
            constraintAxisKey_ = 0;
        } else {
            normalLock_ = false;
            constraintAxisKey_ = constraintAxisKey_ == input.key ? 0 : input.key;
        }
        shiftLockActive_ = false;
        if (!points_.isEmpty()) cursorPoint_ = resolveCursorPoint(lastInput_, context);
        status_.text = normalLock_ ? QStringLiteral("Line: normal direction locked")
            : constraintAxisKey_ != 0
                ? QStringLiteral("Line: constrained to world %1").arg(QChar(constraintAxisKey_))
                : QStringLiteral("Line: axis constraint cleared");
        publish(context);
        return true;
    }
    if (input.key != Qt::Key_Escape) return false;
    cancel(context);
    context.finishTool(ToolId::Select);
    return true;
}
void LineTool::cancel(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Line cancelled");
    publish(context);
}
void LineTool::commit(ToolContext &context)
{
    const QVector<Shape> shapes = planarRuns(points_, drawingFrame_);
    if (!shapes.isEmpty() && context.commitShapes(id(), shapes)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("Line committed");
    } else {
        status_.text = QStringLiteral("Line discarded: at least two distinct points required");
    }
    reset();
    publish(context);
    context.finishTool(ToolId::Select);
}
ToolPreview LineTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = drawingFrame_;
    result.planeLocked = planeLocked_;
    result.snap = snap_;
    result.overridesSnap = true;
    result.worldPoints = points_;
    for (const Point3D &point : points_) {
        result.points.append(worldPointToWorkPlaneFrame(point, drawingFrame_));
    }
    result.worldCursorPoint = cursorPoint_;
    result.cursorPoint = worldPointToWorkPlaneFrame(cursorPoint_, drawingFrame_);
    result.hasCursorPoint = hasCursorPoint_ && !points_.isEmpty();
    QVector<Point3D> previewPoints = points_;
    if (result.hasCursorPoint && length(subtract(cursorPoint_, points_.back())) > 1.0e-8) {
        previewPoints.append(cursorPoint_);
    }
    result.shapes = planarRuns(previewPoints, drawingFrame_);
    result.statusText = status_.text;
    return result;
}
ToolStatus LineTool::status() const { return status_; }
Point3D LineTool::inferredAxisDirection(const ToolInput &input,
                                       const ToolContext &context) const
{
    QPointF referenceScreen;
    const Point3D &reference = points_.back();
    if (!context.viewportTransform().worldPointToScreen(reference, input.viewportSize,
                                                        &referenceScreen)) return {};
    const QPointF delta = input.screenPosition - referenceScreen;
    const qreal screenLength = std::hypot(delta.x(), delta.y());
    if (screenLength <= 1.0) return {};
    qreal best = std::cos(6.0 * 3.14159265358979323846 / 180.0);
    Point3D direction;
    for (const int key : {Qt::Key_X, Qt::Key_Y, Qt::Key_Z}) {
        const Point3D axis = axisDirection(key);
        QPointF projected;
        if (!context.viewportTransform().worldPointToScreen(
                {reference.x + axis.x, reference.y + axis.y, reference.z + axis.z},
                input.viewportSize, &projected)) continue;
        const QPointF screenAxis = projected - referenceScreen;
        const qreal magnitude = std::hypot(screenAxis.x(), screenAxis.y());
        if (magnitude <= 1.0e-8) continue;
        const qreal alignment = std::abs(QPointF::dotProduct(delta, screenAxis) /
                                         (screenLength * magnitude));
        if (alignment >= best) {
            best = alignment;
            direction = axis;
        }
    }
    return direction;
}
Point3D LineTool::resolveCursorPoint(const ToolInput &input, const ToolContext &context)
{
    const Point3D reference = points_.back();
    snap_ = context.snapEngine().findSpatialSnapPoint(context.document(),
        input.screenPosition, &reference, context.viewportTransform(), input.viewportSize, points_);
    const bool geometrySnap = snap_.hasWorldPoint || input.snapType != SnapType::None;
    const WorkPlaneFrame inputFrame = isValidWorkPlaneFrame(input.workPlaneFrame)
        ? input.workPlaneFrame : drawingFrame_;
    Point3D source = workPlaneFramePointToWorld(input.worldPosition, inputFrame);
    if (snap_.hasWorldPoint) source = snap_.worldPoint;
    if (planeLocked_ && !geometrySnap && !input.orthoEnabled &&
        !input.viewportSize.isEmpty()) {
        QPointF point;
        source = context.viewportTransform().screenToWorkPlane(
            input.screenPosition, input.viewportSize, drawingFrame_, &point)
            ? workPlaneFramePointToWorld(point, drawingFrame_) : reference;
    }
    Point3D direction = normalLock_ ? drawingFrame_.normal : axisDirection(constraintAxisKey_);
    if (input.modifiers.testFlag(Qt::ShiftModifier)) {
        if (!shiftLockActive_) {
            if (length(direction) <= 1.0e-12) direction = inferredAxisDirection(input, context);
            if (length(direction) <= 1.0e-12) direction = normalized(subtract(source, reference));
            shiftLockDirection_ = direction;
            shiftLockActive_ = length(direction) > 1.0e-12;
        }
        direction = shiftLockDirection_;
    } else {
        shiftLockActive_ = false;
        if (length(direction) <= 1.0e-12 && !geometrySnap) {
            direction = inferredAxisDirection(input, context);
        }
    }
    if (length(direction) <= 1.0e-12) return source;
    direction = normalized(direction);
    Point3D axisPoint;
    if (!geometrySnap &&
        context.viewportTransform().screenToWorldAxis(input.screenPosition, input.viewportSize,
                                                       reference, direction, &axisPoint)) {
        return axisPoint;
    }
    const qreal distance = dot(subtract(source, reference), direction);
    return {reference.x + direction.x * distance,
            reference.y + direction.y * distance,
            reference.z + direction.z * distance};
}
void LineTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}
} // namespace classiCAD
