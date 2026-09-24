#include "shape_creation_tool.h"

#include "tool_context.h"

namespace classiCAD {

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

    points_.append(input.worldPosition);
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

bool ShapeCreationTool::handleKey(const ToolInput &input,
                                  ToolContext &context)
{
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
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    status_.canCommit = false;
    publish(context);
}

ToolPreview ShapeCreationTool::preview() const
{
    ToolPreview result;
    result.points = points_;
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
    publish(context);
}

void ShapeCreationTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
