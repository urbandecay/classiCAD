#include "rotate_tool.h"

#include "core/commands/transform_command.h"
#include "core/geometry/geometry_transform.h"
#include "services/dimensions/dimension_association.h"
#include "services/viewport/viewport_transform.h"
#include "tool_context.h"

#include <cmath>

namespace classiCAD {
namespace {

qreal vectorDot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

qreal vectorLength(const Point3D &value)
{
    return std::sqrt(vectorDot(value, value));
}

Point3D vectorSubtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D vectorScale(const Point3D &value, qreal scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

Point3D vectorCross(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

QPointF snappedRotatePoint(const RotateTool::InteractionState &state,
                           const QPointF &point,
                           const SnapResult &snap)
{
    if (state.stage > 0 && snap.isValid() && snap.hasWorldPoint &&
        isValidWorkPlaneFrame(state.frame)) {
        return worldPointToWorkPlaneFrame(snap.worldPoint, state.frame);
    }
    return point;
}

qreal snapRotateAngle(qreal angle,
                      qreal incrementDegrees,
                      qreal strengthDegrees)
{
    constexpr qreal pi = 3.14159265358979323846;
    const qreal increment = incrementDegrees * pi / 180.0;
    const qreal strength = strengthDegrees * pi / 180.0;
    const qreal nearest = std::round(angle / increment) * increment;
    return std::abs(angle - nearest) <= strength ? nearest : angle;
}

void changeRotateFrame(const ToolInput &input,
                      const WorkPlaneFrame &frame,
                      const Point3D &fallbackWorld,
                      RotateFrameResult *result,
                      ToolContext &context)
{
    if (result == nullptr) {
        return;
    }
    context.viewportTransform().setWorkPlaneFrame(frame);
    if (!context.viewportTransform().screenToWorkPlane(
            input.screenPosition, input.viewportSize, frame,
            &result->cursorPoint)) {
        result->cursorPoint = worldPointToWorkPlaneFrame(fallbackWorld, frame);
    }
    result->snapResult = input.snapResult;
    if (result->snapResult.isValid() && result->snapResult.hasWorldPoint) {
        result->snapResult.point = worldPointToWorkPlaneFrame(
            result->snapResult.worldPoint, frame);
    }
    result->changed = true;
    result->updateCursor = true;
}

} // namespace

ToolId RotateTool::id() const
{
    return ToolId::Rotate;
}

void RotateTool::begin(ToolContext &context)
{
    hasLastKeyDispatchResult_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.text = QStringLiteral("Rotate");
    status_.canCommit = false;
    context.publishStatus(status_);
}

InteractionTool::EventResult RotateTool::dispatchMousePress(
    const ToolInput &input, ToolContext &context)
{
    if (input.button != Qt::RightButton) {
        return EventResult::Unhandled;
    }
    const bool angleSnapEnabled = interactionState_.angleSnapEnabled;
    resetInteraction(angleSnapEnabled);
    hasLastKeyDispatchResult_ = false;
    context.finishTool(ToolId::Select);
    return EventResult::Handled;
}

InteractionTool::EventResult RotateTool::dispatchKey(
    const ToolInput &input, ToolContext &context)
{
    hasLastKeyDispatchResult_ = false;
    const RotateKeyResult result = handleKeyInput(
        input.key, input.text, input.modifiers, input.autoRepeat, input,
        snapIncrementDegrees_, snapStrengthDegrees_, context);
    if (!result.handled) {
        return EventResult::Unhandled;
    }
    lastKeyDispatchResult_ = result;
    hasLastKeyDispatchResult_ = true;
    return EventResult::Handled;
}

ToolStatus RotateTool::status() const
{
    return status_;
}

void RotateTool::resetInteraction(bool angleSnapEnabled)
{
    interactionState_ = InteractionState{};
    interactionState_.angleSnapEnabled = angleSnapEnabled;
}

void RotateTool::beginSelection(const QVector<ObjectId> &sourceObjectIds,
                                bool angleSnapEnabled,
                                const QHash<quint64, QSet<int>> &controlPointIndices)
{
    resetInteraction(angleSnapEnabled);
    interactionState_.sourceObjectIds = sourceObjectIds;
    interactionState_.controlPointIndices = controlPointIndices;
}

RotatePointResult RotateTool::acceptPoint(const ToolInput &input,
                                          qreal snapIncrementDegrees,
                                          qreal snapStrengthDegrees,
                                          ToolContext &context)
{
    RotatePointResult result;
    if (interactionState_.sourceObjectIds.isEmpty()) {
        return result;
    }

    constexpr qreal minimumPointDistance = 1.0e-9;
    InteractionState &state = interactionState_;
    if (state.stage == 0) {
        const WorkPlaneFrame inputFrame = isValidWorkPlaneFrame(input.workPlaneFrame)
                                              ? input.workPlaneFrame
                                              : makeWorkPlaneFrame(WorkPlane::XY, 0.0);
        state.baseWorldPoint = input.snapResult.isValid() &&
                                       input.snapResult.hasWorldPoint
                                   ? input.snapResult.worldPoint
                                   : workPlaneFramePointToWorld(input.worldPosition,
                                                                inputFrame);
        state.frame = inputFrame;
        state.frame.origin = state.baseWorldPoint;
        const bool usedPrePivotPerpendicular =
            state.prePivotPerpendicularActive &&
            isValidWorkPlaneFrame(state.prePivotFloorFrame);
        state.primaryFrame = usedPrePivotPerpendicular
                                 ? state.prePivotFloorFrame
                                 : state.frame;
        state.primaryFrame.origin = state.baseWorldPoint;
        state.referenceNormal = usedPrePivotPerpendicular
                                    ? state.prePivotFloorNormal
                                    : state.frame.normal;
        state.prePivotPerpendicularActive = false;
        state.prePivotPlaneFrame = {};
        state.prePivotFloorFrame = {};
        state.prePivotFloorNormal = {};
        state.axisLockKey = 0;
        state.basePoint = QPointF();
        state.referencePoint = QPointF();
        state.stage = 1;
        state.previewAngle = 0.0;
        state.accumulatedAngle = 0.0;
        state.hasPreviousAngle = false;
        state.angleInputManual = false;
        state.angleInputActive = false;
        state.angleInput.clear();
        state.angleInputDirectionCaptured = false;
        context.viewportTransform().setWorkPlaneFrame(state.frame);

        result.action = RotatePointAction::PivotCaptured;
        result.updateCursor = true;
        result.cursorPoint = state.basePoint;
        result.frame = state.frame;
        return result;
    }

    if (state.stage == 1) {
        QPointF cursorPoint = input.worldPosition;
        if (!updateReferencePreview(input.worldPosition,
                                    input.snapResult,
                                    snapIncrementDegrees,
                                    snapStrengthDegrees,
                                    &cursorPoint)) {
            return result;
        }
        result.updateCursor = true;
        result.cursorPoint = cursorPoint;
        const QPointF reference = cursorPoint - state.basePoint;
        if (std::hypot(reference.x(), reference.y()) <= minimumPointDistance) {
            return result;
        }

        state.referencePoint = cursorPoint;
        state.referenceWorldPoint = workPlaneFramePointToWorld(
            state.referencePoint, state.frame);
        state.lastPointerPoint = state.referencePoint;
        state.referenceAngle = std::atan2(reference.y(), reference.x());
        state.stage = 2;
        state.previewAngle = 0.0;
        state.accumulatedAngle = 0.0;
        state.lastRawAngle = state.referenceAngle;
        state.hasPreviousAngle = true;
        state.angleInputManual = false;
        state.angleInputActive = false;
        state.angleInput.clear();

        result.action = RotatePointAction::ReferenceCaptured;
        result.frame = state.frame;
        return result;
    }

    QPointF cursorPoint = input.worldPosition;
    updatePreview(input.worldPosition,
                  input.snapResult,
                  snapIncrementDegrees,
                  snapStrengthDegrees,
                  &cursorPoint);
    if (state.angleInputActive && !state.angleInput.isEmpty() &&
        !setTypedAngle(state.angleInput, &cursorPoint)) {
        return result;
    }
    result.updateCursor = true;
    result.updateRawCursor = state.angleInputManual;
    result.cursorPoint = cursorPoint;
    if (std::hypot(input.worldPosition.x() - state.basePoint.x(),
                   input.worldPosition.y() - state.basePoint.y()) <=
            minimumPointDistance &&
        std::abs(state.previewAngle) <= minimumPointDistance) {
        return result;
    }

    result.action = RotatePointAction::CommitRequested;
    result.frame = state.frame;
    result.angle = state.previewAngle;
    return result;
}

bool RotateTool::commitAngle(qreal angle,
                             bool defaultAngleSnapEnabled,
                             ToolContext &context)
{
    if (!std::isfinite(angle)) {
        return false;
    }
    if (std::abs(angle) > 1.0e-12) {
        DocumentTransaction transaction = context.beginTransaction();
        const InteractionState &state = interactionState_;
        const bool componentEdit = !state.controlPointIndices.isEmpty();
        const bool edited = componentEdit
            ? TransformCommand::applyPerObject(
                  context.document(), transaction, state.sourceObjectIds,
                  [pivot = state.baseWorldPoint,
                   axis = state.frame.normal,
                   angle,
                   indices = state.controlPointIndices](ObjectId objectId,
                                                        Shape &shape) {
                      const auto targets = indices.constFind(objectId.value());
                      if (targets == indices.cend()) {
                          return;
                      }
                      transformShapeControlPoints(
                          &shape, targets.value(),
                          [pivot, axis, angle](const Point3D &point) {
                              return rotatePointAboutAxis(point, pivot, axis,
                                                          angle);
                          });
                  })
            : TransformCommand::apply(
                  context.document(), transaction, state.sourceObjectIds,
                  [pivot = state.baseWorldPoint,
                   axis = state.frame.normal,
                   angle](Shape &shape) {
                      rotateShapeGeometry(&shape, pivot, axis, angle);
                  });
        if (edited) {
            updateAssociativeDimensions(context.document(), context.curveSampler());
            context.commitTransaction(transaction);
            context.notifyLayersChanged();
        }
    }

    resetInteraction(defaultAngleSnapEnabled);
    context.finishTool(ToolId::Select);
    return true;
}

RotateFrameResult RotateTool::handleAxisKey(int key,
                                           const ToolInput &input,
                                           ToolContext &context)
{
    RotateFrameResult result;
    if (interactionState_.stage != 0 ||
        (key != Qt::Key_X && key != Qt::Key_Y && key != Qt::Key_Z)) {
        return result;
    }
    result.handled = true;

    const WorkPlaneFrame currentFrame = context.viewportTransform().workPlaneFrame();
    if (!isValidWorkPlaneFrame(currentFrame)) {
        return result;
    }
    InteractionState &state = interactionState_;
    if (state.axisLockKey == key) {
        state.axisLockKey = 0;
        state.prePivotPerpendicularActive = false;
        state.prePivotPlaneFrame = {};
        state.prePivotFloorFrame = {};
        state.prePivotFloorNormal = {};
        result.restoreDrawingFrame = true;
        return result;
    }

    const Point3D cursorWorld = workPlaneFramePointToWorld(
        input.worldPosition, currentFrame);
    Point3D normal;
    if (key == Qt::Key_X) normal = {1.0, 0.0, 0.0};
    if (key == Qt::Key_Y) normal = {0.0, 1.0, 0.0};
    if (key == Qt::Key_Z) normal = {0.0, 0.0, 1.0};
    const Point3D preferred = key == Qt::Key_X
                                  ? currentFrame.yAxis
                                  : currentFrame.xAxis;
    const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
        cursorWorld, normal, preferred);
    if (!isValidWorkPlaneFrame(frame)) {
        return result;
    }

    state.prePivotPlaneFrame = frame;
    state.axisLockKey = key;
    state.prePivotPerpendicularActive = false;
    state.prePivotFloorFrame = {};
    state.prePivotFloorNormal = {};
    changeRotateFrame(input, frame, cursorWorld, &result, context);
    return result;
}

RotateFrameResult RotateTool::togglePerpendicularPlane(const ToolInput &input,
                                                       ToolContext &context)
{
    RotateFrameResult result;
    result.handled = true;
    InteractionState &state = interactionState_;
    const WorkPlaneFrame currentFrame = context.viewportTransform().workPlaneFrame();

    if (state.stage == 0) {
        if (state.prePivotPerpendicularActive) {
            state.prePivotPerpendicularActive = false;
            state.prePivotPlaneFrame = {};
            state.prePivotFloorFrame = {};
            state.prePivotFloorNormal = {};
            state.axisLockKey = 0;
            result.restoreDrawingFrame = true;
            return result;
        }
        if (!isValidWorkPlaneFrame(currentFrame)) {
            return result;
        }

        const Point3D viewDirection = context.viewportTransform().viewDirection();
        const qreal xAlignment = std::abs(vectorDot(viewDirection,
                                                    currentFrame.xAxis));
        const qreal yAlignment = std::abs(vectorDot(viewDirection,
                                                    currentFrame.yAxis));
        const Point3D normal = xAlignment > yAlignment
                                   ? currentFrame.xAxis
                                   : currentFrame.yAxis;
        const Point3D preferred = xAlignment > yAlignment
                                      ? currentFrame.yAxis
                                      : currentFrame.xAxis;
        const Point3D cursorWorld = workPlaneFramePointToWorld(
            input.worldPosition, currentFrame);
        const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
            cursorWorld, normal, preferred);
        if (!isValidWorkPlaneFrame(frame)) {
            return result;
        }

        state.prePivotPlaneFrame = frame;
        state.prePivotFloorFrame = currentFrame;
        state.prePivotFloorNormal = currentFrame.normal;
        state.prePivotPerpendicularActive = true;
        state.axisLockKey = 0;
        changeRotateFrame(input, frame, cursorWorld, &result, context);
        return result;
    }

    const WorkPlaneFrame oldFrame = state.frame;
    const Point3D targetWorld = workPlaneFramePointToWorld(
        input.worldPosition, oldFrame);
    WorkPlaneFrame nextFrame;
    if (state.perpendicularActive) {
        nextFrame = state.primaryFrame;
        state.perpendicularActive = false;
    } else {
        const Point3D bridgePoint = state.stage == 1
                                        ? targetWorld
                                        : state.referenceWorldPoint;
        Point3D bridge = vectorSubtract(bridgePoint, state.baseWorldPoint);
        bridge = vectorSubtract(
            bridge,
            vectorScale(state.referenceNormal,
                        vectorDot(bridge, state.referenceNormal)));
        const qreal bridgeLength = vectorLength(bridge);
        if (bridgeLength <= 1.0e-9) {
            return result;
        }
        const Point3D xAxis = vectorScale(bridge, 1.0 / bridgeLength);
        const Point3D yAxis = state.referenceNormal;
        const Point3D normal = vectorCross(xAxis, yAxis);
        nextFrame = WorkPlaneFrame{state.baseWorldPoint,
                                   xAxis,
                                   yAxis,
                                   normal,
                                   true};
        state.perpendicularActive = true;
    }

    if (!isValidWorkPlaneFrame(nextFrame)) {
        return result;
    }
    state.frame = nextFrame;
    changeRotateFrame(input, state.frame, targetWorld, &result, context);
    if (state.stage == 2) {
        state.referencePoint = worldPointToWorkPlaneFrame(
            state.referenceWorldPoint, state.frame);
        const QPointF reference = state.referencePoint - state.basePoint;
        state.referenceAngle = std::atan2(reference.y(), reference.x());
        state.angleInputActive = false;
        state.angleInputManual = false;
        state.angleInput.clear();
        const QPointF direction = result.cursorPoint - state.basePoint;
        if (std::hypot(direction.x(), direction.y()) > 1.0e-12) {
            state.lastRawAngle = std::atan2(direction.y(), direction.x());
            state.accumulatedAngle = angleForPoint(result.cursorPoint);
            state.previewAngle = state.accumulatedAngle;
            state.hasPreviousAngle = true;
        } else {
            state.hasPreviousAngle = false;
        }
        state.lastPointerPoint = result.cursorPoint;
    }
    return result;
}

RotateKeyResult RotateTool::handleKeyInput(int key,
                                          const QString &text,
                                          Qt::KeyboardModifiers modifiers,
                                          bool autoRepeat,
                                          const ToolInput &input,
                                          qreal snapIncrementDegrees,
                                          qreal snapStrengthDegrees,
                                          ToolContext &context)
{
    RotateKeyResult result;
    if (key == Qt::Key_Escape) {
        result.handled = true;
        result.cancelled = true;
        const bool angleSnapEnabled = interactionState_.angleSnapEnabled;
        resetInteraction(angleSnapEnabled);
        context.finishTool(ToolId::Select);
        return result;
    }
    if (autoRepeat || modifiers != Qt::NoModifier) {
        return result;
    }

    InteractionState &state = interactionState_;
    if ((key == Qt::Key_Return || key == Qt::Key_Enter ||
         key == Qt::Key_Space) && state.stage == 2) {
        result.handled = true;
        if (state.angleInputActive && !state.angleInput.isEmpty()) {
            QPointF cursorPoint;
            if (!setTypedAngle(state.angleInput, &cursorPoint)) {
                return result;
            }
            result.updateCursor = true;
            result.updateRawCursor = true;
            result.cursorPoint = cursorPoint;
        }
        result.commitRequested = true;
        result.commitAngle = state.previewAngle;
        return result;
    }

    if (state.stage == 2 && key == Qt::Key_A) {
        state.angleInputActive = true;
        state.angleInputManual = false;
        state.angleInput.clear();
        state.angleInputDirectionCaptured = false;
        result.handled = true;
        return result;
    }

    if (key == Qt::Key_C) {
        state.angleSnapEnabled = !state.angleSnapEnabled;
        if (state.stage == 1) {
            QPointF cursorPoint;
            if (updateReferencePreview(input.worldPosition,
                                       input.snapResult,
                                       snapIncrementDegrees,
                                       snapStrengthDegrees,
                                       &cursorPoint)) {
                result.updateCursor = true;
                result.cursorPoint = cursorPoint;
            }
        } else if (state.stage == 2 && state.angleSnapEnabled &&
                   !input.snapResult.isValid()) {
            const QPointF offset = input.worldPosition - state.basePoint;
            if (std::hypot(offset.x(), offset.y()) > 1.0e-12) {
                state.lastRawAngle = std::atan2(offset.y(), offset.x());
                state.hasPreviousAngle = true;
            }
        }
        result.handled = true;
        return result;
    }

    if (key == Qt::Key_P) {
        result.frame = togglePerpendicularPlane(input, context);
        result.handled = result.frame.handled;
        return result;
    }

    if (state.stage == 0 &&
        (key == Qt::Key_X || key == Qt::Key_Y || key == Qt::Key_Z)) {
        result.frame = handleAxisKey(key, input, context);
        result.handled = result.frame.handled;
        return result;
    }

    if (state.stage == 2 && state.angleInputActive) {
        if (key == Qt::Key_Backspace) {
            state.angleInput.chop(1);
            state.angleInputManual = !state.angleInput.isEmpty();
            if (state.angleInput.isEmpty()) {
                state.angleInputDirectionCaptured = false;
                state.angleInputManual = false;
                result.cursorPoint = input.worldPosition;
                result.updateCursor = updatePreview(
                    state.lastPointerPoint, input.snapResult,
                    snapIncrementDegrees, snapStrengthDegrees,
                    &result.cursorPoint);
            } else if (!setTypedAngle(state.angleInput, &result.cursorPoint)) {
                return result;
            } else {
                result.updateCursor = true;
            }
            result.updateRawCursor = state.angleInputManual;
            result.handled = true;
            return result;
        }

        if (text.size() == 1) {
            const QChar character = text.front();
            const bool digit = character.isDigit();
            const bool decimal = character == QLatin1Char('.') &&
                                 !state.angleInput.contains(QLatin1Char('.'));
            const bool sign = (character == QLatin1Char('-') ||
                               character == QLatin1Char('+')) &&
                              state.angleInput.isEmpty();
            if (digit || decimal || sign) {
                if (state.angleInput.isEmpty()) {
                    state.angleInputDirectionCaptured = false;
                }
                state.angleInput.append(character);
                if (!setTypedAngle(state.angleInput, &result.cursorPoint)) {
                    return result;
                }
                result.updateCursor = true;
                result.updateRawCursor = true;
                result.handled = true;
                return result;
            }
        }
    }

    if (state.stage == 2 && text.size() == 1 &&
        (text.front().isDigit() || text.front() == QLatin1Char('.') ||
         text.front() == QLatin1Char('-') || text.front() == QLatin1Char('+'))) {
        state.angleInputActive = true;
        state.angleInput = text;
        state.angleInputDirectionCaptured = false;
        if (!setTypedAngle(state.angleInput, &result.cursorPoint)) {
            return result;
        }
        result.updateCursor = true;
        result.updateRawCursor = true;
        result.handled = true;
    }
    return result;
}

RotateKeyResult RotateTool::takeLastKeyDispatchResult()
{
    if (!hasLastKeyDispatchResult_) {
        return {};
    }
    hasLastKeyDispatchResult_ = false;
    RotateKeyResult result = lastKeyDispatchResult_;
    lastKeyDispatchResult_ = RotateKeyResult{};
    return result;
}

void RotateTool::setAngleSnapParameters(qreal incrementDegrees,
                                        qreal strengthDegrees)
{
    if (std::isfinite(incrementDegrees) && incrementDegrees > 0.0) {
        snapIncrementDegrees_ = incrementDegrees;
    }
    if (std::isfinite(strengthDegrees) && strengthDegrees >= 0.0) {
        snapStrengthDegrees_ = strengthDegrees;
    }
}

void RotateTool::setAngleSnapEnabled(bool enabled)
{
    interactionState_.angleSnapEnabled = enabled;
}

bool RotateTool::toggleAngleSnap()
{
    interactionState_.angleSnapEnabled = !interactionState_.angleSnapEnabled;
    return interactionState_.angleSnapEnabled;
}

qreal RotateTool::angleForPoint(const QPointF &worldPoint) const
{
    const QPointF startVector = interactionState_.referencePoint -
                                interactionState_.basePoint;
    const QPointF endVector = worldPoint - interactionState_.basePoint;
    if (std::hypot(startVector.x(), startVector.y()) <= 1.0e-12 ||
        std::hypot(endVector.x(), endVector.y()) <= 1.0e-12) {
        return 0.0;
    }

    constexpr qreal pi = 3.14159265358979323846;
    const qreal angle = std::atan2(endVector.y(), endVector.x()) -
                        std::atan2(startVector.y(), startVector.x());
    return std::remainder(angle, 2.0 * pi);
}

bool RotateTool::updateReferencePreview(const QPointF &point,
                                        const SnapResult &snap,
                                        qreal snapIncrementDegrees,
                                        qreal snapStrengthDegrees,
                                        QPointF *cursorPoint)
{
    if (cursorPoint == nullptr || interactionState_.stage != 1) {
        return false;
    }
    const QPointF planePoint = snappedRotatePoint(interactionState_, point, snap);
    const QPointF offset = planePoint - interactionState_.basePoint;
    const qreal radius = std::hypot(offset.x(), offset.y());
    if (radius <= 1.0e-12) {
        return false;
    }

    qreal angle = std::atan2(offset.y(), offset.x());
    if (interactionState_.angleSnapEnabled && !snap.isValid()) {
        angle = snapRotateAngle(angle, snapIncrementDegrees,
                                snapStrengthDegrees);
    }
    *cursorPoint = interactionState_.basePoint +
                   QPointF(radius * std::cos(angle), radius * std::sin(angle));
    return true;
}

bool RotateTool::updatePreview(const QPointF &point,
                               const SnapResult &snap,
                               qreal snapIncrementDegrees,
                               qreal snapStrengthDegrees,
                               QPointF *cursorPoint)
{
    if (cursorPoint == nullptr || interactionState_.stage != 2) {
        return false;
    }

    interactionState_.lastPointerPoint =
        snappedRotatePoint(interactionState_, point, snap);
    if (interactionState_.angleInputManual) {
        return updateCursorForPreviewAngle(cursorPoint);
    }

    const QPointF offset = interactionState_.lastPointerPoint -
                           interactionState_.basePoint;
    if (std::hypot(offset.x(), offset.y()) <= 1.0e-12) {
        return false;
    }

    const qreal radius = std::hypot(offset.x(), offset.y());
    qreal rawAngle = std::atan2(offset.y(), offset.x());
    if (snap.isValid()) {
        *cursorPoint = interactionState_.lastPointerPoint;
    } else if (interactionState_.angleSnapEnabled) {
        rawAngle = snapRotateAngle(rawAngle, snapIncrementDegrees,
                                   snapStrengthDegrees);
        *cursorPoint = interactionState_.basePoint +
                       QPointF(radius * std::cos(rawAngle),
                               radius * std::sin(rawAngle));
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    if (!interactionState_.hasPreviousAngle) {
        interactionState_.lastRawAngle = rawAngle;
        interactionState_.hasPreviousAngle = true;
    } else {
        qreal delta = rawAngle - interactionState_.lastRawAngle;
        if (delta > pi) {
            delta -= twoPi;
        } else if (delta < -pi) {
            delta += twoPi;
        }
        interactionState_.accumulatedAngle += delta;
        interactionState_.lastRawAngle = rawAngle;
    }
    interactionState_.previewAngle = interactionState_.accumulatedAngle;
    return true;
}

bool RotateTool::setTypedAngle(const QString &text, QPointF *cursorPoint)
{
    if (cursorPoint == nullptr) {
        return false;
    }
    bool validAngle = false;
    const qreal degrees = text.toDouble(&validAngle);
    if (!validAngle || !std::isfinite(degrees)) {
        return false;
    }
    if (!interactionState_.angleInputDirectionCaptured) {
        interactionState_.angleInputDirection =
            interactionState_.accumulatedAngle < -1.0e-12 ? -1.0 : 1.0;
        interactionState_.angleInputDirectionCaptured = true;
    }
    constexpr qreal pi = 3.14159265358979323846;
    const qreal angleRadians = degrees * pi / 180.0;
    interactionState_.previewAngle =
        interactionState_.angleInputDirection * angleRadians;
    interactionState_.accumulatedAngle = interactionState_.previewAngle;
    interactionState_.angleInputManual = true;
    return updateCursorForPreviewAngle(cursorPoint);
}

bool RotateTool::updateCursorForPreviewAngle(QPointF *cursorPoint) const
{
    if (cursorPoint == nullptr || interactionState_.stage != 2) {
        return false;
    }
    QPointF radial = interactionState_.lastPointerPoint -
                     interactionState_.basePoint;
    qreal radius = std::hypot(radial.x(), radial.y());
    if (radius <= 1.0e-12) {
        radial = interactionState_.referencePoint - interactionState_.basePoint;
        radius = std::hypot(radial.x(), radial.y());
    }
    if (radius <= 1.0e-12) {
        return false;
    }

    const qreal angle = interactionState_.referenceAngle +
                        interactionState_.previewAngle;
    *cursorPoint = interactionState_.basePoint +
                   QPointF(radius * std::cos(angle), radius * std::sin(angle));
    return true;
}

RotateTool::InteractionState &RotateTool::interactionState()
{
    return interactionState_;
}

const RotateTool::InteractionState &RotateTool::interactionState() const
{
    return interactionState_;
}

} // namespace classiCAD
