#include "scale_tool.h"

#include "core/commands/transform_command.h"
#include "core/geometry/geometry_transform.h"
#include "services/dimensions/dimension_association.h"
#include "services/viewport/viewport_transform.h"
#include "tool_context.h"

#include <cmath>

namespace classiCAD {

ToolId ScaleTool::id() const
{
    return ToolId::Scale;
}

void ScaleTool::begin(ToolContext &context)
{
    hasLastDispatchResult_ = false;
    hasLastKeyDispatchResult_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Scale");
    status_.canCommit = false;
    context.publishStatus(status_);
}

InteractionTool::EventResult ScaleTool::dispatchMousePress(
    const ToolInput &input, ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        ScaleDispatchResult result;
        result.cancelled = true;
        result.mode = interactionState_.mode;
        result.sourceCount = interactionState_.sourceObjectIds.size();
        resetInteraction();
        lastDispatchResult_ = result;
        hasLastDispatchResult_ = true;
        context.finishTool(ToolId::Select);
        return EventResult::Handled;
    }
    if (input.button != Qt::LeftButton) {
        return EventResult::Unhandled;
    }

    hasLastDispatchResult_ = false;
    if (interactionState_.sourceObjectIds.isEmpty()) {
        return EventResult::Handled;
    }

    ScaleDispatchResult result;
    result.point = acceptPoint(input.worldPosition);
    result.mode = interactionState_.mode;
    result.basePoint = interactionState_.basePoint;
    result.sourceCount = interactionState_.sourceObjectIds.size();
    if (result.point.action == ScalePointAction::CommitRequested) {
        result.commitAttempted = true;
        result.committed = commitScale(result.point.factor,
                                       result.point.axisDirection,
                                       context);
    }
    lastDispatchResult_ = result;
    hasLastDispatchResult_ = true;
    return EventResult::Handled;
}

InteractionTool::EventResult ScaleTool::dispatchKey(
    const ToolInput &input, ToolContext &context)
{
    if (interactionState_.sourceObjectIds.isEmpty()) {
        return EventResult::Unhandled;
    }

    ScaleKeyDispatchResult result;
    if (input.key == Qt::Key_Escape) {
        result.handled = true;
        result.cancelled = true;
        resetInteraction();
        context.finishTool(ToolId::Select);
    } else if (interactionState_.stage == 0 &&
               (input.key == Qt::Key_Return ||
                input.key == Qt::Key_Enter)) {
        result.point = acceptPoint(input.worldPosition);
        result.handled = result.point.action ==
                         ScalePointAction::BasePointCaptured;
        result.promptChanged = result.handled;
        result.redrawRequested = result.handled;
    } else if (interactionState_.stage == 1) {
        if (input.key == Qt::Key_Backspace) {
            backspaceFactorInput();
            result.handled = true;
            result.promptChanged = true;
        } else if (input.key == Qt::Key_Return ||
                   input.key == Qt::Key_Enter) {
            result.handled = true;
            if (!interactionState_.factorText.isEmpty()) {
                qreal factor = 1.0;
                if (acceptFactorInput(&factor)) {
                    if (interactionState_.mode == ScaleMode::OneD) {
                        updatePreview(input.worldPosition);
                        result.promptChanged = true;
                        result.redrawRequested = true;
                    } else {
                        result.mode = interactionState_.mode;
                        result.factor = factor;
                        result.basePoint = interactionState_.basePoint;
                        result.sourceCount = interactionState_.sourceObjectIds.size();
                        result.commitAttempted = true;
                        result.committed = commitScale(factor, QPointF(), context);
                        result.redrawRequested = result.committed;
                    }
                }
            }
        } else if (input.text.size() == 1) {
            const QChar character = input.text.front() == QLatin1Char(',')
                                        ? QLatin1Char('.')
                                        : input.text.front();
            if (appendFactorCharacter(character)) {
                result.handled = true;
                result.promptChanged = true;
            }
        }
    } else if (interactionState_.stage == 2 &&
               (input.key == Qt::Key_Return ||
                input.key == Qt::Key_Enter)) {
        result.handled = true;
        if (updatePreview(input.worldPosition)) {
            result.mode = interactionState_.mode;
            result.factor = interactionState_.previewFactor;
            result.basePoint = interactionState_.basePoint;
            result.sourceCount = interactionState_.sourceObjectIds.size();
            result.commitAttempted = true;
            result.committed = commitScale(interactionState_.previewFactor,
                                           interactionState_.previewAxis,
                                           context);
            result.redrawRequested = result.committed;
        }
    }

    if (!result.handled) {
        return EventResult::Unhandled;
    }
    lastKeyDispatchResult_ = result;
    hasLastKeyDispatchResult_ = true;
    return EventResult::Handled;
}

ToolStatus ScaleTool::status() const
{
    return status_;
}

QString ScaleTool::prompt() const
{
    if (interactionState_.stage == 0) {
        return QStringLiteral("%1: click a base point, or press Enter for the selection center")
            .arg(scaleModeName(interactionState_.mode));
    }
    if (interactionState_.stage == 1) {
        const QString numeric = interactionState_.factorText.isEmpty()
                                    ? QString()
                                    : QStringLiteral("  Factor: %1")
                                          .arg(interactionState_.factorText);
        return QStringLiteral("%1: type a factor + Enter, or click the first reference point%2")
            .arg(scaleModeName(interactionState_.mode), numeric);
    }
    if (interactionState_.usingTypedFactor) {
        return QStringLiteral("Scale 1D: click the scale direction  •  Factor %1")
            .arg(interactionState_.typedFactor, 0, 'g', 8);
    }
    return interactionState_.mode == ScaleMode::OneD
               ? QStringLiteral("Scale 1D: click the second reference point along the first-point axis")
               : QStringLiteral("%1: click the second reference point")
                     .arg(scaleModeName(interactionState_.mode));
}

void ScaleTool::beginSelection(
    const QVector<ObjectId> &sourceObjectIds,
    ScaleMode mode,
    const QHash<quint64, QSet<int>> &controlPointIndices)
{
    resetInteraction();
    interactionState_.sourceObjectIds = sourceObjectIds;
    interactionState_.mode = mode;
    interactionState_.controlPointIndices = controlPointIndices;
}

void ScaleTool::resetInteraction()
{
    const ScaleMode mode = interactionState_.mode;
    interactionState_ = InteractionState{};
    interactionState_.mode = mode;
}

ScalePointResult ScaleTool::acceptPoint(const QPointF &point)
{
    if (interactionState_.sourceObjectIds.isEmpty()) {
        return {};
    }

    if (interactionState_.stage == 0) {
        interactionState_.basePoint = point;
        interactionState_.stage = 1;
        ScalePointResult result;
        result.action = ScalePointAction::BasePointCaptured;
        result.point = point;
        return result;
    }

    if (interactionState_.stage == 1) {
        const QPointF reference = point - interactionState_.basePoint;
        const qreal referenceLength = std::hypot(reference.x(), reference.y());
        if (referenceLength <= 1.0e-12) {
            return {};
        }
        interactionState_.referenceLength = referenceLength;
        interactionState_.referencePoint = point;
        interactionState_.axisDirection = reference / referenceLength;
        interactionState_.usingTypedFactor = false;
        interactionState_.stage = 2;
        updatePreview(point);
        ScalePointResult result;
        result.action = ScalePointAction::ReferenceCaptured;
        result.point = point;
        return result;
    }

    if (!updatePreview(point)) {
        return {};
    }
    return {ScalePointAction::CommitRequested,
            point,
            interactionState_.previewFactor,
            interactionState_.previewAxis};
}

bool ScaleTool::commitScale(qreal factor,
                            const QPointF &axisDirection,
                            ToolContext &context)
{
    if (!std::isfinite(factor)) {
        return false;
    }

    if (std::abs(factor - 1.0) > 1.0e-12) {
        const InteractionState &state = interactionState_;
        const WorkPlaneFrame surfaceFrame = context.viewportTransform().workPlaneFrame();
        DocumentTransaction transaction = context.beginTransaction();
        const bool componentEdit = !state.controlPointIndices.isEmpty();
        const bool edited = componentEdit
            ? TransformCommand::applyPerObject(
                  context.document(), transaction, state.sourceObjectIds,
                  [base = state.basePoint, axisDirection, factor,
                   oneDimensional = state.mode == ScaleMode::OneD,
                   surfaceFrame,
                   indices = state.controlPointIndices](ObjectId objectId,
                                                        Shape &shape) {
                      const auto targets = indices.constFind(objectId.value());
                      if (targets == indices.cend()) {
                          return;
                      }
                      transformShapeControlPoints(
                          &shape, targets.value(),
                          [base, axisDirection, factor, oneDimensional,
                           surfaceFrame](const Point3D &point) {
                              return scalePointInFrame(
                                  point, base, axisDirection, factor,
                                  oneDimensional, surfaceFrame);
                          });
                  })
            : TransformCommand::apply(
                  context.document(), transaction, state.sourceObjectIds,
                  [base = state.basePoint, axisDirection, factor,
                   oneDimensional = state.mode == ScaleMode::OneD,
                   surfaceFrame](Shape &shape) {
                      scaleShapeGeometry(&shape, base, axisDirection, factor,
                                         oneDimensional, surfaceFrame);
                  });
        if (edited) {
            updateAssociativeDimensions(context.document(), context.curveSampler());
            context.commitTransaction(transaction);
            context.notifyLayersChanged();
        }
    }

    context.finishTool(ToolId::Select);
    return true;
}

bool ScaleTool::updatePreview(const QPointF &point)
{
    interactionState_.previewValid = false;
    if (interactionState_.stage != 2) {
        return false;
    }

    if (interactionState_.usingTypedFactor) {
        const QPointF direction = point - interactionState_.basePoint;
        const qreal length = std::hypot(direction.x(), direction.y());
        if (length <= 1.0e-12) {
            return false;
        }
        interactionState_.previewAxis = direction / length;
        interactionState_.previewFactor = interactionState_.typedFactor;
    } else {
        interactionState_.previewAxis = interactionState_.axisDirection;
        if (interactionState_.referenceLength <= 1.0e-12) {
            return false;
        }
        const QPointF offset = point - interactionState_.basePoint;
        interactionState_.previewFactor = interactionState_.mode == ScaleMode::OneD
            ? QPointF::dotProduct(offset, interactionState_.axisDirection) /
                  interactionState_.referenceLength
            : std::hypot(offset.x(), offset.y()) /
                  interactionState_.referenceLength;
    }

    interactionState_.previewValid =
        std::isfinite(interactionState_.previewFactor) &&
        (interactionState_.mode != ScaleMode::OneD ||
         std::hypot(interactionState_.previewAxis.x(),
                    interactionState_.previewAxis.y()) > 1.0e-12);
    return interactionState_.previewValid;
}

bool ScaleTool::appendFactorCharacter(QChar character)
{
    const bool digit = character.isDigit();
    const bool decimal = character == QLatin1Char('.') &&
                         !interactionState_.factorText.contains(QLatin1Char('.'));
    const bool sign = character == QLatin1Char('-') &&
                      interactionState_.factorText.isEmpty();
    if (!digit && !decimal && !sign) {
        return false;
    }
    interactionState_.factorText.append(character);
    return true;
}

void ScaleTool::backspaceFactorInput()
{
    interactionState_.factorText.chop(1);
}

bool ScaleTool::acceptFactorInput(qreal *factor)
{
    if (factor == nullptr) {
        return false;
    }
    bool valid = false;
    const qreal parsed = interactionState_.factorText.toDouble(&valid);
    if (!valid || !std::isfinite(parsed)) {
        return false;
    }

    interactionState_.factorText.clear();
    if (interactionState_.mode == ScaleMode::OneD) {
        interactionState_.usingTypedFactor = true;
        interactionState_.typedFactor = parsed;
        interactionState_.stage = 2;
    }
    *factor = parsed;
    return true;
}

ScaleDispatchResult ScaleTool::takeLastDispatchResult()
{
    if (!hasLastDispatchResult_) {
        return {};
    }
    hasLastDispatchResult_ = false;
    ScaleDispatchResult result = lastDispatchResult_;
    lastDispatchResult_ = ScaleDispatchResult{};
    return result;
}

ScaleKeyDispatchResult ScaleTool::takeLastKeyDispatchResult()
{
    if (!hasLastKeyDispatchResult_) {
        return {};
    }
    hasLastKeyDispatchResult_ = false;
    ScaleKeyDispatchResult result = lastKeyDispatchResult_;
    lastKeyDispatchResult_ = ScaleKeyDispatchResult{};
    return result;
}

const ScaleTool::InteractionState &ScaleTool::interactionState() const
{
    return interactionState_;
}

} // namespace classiCAD
