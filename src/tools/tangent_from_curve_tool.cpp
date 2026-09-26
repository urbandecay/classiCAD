#include "tangent_from_curve_tool.h"

#include "tool_context.h"

#include <cmath>
#include <limits>

namespace classiCAD {

ToolId TangentFromCurveTool::id() const
{
    return ToolId::TangentFromCurve;
}

void TangentFromCurveTool::begin(ToolContext &context)
{
    curveObjectId_ = ObjectId::invalid();
    curvePickScreen_ = QPointF();
    tangentPoint_ = QPointF();
    tangentPreviewAvailable_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Click a curve to start a tangent line");
    status_.canCommit = false;
    publish(context);
}

bool TangentFromCurveTool::handleMousePress(const ToolInput &input,
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
            status_.text = QStringLiteral("Click a visible curve to start a tangent line");
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
        curvePickScreen_ = input.screenPosition;
        context.selection().setObjectIds({curveObjectId_}, curveObjectId_);
        status_.text = QStringLiteral("Curve selected — move to an endpoint and click to place the tangent line");
        publish(context);
        return true;
    }

    if (!updateTangentPreview(input, context)) {
        return true;
    }

    const QPointF endpoint = input.worldPosition;
    if (std::hypot(endpoint.x() - tangentPoint_.x(),
                   endpoint.y() - tangentPoint_.y()) <= 1.0e-9) {
        status_.text = QStringLiteral("Move farther from the curve to draw a tangent line");
        publish(context);
        return true;
    }

    Shape line;
    if (!context.createShape(ToolId::Line,
                             {tangentPoint_, endpoint},
                             ArcMode::TwoPoint,
                             0.0,
                             &line) ||
        !context.commitShape(ToolId::Line, line)) {
        status_.text = QStringLiteral("Could not create the tangent line");
        publish(context);
        return true;
    }

    status_.state = ToolLifecycleState::Completed;
    status_.text = QStringLiteral("Tangent line created");
    status_.canCommit = false;
    tangentPreviewAvailable_ = false;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

bool TangentFromCurveTool::handleMouseMove(const ToolInput &input,
                                           ToolContext &context)
{
    if (curveObjectId_.isValid()) {
        updateTangentPreview(input, context);
    }
    return true;
}

bool TangentFromCurveTool::handleKey(const ToolInput &input,
                                     ToolContext &context)
{
    if (input.key != Qt::Key_Escape) {
        return false;
    }

    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Tangent line cancelled");
    status_.canCommit = false;
    curveObjectId_ = ObjectId::invalid();
    tangentPreviewAvailable_ = false;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

void TangentFromCurveTool::cancel(ToolContext &context)
{
    curveObjectId_ = ObjectId::invalid();
    tangentPreviewAvailable_ = false;
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Tangent line cancelled");
    status_.canCommit = false;
    publish(context);
}

ToolPreview TangentFromCurveTool::preview() const
{
    ToolPreview result;
    if (tangentPreviewAvailable_) {
        result.points.append(tangentPoint_);
    }
    result.statusText = status_.text;
    return result;
}

ToolStatus TangentFromCurveTool::status() const
{
    return status_;
}

bool TangentFromCurveTool::updateTangentPreview(const ToolInput &input,
                                                ToolContext &context)
{
    const Shape *shape = context.document().shape(curveObjectId_);
    if (shape == nullptr) {
        curveObjectId_ = ObjectId::invalid();
        tangentPreviewAvailable_ = false;
        status_.text = QStringLiteral("Selected curve is no longer available");
        status_.canCommit = false;
        publish(context);
        return false;
    }

    const QVector<SnapCandidate> candidates =
        context.snapEngine().tangentCandidatesForShape(*shape,
                                                       input.worldPosition,
                                                       context.viewportTransform(),
                                                       input.viewportSize);
    qreal bestDistance = std::numeric_limits<qreal>::infinity();
    tangentPreviewAvailable_ = false;
    for (const SnapCandidate &candidate : candidates) {
        const QPointF candidateScreen = context.viewportTransform().worldToScreen(
            candidate.point, input.viewportSize);
        const qreal distance = std::hypot(candidateScreen.x() - curvePickScreen_.x(),
                                          candidateScreen.y() - curvePickScreen_.y());
        if (distance < bestDistance) {
            bestDistance = distance;
            tangentPoint_ = candidate.point;
            tangentPreviewAvailable_ = true;
        }
    }

    status_.state = ToolLifecycleState::Active;
    status_.canCommit = tangentPreviewAvailable_;
    status_.text = tangentPreviewAvailable_
                       ? QStringLiteral("Tangent ready — click to place the line; Esc cancels")
                       : QStringLiteral("Move the cursor outside the curve to find a tangent");
    publish(context);
    return tangentPreviewAvailable_;
}

void TangentFromCurveTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
