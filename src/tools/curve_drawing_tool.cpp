#include "curve_drawing_tool.h"

#include "core/geometry/interpolating_curve_factory.h"
#include "tool_context.h"

#include <cmath>

namespace classiCAD {
namespace {

void setLegacyWorkPlaneMetadata(Shape *shape, const WorkPlaneFrame &frame)
{
    if (shape == nullptr) {
        return;
    }
    shape->workPlaneFrame = frame;
    shape->workPlane = WorkPlane::XY;
    shape->workPlaneOffset = frame.origin.z;
    if (std::abs(frame.normal.z) >= 1.0 - 1.0e-8) {
        shape->workPlane = WorkPlane::XY;
        shape->workPlaneOffset = frame.origin.z;
    } else if (std::abs(frame.normal.y) >= 1.0 - 1.0e-8) {
        shape->workPlane = WorkPlane::XZ;
        shape->workPlaneOffset = frame.origin.y;
    } else if (std::abs(frame.normal.x) >= 1.0 - 1.0e-8) {
        shape->workPlane = WorkPlane::YZ;
        shape->workPlaneOffset = frame.origin.x;
    }
}

} // namespace

CurveDrawingTool::CurveDrawingTool(ToolId tool)
    : tool_(tool)
{
}

ToolId CurveDrawingTool::id() const
{
    return tool_;
}

void CurveDrawingTool::begin(ToolContext &context)
{
    points_.clear();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    frameBeforeAxisLock_ = {};
    cursorPoint_ = {};
    lastSampleScreen_ = {};
    hasCursorPoint_ = false;
    hasSampleScreen_ = false;
    drawing_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    status_.state = ToolLifecycleState::Active;
    status_.canCommit = false;
    status_.text = isFreehand()
                       ? QStringLiteral("Freehand Curve: click to start, click again to finish")
                       : QStringLiteral("Interpolate Curve: click points, Enter or right-click to finish");
    publish(context);
}

bool CurveDrawingTool::handleMousePress(const ToolInput &input,
                                         ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        finish(context);
        return true;
    }
    if (input.button != Qt::LeftButton) {
        return false;
    }

    if (!drawing_) {
        if (!planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                                ? input.workPlaneFrame
                                : context.viewportTransform().workPlaneFrame();
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
            return true;
        }
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        drawing_ = true;
        planeLocked_ = true;
        appendPoint(input.worldPosition);
        cursorPoint_ = input.worldPosition;
        lastSampleScreen_ = input.screenPosition;
        hasCursorPoint_ = true;
        hasSampleScreen_ = true;
        status_.text = isFreehand()
                           ? QStringLiteral("Freehand Curve: draw, then click to finish")
                           : QStringLiteral("Interpolate Curve: %1 points • Enter to finish")
                                 .arg(points_.size());
        publish(context);
        return true;
    }

    if (isFreehand()) {
        appendPoint(input.worldPosition);
        cursorPoint_ = input.worldPosition;
        finish(context);
        return true;
    }

    if (!appendPoint(input.worldPosition)) {
        status_.text = QStringLiteral("Pick a point farther from the previous point");
    } else {
        cursorPoint_ = input.worldPosition;
        status_.text = QStringLiteral("Interpolate Curve: %1 points • Enter to finish")
                           .arg(points_.size());
    }
    hasCursorPoint_ = true;
    publish(context);
    return true;
}

bool CurveDrawingTool::handleMouseMove(const ToolInput &input,
                                       ToolContext &context)
{
    if (!drawing_) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        return false;
    }
    cursorPoint_ = input.worldPosition;
    hasCursorPoint_ = true;
    if (isFreehand()) {
        const qreal screenDistance = std::hypot(
            input.screenPosition.x() - lastSampleScreen_.x(),
            input.screenPosition.y() - lastSampleScreen_.y());
        if (!hasSampleScreen_ || screenDistance >= 2.0) {
            appendPoint(input.worldPosition);
            lastSampleScreen_ = input.screenPosition;
            hasSampleScreen_ = true;
        }
    }
    publish(context);
    return true;
}

bool CurveDrawingTool::handleKey(const ToolInput &input,
                                  ToolContext &context)
{
    if (!drawing_ && input.key == Qt::Key_L) {
        if (planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeAxisLock_)
                                ? frameBeforeAxisLock_
                                : context.viewportTransform().workPlaneFrame();
            normalAxisLockKey_ = 0;
            planeLocked_ = false;
            status_.text = QStringLiteral("%1: drawing plane unlocked")
                               .arg(toolName(tool_));
        } else {
            drawingFrame_ = input.workPlaneFrame;
            planeLocked_ = isValidWorkPlaneFrame(drawingFrame_);
            status_.text = QStringLiteral("%1: drawing plane locked")
                               .arg(toolName(tool_));
        }
        if (isValidWorkPlaneFrame(drawingFrame_)) {
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        publish(context);
        return true;
    }
    if (!drawing_ && (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
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
    if (input.key == Qt::Key_Return || input.key == Qt::Key_Enter) {
        finish(context);
        return true;
    }
    if (!isFreehand() &&
        (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) &&
        !points_.isEmpty()) {
        points_.removeLast();
        cursorPoint_ = points_.isEmpty() ? QPointF() : points_.last();
        hasCursorPoint_ = !points_.isEmpty();
        status_.canCommit = points_.size() >= 2;
        status_.text = QStringLiteral("Interpolate Curve: %1 points • Enter to finish")
                           .arg(points_.size());
        publish(context);
        return true;
    }
    return false;
}

void CurveDrawingTool::cancel(ToolContext &context)
{
    points_.clear();
    drawing_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    hasCursorPoint_ = false;
    hasSampleScreen_ = false;
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview CurveDrawingTool::preview() const
{
    ToolPreview result;
    result.points = points_;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = drawing_ || planeLocked_;
    result.hasShape = drawing_;
    result.shape = makePreviewShape(true);
    result.statusText = status_.text;
    return result;
}

ToolStatus CurveDrawingTool::status() const
{
    return status_;
}

bool CurveDrawingTool::isFreehand() const
{
    return tool_ == ToolId::CurveFreehand;
}

bool CurveDrawingTool::appendPoint(const QPointF &point)
{
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
        return false;
    }
    if (!points_.isEmpty() &&
        std::hypot(points_.last().x() - point.x(),
                   points_.last().y() - point.y()) <= 1.0e-8) {
        return false;
    }
    points_.append(point);
    return true;
}

Shape CurveDrawingTool::makePreviewShape(bool includeCursor) const
{
    Shape shape;
    shape.geometryType = GeometryType::Nurbs;
    shape.points = points_;
    if (includeCursor && !shape.points.isEmpty() && hasCursorPoint_ &&
        std::hypot(shape.points.last().x() - cursorPoint_.x(),
                   shape.points.last().y() - cursorPoint_.y()) > 1.0e-8) {
        shape.points.append(cursorPoint_);
    }
    setLegacyWorkPlaneMetadata(&shape, drawingFrame_);
    if (isValidWorkPlaneFrame(drawingFrame_) && shape.points.size() >= 2) {
        makeCentripetalCatmullRomNurbsCurve(shape.points, &shape.nurbs);
    }
    return shape;
}

void CurveDrawingTool::finish(ToolContext &context)
{
    Shape shape = makePreviewShape(false);
    if (points_.size() >= 2 && validateNurbsCurve(shape.nurbs) &&
        context.commitShape(tool_, shape)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("%1 committed").arg(toolName(tool_));
    } else {
        status_.text = QStringLiteral("%1 needs at least two distinct points")
                           .arg(toolName(tool_));
    }
    points_.clear();
    drawing_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    hasCursorPoint_ = false;
    hasSampleScreen_ = false;
    status_.canCommit = false;
    publish(context);
    context.finishTool(ToolId::Select);
}

void CurveDrawingTool::publish(ToolContext &context)
{
    status_.canCommit = points_.size() >= 2;
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
