#include "perpendicular_from_curve_tool.h"

#include "tool_context.h"

#include <cmath>

namespace classiCAD {

ToolId PerpendicularFromCurveTool::id() const
{
    return ToolId::PerpendicularFromCurve;
}

void PerpendicularFromCurveTool::begin(ToolContext &context)
{
    curveObjectId_ = ObjectId::invalid();
    perpendicularPoint_ = QPointF();
    perpendicularPreviewAvailable_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Click a curve to start a perpendicular line");
    status_.canCommit = false;
    publish(context);
}

bool PerpendicularFromCurveTool::handleMousePress(const ToolInput &input,
                                                  ToolContext &context)
{
    if (input.button != Qt::LeftButton) {
        return false;
    }

    if (!curveObjectId_.isValid()) {
        const int shapeIndex = context.curveHitTester().hitTestShape(
            context.document(),
            input.screenPosition,
            context.viewportTransform(),
            input.viewportSize);
        if (shapeIndex < 0) {
            status_.text = QStringLiteral("Click a visible curve to start a perpendicular line");
            publish(context);
            return true;
        }

        const Shape &shape = context.document()[shapeIndex];
        switch (shape.geometryType) {
        case GeometryType::Line:
        case GeometryType::Arc:
        case GeometryType::Bezier:
        case GeometryType::Nurbs:
        case GeometryType::Circle:
        case GeometryType::Ellipse:
        case GeometryType::PolyCurve:
            break;
        case GeometryType::Invalid:
        case GeometryType::Rectangle:
        case GeometryType::Point:
            status_.text = QStringLiteral("Choose a curve, not a point or rectangle");
            publish(context);
            return true;
        }

        curveObjectId_ = context.document().objectIdAt(shapeIndex);
        context.selection().setObjectIds({curveObjectId_}, curveObjectId_);
        status_.text = QStringLiteral(
            "Curve selected — move to a line endpoint and click to place the perpendicular line");
        publish(context);
        return true;
    }

    if (!updatePerpendicularPreview(input, context)) {
        return true;
    }

    const QPointF endpoint = input.worldPosition;
    if (std::hypot(endpoint.x() - perpendicularPoint_.x(),
                   endpoint.y() - perpendicularPoint_.y()) <= 1.0e-9) {
        status_.text = QStringLiteral("Move away from the curve to draw a perpendicular line");
        publish(context);
        return true;
    }

    Shape line;
    if (!context.createShape(ToolId::Line,
                             {perpendicularPoint_, endpoint},
                             ArcMode::TwoPoint,
                             0.0,
                             &line) ||
        !context.commitShape(ToolId::Line, line)) {
        status_.text = QStringLiteral("Could not create the perpendicular line");
        publish(context);
        return true;
    }

    status_.state = ToolLifecycleState::Completed;
    status_.text = QStringLiteral("Perpendicular line created");
    status_.canCommit = false;
    perpendicularPreviewAvailable_ = false;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

bool PerpendicularFromCurveTool::handleMouseMove(const ToolInput &input,
                                                 ToolContext &context)
{
    if (curveObjectId_.isValid()) {
        updatePerpendicularPreview(input, context);
    }
    return true;
}

bool PerpendicularFromCurveTool::handleKey(const ToolInput &input,
                                           ToolContext &context)
{
    if (input.key != Qt::Key_Escape) {
        return false;
    }

    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Perpendicular line cancelled");
    status_.canCommit = false;
    curveObjectId_ = ObjectId::invalid();
    perpendicularPreviewAvailable_ = false;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

void PerpendicularFromCurveTool::cancel(ToolContext &context)
{
    curveObjectId_ = ObjectId::invalid();
    perpendicularPreviewAvailable_ = false;
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Perpendicular line cancelled");
    status_.canCommit = false;
    publish(context);
}

ToolPreview PerpendicularFromCurveTool::preview() const
{
    ToolPreview result;
    if (perpendicularPreviewAvailable_) {
        result.points.append(perpendicularPoint_);
    }
    result.statusText = status_.text;
    return result;
}

ToolStatus PerpendicularFromCurveTool::status() const
{
    return status_;
}

bool PerpendicularFromCurveTool::updatePerpendicularPreview(
    const ToolInput &input,
    ToolContext &context)
{
    const Shape *shape = context.document().shape(curveObjectId_);
    if (shape == nullptr) {
        curveObjectId_ = ObjectId::invalid();
        perpendicularPreviewAvailable_ = false;
        status_.text = QStringLiteral("Selected curve is no longer available");
        status_.canCommit = false;
        publish(context);
        return false;
    }

    if (!context.snapEngine().perpendicularPointForShape(*shape,
                                                         input.worldPosition,
                                                         context.viewportTransform(),
                                                         input.viewportSize,
                                                         &perpendicularPoint_)) {
        perpendicularPreviewAvailable_ = false;
        status_.text = QStringLiteral("Could not find a perpendicular point on the curve");
        status_.canCommit = false;
        publish(context);
        return false;
    }

    const QPointF endpoint = input.worldPosition;
    perpendicularPreviewAvailable_ =
        std::hypot(endpoint.x() - perpendicularPoint_.x(),
                   endpoint.y() - perpendicularPoint_.y()) > 1.0e-9;
    status_.state = ToolLifecycleState::Active;
    status_.canCommit = perpendicularPreviewAvailable_;
    status_.text = perpendicularPreviewAvailable_
                       ? QStringLiteral("Perpendicular ready — click to place the line; Esc cancels")
                       : QStringLiteral("Move away from the curve to draw a perpendicular line");
    publish(context);
    return perpendicularPreviewAvailable_;
}

void PerpendicularFromCurveTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
