#include "line_tool.h"

#include "tool_context.h"

namespace classiCAD {

ToolId LineTool::id() const
{
    return ToolId::Line;
}

void LineTool::begin(ToolContext &context)
{
    points_.clear();
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Line input started");
    status_.canCommit = false;
    publish(context);
}

bool LineTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        commit(context);
        return true;
    }
    if (input.button != Qt::LeftButton) {
        return false;
    }

    points_.append(input.worldPosition);
    status_.canCommit = points_.size() >= 2;
    status_.text = QStringLiteral("Line: %1 points  •  Right-click to finish")
                       .arg(points_.size());
    publish(context);
    return true;
}

bool LineTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key != Qt::Key_Escape) {
        return false;
    }

    points_.clear();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Line cancelled");
    status_.canCommit = false;
    publish(context);
    context.finishTool(ToolId::Select);
    return true;
}

void LineTool::cancel(ToolContext &context)
{
    points_.clear();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Line cancelled");
    status_.canCommit = false;
    publish(context);
}

void LineTool::commit(ToolContext &context)
{
    if (points_.size() >= 2) {
        Shape shape;
        if (context.createShape(id(), points_, ArcMode::TwoPoint, 0.0, &shape) &&
            context.commitShape(id(), shape)) {
            status_.state = ToolLifecycleState::Completed;
            status_.text = QStringLiteral("Line committed");
        }
    } else {
        status_.text = QStringLiteral("Line discarded: at least two points required");
    }

    points_.clear();
    status_.canCommit = false;
    publish(context);
    context.finishTool(ToolId::Select);
}

ToolPreview LineTool::preview() const
{
    ToolPreview result;
    result.points = points_;
    result.statusText = status_.text;
    return result;
}

ToolStatus LineTool::status() const
{
    return status_;
}

void LineTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
