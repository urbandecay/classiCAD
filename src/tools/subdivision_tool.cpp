#include "subdivision_tool.h"

#include "core/commands/subdivision_command.h"
#include "core/document/document.h"
#include "core/geometry/curve_subdivision.h"
#include "tool_context.h"

#include <algorithm>
#include <cmath>

#include <Qt>

namespace classiCAD {

void SubdivisionTool::begin(ObjectId targetObjectId,
                            int initialSections,
                            int maximumSections)
{
    active_ = true;
    targetObjectId_ = targetObjectId;
    sections_ = std::clamp(initialSections, 2, maximumSections);
    resetWheelTracking();
    previewParameters_.clear();
}

void SubdivisionTool::finish()
{
    active_ = false;
    targetObjectId_ = ObjectId::invalid();
    sections_ = 2;
    resetWheelTracking();
    previewParameters_.clear();
}

void SubdivisionTool::resetWheelTracking()
{
    wheelAngleAccumulator_ = 0;
    wheelPixelAccumulator_ = 0.0;
}

int SubdivisionTool::wheelStepsFromEvent(int angleDelta, int pixelDelta)
{
    if (angleDelta != 0) {
        wheelPixelAccumulator_ = 0.0;
        if (wheelAngleAccumulator_ != 0 &&
            ((wheelAngleAccumulator_ > 0) != (angleDelta > 0))) {
            wheelAngleAccumulator_ = 0;
        }

        wheelAngleAccumulator_ += angleDelta;
        const int steps = wheelAngleAccumulator_ / 120;
        wheelAngleAccumulator_ -= steps * 120;
        return steps;
    }

    if (pixelDelta != 0) {
        wheelAngleAccumulator_ = 0;
        if (wheelPixelAccumulator_ != 0.0 &&
            ((wheelPixelAccumulator_ > 0.0) != (pixelDelta > 0))) {
            wheelPixelAccumulator_ = 0.0;
        }

        wheelPixelAccumulator_ += pixelDelta;
        constexpr qreal pixelsPerWheelStep = 40.0;
        const int magnitude = static_cast<int>(std::floor(
            std::abs(wheelPixelAccumulator_) / pixelsPerWheelStep));
        const int steps = wheelPixelAccumulator_ > 0.0
                              ? magnitude
                              : -magnitude;
        wheelPixelAccumulator_ -= steps * pixelsPerWheelStep;
        return steps;
    }

    return 0;
}

SubdivisionWheelResult SubdivisionTool::handleWheel(
    const ToolInput &input,
    int maximumSections,
    ToolContext &context)
{
    SubdivisionWheelResult result;
    if (!active_) {
        return result;
    }

    result.handled = true;
    result.logicalSteps = wheelStepsFromEvent(input.wheelAngleDelta,
                                              input.wheelPixelDelta);
    result.sectionsChanged = adjustSections(result.logicalSteps,
                                            maximumSections);
    if (result.sectionsChanged) {
        const SceneObject *target = context.document().object(targetObjectId_);
        refreshPreview(target != nullptr ? target->geometry : Shape{});
    }
    return result;
}

SubdivisionInputAction SubdivisionTool::handleMousePress(
    const ToolInput &input) const
{
    if (!active_) {
        return SubdivisionInputAction::Unhandled;
    }
    if (input.button == Qt::LeftButton) {
        return SubdivisionInputAction::Apply;
    }
    if (input.button == Qt::RightButton) {
        return SubdivisionInputAction::Cancel;
    }
    return SubdivisionInputAction::Unhandled;
}

SubdivisionInputAction SubdivisionTool::handleKey(int key) const
{
    if (!active_) {
        return SubdivisionInputAction::Unhandled;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        return SubdivisionInputAction::Apply;
    }
    if (key == Qt::Key_Escape) {
        return SubdivisionInputAction::Cancel;
    }
    return SubdivisionInputAction::Unhandled;
}

bool SubdivisionTool::adjustSections(int steps, int maximumSections)
{
    if (!active_ || steps == 0) {
        return false;
    }
    const int previousSections = sections_;
    sections_ = std::clamp(sections_ + steps, 2, maximumSections);
    return sections_ != previousSections;
}

void SubdivisionTool::refreshPreview(const Shape &shape)
{
    previewParameters_ = active_
                             ? equalArcLengthSubdivisionParameters(shape, sections_)
                             : QVector<double>();
}

const QVector<double> &SubdivisionTool::previewParameters() const
{
    return previewParameters_;
}

SubdivisionCommitResult SubdivisionTool::commit(
    ObjectId targetObjectId,
    int sections,
    const QVector<double> &parameters,
    ToolContext &context)
{
    SubdivisionCommitResult result;
    if (!targetObjectId.isValid() || sections < 2 ||
        parameters.size() != sections - 1) {
        return result;
    }

    const SceneObject *object = context.document().object(targetObjectId);
    if (object == nullptr || !isSubdividableCurveShape(object->geometry)) {
        return result;
    }

    result.changed = object->geometry.subdivisionParameters != parameters;
    if (result.changed) {
        DocumentTransaction transaction = context.beginTransaction();
        if (!SubdivisionCommand::apply(transaction, targetObjectId, parameters) ||
            !context.commitTransaction(transaction)) {
            return SubdivisionCommitResult{};
        }
    }

    result.committed = true;
    if (active_ && targetObjectId_ == targetObjectId) {
        finish();
    }
    return result;
}

QString SubdivisionTool::prompt() const
{
    if (!active_) {
        return QString();
    }
    return QStringLiteral("Subdivide: %1 sections  •  Scroll to change  •  Click/Enter to apply  •  Esc to cancel")
        .arg(sections_);
}

bool SubdivisionTool::isActive() const
{
    return active_;
}

ObjectId SubdivisionTool::targetObjectId() const
{
    return targetObjectId_;
}

int SubdivisionTool::sections() const
{
    return sections_;
}

int SubdivisionTool::wheelAngleAccumulator() const
{
    return wheelAngleAccumulator_;
}

qreal SubdivisionTool::wheelPixelAccumulator() const
{
    return wheelPixelAccumulator_;
}

} // namespace classiCAD
