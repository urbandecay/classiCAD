#include "shape_creation_tool.h"

#include "tool_context.h"

#include <cmath>

namespace classiCAD {
namespace {

Point3D normalForAxisKey(int key)
{
    switch (key) {
    case Qt::Key_X:
        return {1.0, 0.0, 0.0};
    case Qt::Key_Y:
        return {0.0, 1.0, 0.0};
    case Qt::Key_Z:
        return {0.0, 0.0, 1.0};
    default:
        return {};
    }
}

void setLegacyPlaneMetadata(Shape *shape, const WorkPlaneFrame &frame)
{
    if (shape == nullptr) {
        return;
    }
    shape->workPlaneFrame = frame;
    shape->workPlane = WorkPlane::XY;
    shape->workPlaneOffset = 0.0;
    constexpr qreal tolerance = 1.0e-8;
    if (std::abs(frame.normal.z) >= 1.0 - tolerance) {
        shape->workPlane = WorkPlane::XY;
        shape->workPlaneOffset = frame.origin.z;
    } else if (std::abs(frame.normal.y) >= 1.0 - tolerance) {
        shape->workPlane = WorkPlane::XZ;
        shape->workPlaneOffset = frame.origin.y;
    } else if (std::abs(frame.normal.x) >= 1.0 - tolerance) {
        shape->workPlane = WorkPlane::YZ;
        shape->workPlaneOffset = frame.origin.x;
    }
}

} // namespace

ShapeCreationTool::ShapeCreationTool(ToolId tool, int requiredPoints)
    : tool_(tool)
    , requiredPoints_(requiredPoints)
{
}

ToolId ShapeCreationTool::id() const
{
    return tool_;
}

void ShapeCreationTool::begin(ToolContext &context)
{
    points_.clear();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    frameBeforeAxisLock_ = {};
    normalAxisLockKey_ = 0;
    planeLocked_ = false;
    frameCaptured_ = false;
    manualPlaneLock_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("%1 input started").arg(toolName(tool_));
    status_.canCommit = false;
    publish(context);
}

bool ShapeCreationTool::handleMousePress(const ToolInput &input,
                                         ToolContext &context)
{
    if (input.button != Qt::LeftButton) {
        return false;
    }

    if (!frameCaptured_) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
            drawingFrame_ = context.viewportTransform().workPlaneFrame();
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
            return true;
        }
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        frameCaptured_ = true;
        planeLocked_ = true;
    }

    points_.append(input.positionInFrame(drawingFrame_));
    publish(context);
    if (points_.size() != requiredPoints_) {
        return true;
    }

    Shape shape;
    if (!buildShape(context, &shape)) {
        status_.state = ToolLifecycleState::Cancelled;
        status_.text = QStringLiteral("%1 input rejected").arg(toolName(tool_));
        status_.canCommit = false;
        publish(context);
        return true;
    }

    setLegacyPlaneMetadata(&shape, drawingFrame_);
    if (context.commitShape(tool_, shape)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("%1 committed").arg(toolName(tool_));
        status_.canCommit = false;
        points_.clear();
        publish(context);
        context.finishTool(ToolId::Select);
    }
    return true;
}

bool ShapeCreationTool::handleMouseMove(const ToolInput &input,
                                        ToolContext &context)
{
    if (!frameCaptured_ && !planeLocked_ &&
        isValidWorkPlaneFrame(input.workPlaneFrame)) {
        drawingFrame_ = input.workPlaneFrame;
    }
    if ((frameCaptured_ || planeLocked_) &&
        isValidWorkPlaneFrame(drawingFrame_)) {
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    }
    publish(context);
    return false;
}

bool ShapeCreationTool::handleKey(const ToolInput &input,
                                  ToolContext &context)
{
    if (input.key == Qt::Key_L && !frameCaptured_) {
        if (planeLocked_) {
            if (normalAxisLockKey_ != 0 &&
                isValidWorkPlaneFrame(frameBeforeAxisLock_)) {
                drawingFrame_ = frameBeforeAxisLock_;
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
            normalAxisLockKey_ = 0;
            manualPlaneLock_ = false;
            planeLocked_ = false;
            status_.text = QStringLiteral("%1: drawing plane unlocked")
                               .arg(toolName(tool_));
        } else if (isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
            planeLocked_ = true;
            manualPlaneLock_ = true;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            status_.text = QStringLiteral("%1: drawing plane locked")
                               .arg(toolName(tool_));
        }
        publish(context);
        return true;
    }

    if (!frameCaptured_ &&
        (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
         input.key == Qt::Key_Z)) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        if (normalAxisLockKey_ == input.key) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeAxisLock_)
                                ? frameBeforeAxisLock_
                                : input.workPlaneFrame;
            normalAxisLockKey_ = 0;
            planeLocked_ = manualPlaneLock_;
            if (isValidWorkPlaneFrame(drawingFrame_)) {
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
            status_.text = QStringLiteral("%1: axis plane cleared")
                               .arg(toolName(tool_));
            publish(context);
            return true;
        }

        if (normalAxisLockKey_ == 0) {
            frameBeforeAxisLock_ = drawingFrame_;
        }
        const Point3D normal = normalForAxisKey(input.key);
        Point3D planeOrigin = workPlaneFramePointToWorld(
            input.worldPosition, input.workPlaneFrame);
        if (!std::isfinite(planeOrigin.x) || !std::isfinite(planeOrigin.y) ||
            !std::isfinite(planeOrigin.z)) {
            planeOrigin = drawingFrame_.origin;
        }
        const WorkPlaneFrame axisFrame = makeWorkPlaneFrameFromNormal(
            planeOrigin, normal, drawingFrame_.xAxis);
        if (isValidWorkPlaneFrame(axisFrame)) {
            drawingFrame_ = axisFrame;
            normalAxisLockKey_ = input.key;
            planeLocked_ = true;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            status_.text = QStringLiteral("%1: plane normal locked to world %2")
                               .arg(toolName(tool_), QChar(input.key));
        }
        publish(context);
        return true;
    }

    if (frameCaptured_ &&
        (input.key == Qt::Key_L || input.key == Qt::Key_X ||
         input.key == Qt::Key_Y || input.key == Qt::Key_Z)) {
        status_.text = QStringLiteral("%1: plane is fixed for this shape")
                           .arg(toolName(tool_));
        publish(context);
        return true;
    }

    if (input.key != Qt::Key_Escape) {
        return false;
    }

    clearPoints(context);
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("%1 input cleared").arg(toolName(tool_));
    status_.canCommit = false;
    publish(context);
    return true;
}

void ShapeCreationTool::cancel(ToolContext &context)
{
    clearPoints(context);
    frameCaptured_ = false;
    planeLocked_ = false;
    normalAxisLockKey_ = 0;
    manualPlaneLock_ = false;
    drawingFrame_ = {};
    frameBeforeAxisLock_ = {};
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    status_.canCommit = false;
    publish(context);
}

ToolPreview ShapeCreationTool::preview() const
{
    ToolPreview result;
    result.points = points_;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = planeLocked_ || frameCaptured_;
    result.statusText = status_.text;
    return result;
}

ToolStatus ShapeCreationTool::status() const
{
    return status_;
}

const QVector<QPointF> &ShapeCreationTool::points() const
{
    return points_;
}

int ShapeCreationTool::requiredPointCount() const
{
    return requiredPoints_;
}

void ShapeCreationTool::clearPoints(ToolContext &context)
{
    points_.clear();
    frameCaptured_ = false;
    planeLocked_ = manualPlaneLock_ || normalAxisLockKey_ != 0;
    publish(context);
}

void ShapeCreationTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
