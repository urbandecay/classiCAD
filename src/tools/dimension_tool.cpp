#include "dimension_tool.h"

#include "tool_context.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qreal kMinimumLength = 1.0e-9;

qreal length(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

QString nextPointPrompt(ToolId tool, int fixedPointCount)
{
    if (tool == ToolId::LinearDimension) {
        return fixedPointCount == 0
                   ? QStringLiteral("Linear Dimension: pick first point")
                   : fixedPointCount == 1
                         ? QStringLiteral("Linear Dimension: pick second point")
                         : QStringLiteral("Linear Dimension: position the dimension and click");
    }
    return fixedPointCount == 0
               ? QStringLiteral("Angular Dimension: pick the vertex")
               : fixedPointCount == 1
                     ? QStringLiteral("Angular Dimension: pick the first ray (sets arc size)")
                     : QStringLiteral("Angular Dimension: pick the second ray");
}

} // namespace

DimensionTool::DimensionTool(ToolId tool)
    : tool_(tool)
{
}

ToolId DimensionTool::id() const
{
    return tool_;
}

void DimensionTool::begin(ToolContext &context)
{
    fixedPoints_.clear();
    cursor_ = QPointF{};
    minimumOffsetWorld_ = 2.0 / std::max(context.viewportTransform().zoom(), 1.0e-6);
    cursorValid_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = nextPointPrompt(tool_, 0);
    status_.canCommit = false;
    publish(context);
}

bool DimensionTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button != Qt::LeftButton) {
        return false;
    }

    cursor_ = input.worldPosition;
    minimumOffsetWorld_ = 2.0 / std::max(context.viewportTransform().zoom(), 1.0e-6);
    cursorValid_ = true;
    if (fixedPoints_.size() < 2) {
        if (!fixedPoints_.isEmpty() &&
            length(cursor_ - fixedPoints_.first()) <= kMinimumLength) {
            status_.text = tool_ == ToolId::LinearDimension
                               ? QStringLiteral("Dimension points must be different")
                               : QStringLiteral("Pick a ray point away from the vertex");
            publish(context);
            return true;
        }
        fixedPoints_.append(cursor_);
        status_.text = nextPointPrompt(tool_, fixedPoints_.size());
        publish(context);
        return true;
    }

    QVector<QPointF> points = fixedPoints_;
    points.append(cursor_);
    Shape shape;
    if (!makeShape(points, &shape)) {
        status_.text = QStringLiteral("The selected points do not define a dimension");
        publish(context);
        return true;
    }

    if (context.commitShape(tool_, shape)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("%1 committed").arg(toolName(tool_));
        status_.canCommit = false;
        fixedPoints_.clear();
        publish(context);
        context.finishTool(ToolId::Select);
    }
    return true;
}

bool DimensionTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    cursor_ = input.worldPosition;
    minimumOffsetWorld_ = 2.0 / std::max(context.viewportTransform().zoom(), 1.0e-6);
    cursorValid_ = true;
    status_.text = nextPointPrompt(tool_, fixedPoints_.size());
    publish(context);
    return true;
}

bool DimensionTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) {
        if (!fixedPoints_.isEmpty()) {
            fixedPoints_.removeLast();
        }
        status_.state = ToolLifecycleState::Active;
        status_.text = nextPointPrompt(tool_, fixedPoints_.size());
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_Escape) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }
    return false;
}

void DimensionTool::cancel(ToolContext &context)
{
    fixedPoints_.clear();
    cursorValid_ = false;
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    status_.canCommit = false;
    publish(context);
}

ToolPreview DimensionTool::preview() const
{
    ToolPreview result;
    result.points = fixedPoints_;
    result.statusText = status_.text;
    result.cursorVisible = cursorValid_;

    if (!cursorValid_ || fixedPoints_.isEmpty()) {
        return result;
    }

    if (tool_ == ToolId::LinearDimension) {
        QVector<QPointF> points;
        if (fixedPoints_.size() == 1) {
            const QPointF measured = cursor_ - fixedPoints_.first();
            const qreal measuredLength = length(measured);
            if (measuredLength <= kMinimumLength) {
                return result;
            }
            const QPointF direction = measured / measuredLength;
            const QPointF normal(-direction.y(), direction.x());
            const QPointF midpoint = (fixedPoints_.first() + cursor_) * 0.5;
            const qreal defaultOffset = std::max(measuredLength * 0.25,
                                                 minimumOffsetWorld_);
            points = {fixedPoints_.first(), cursor_, midpoint + normal * defaultOffset};
        } else {
            points = {fixedPoints_[0], fixedPoints_[1], cursor_};
        }
        if (makeShape(points, &result.shape)) {
            result.hasShape = true;
        }
        return result;
    }

    if (fixedPoints_.size() == 1) {
        if (length(cursor_ - fixedPoints_.first()) > kMinimumLength) {
            result.shape = Shape{GeometryType::Line,
                                 {fixedPoints_.first(), cursor_},
                                 {},
                                 ArcMode::TwoPoint,
                                 0.0,
                                 {},
                                 {}};
            result.hasShape = true;
        }
        return result;
    }

    const QVector<QPointF> points{fixedPoints_[0], fixedPoints_[1], cursor_};
    if (makeShape(points, &result.shape)) {
        result.hasShape = true;
    }
    return result;
}

ToolStatus DimensionTool::status() const
{
    return status_;
}

bool DimensionTool::makeShape(const QVector<QPointF> &points, Shape *shape) const
{
    if (shape == nullptr || points.size() != 3) {
        return false;
    }

    if (tool_ == ToolId::LinearDimension) {
        if (length(points[1] - points[0]) <= kMinimumLength) {
            return false;
        }
        *shape = Shape{GeometryType::LinearDimension,
                       points,
                       {},
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
        return true;
    }

    const QPointF firstRay = points[1] - points[0];
    const QPointF secondRay = points[2] - points[0];
    if (length(firstRay) <= kMinimumLength ||
        length(secondRay) <= kMinimumLength) {
        return false;
    }
    const qreal cross = firstRay.x() * secondRay.y() -
                        firstRay.y() * secondRay.x();
    const qreal dot = QPointF::dotProduct(firstRay, secondRay);
    if (std::abs(std::atan2(cross, dot)) <= 1.0e-8) {
        return false;
    }

    *shape = Shape{GeometryType::AngularDimension,
                   points,
                   {},
                   ArcMode::TwoPoint,
                   0.0,
                   {},
                   {}};
    return true;
}

void DimensionTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
