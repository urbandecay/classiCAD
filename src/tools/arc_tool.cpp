#include "arc_tool.h"

#include "core/document/document_settings.h"
#include "core/geometry/arc_curve_factory.h"
#include "services/input/input_constraint_service.h"
#include "services/viewport/viewport_transform.h"
#include "tool_context.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

qreal vectorDot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

qreal vectorLength(const Point3D &vector)
{
    return std::sqrt(vectorDot(vector, vector));
}

Point3D vectorAdd(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D vectorSubtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D vectorScale(const Point3D &vector, qreal scale)
{
    return {vector.x * scale, vector.y * scale, vector.z * scale};
}

Point3D vectorCross(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

bool makeOnePointArcFrameThroughRadius(const Point3D &center,
                                       const Point3D &radiusPoint,
                                       const Point3D &referenceNormal,
                                       const WorkPlaneFrame &referenceFrame,
                                       WorkPlaneFrame *frame)
{
    if (frame == nullptr) {
        return false;
    }
    Point3D yAxis = referenceNormal;
    const qreal normalLength = vectorLength(yAxis);
    const Point3D bridge = vectorSubtract(radiusPoint, center);
    const qreal bridgeLength = vectorLength(bridge);
    if (normalLength <= 1.0e-9 || bridgeLength <= 1.0e-9) {
        return false;
    }
    yAxis = vectorScale(yAxis, 1.0 / normalLength);
    const Point3D bridgeDirection = vectorScale(bridge, 1.0 / bridgeLength);

    Point3D planeNormal = vectorCross(bridgeDirection, yAxis);
    qreal planeNormalLength = vectorLength(planeNormal);
    Point3D xAxis;
    if (planeNormalLength > 1.0e-9) {
        planeNormal = vectorScale(planeNormal, 1.0 / planeNormalLength);
        xAxis = vectorCross(yAxis, planeNormal);
        const qreal xLength = vectorLength(xAxis);
        if (xLength <= 1.0e-9) {
            return false;
        }
        xAxis = vectorScale(xAxis, 1.0 / xLength);
    } else {
        // If the snapped radius direction is parallel to the reference
        // normal, any perpendicular axis completes the plane. Reuse the
        // captured X axis so the choice remains stable.
        xAxis = vectorSubtract(
            referenceFrame.xAxis,
            vectorScale(yAxis, vectorDot(referenceFrame.xAxis, yAxis)));
        const qreal xLength = vectorLength(xAxis);
        if (xLength <= 1.0e-9) {
            return false;
        }
        xAxis = vectorScale(xAxis, 1.0 / xLength);
        yAxis = bridgeDirection;
        planeNormal = vectorCross(xAxis, yAxis);
        planeNormalLength = vectorLength(planeNormal);
        if (planeNormalLength <= 1.0e-9) {
            return false;
        }
        planeNormal = vectorScale(planeNormal, 1.0 / planeNormalLength);
    }

    *frame = {};
    frame->origin = center;
    frame->xAxis = xAxis;
    frame->yAxis = yAxis;
    frame->normal = planeNormal;
    frame->valid = true;
    return isValidWorkPlaneFrame(*frame);
}

} // namespace

ArcTool::ArcTool()
    : ShapeCreationTool(ToolId::Arc, 3)
{
}

ArcTool::InteractionState &ArcTool::interactionState()
{
    return interactionState_;
}

const ArcTool::InteractionState &ArcTool::interactionState() const
{
    return interactionState_;
}

ArcMode ArcTool::mode() const
{
    return interactionState_.mode;
}

void ArcTool::setMode(ArcMode mode)
{
    interactionState_.mode = mode;
}

const QVector<QPointF> &ArcTool::inputPoints() const
{
    return interactionState_.inputPoints;
}

ArcInputStage ArcTool::inputStage() const
{
    const int pointCount = interactionState_.inputPoints.size();
    if (pointCount == 0) {
        return ArcInputStage::FirstPoint;
    }
    if (pointCount == 1) {
        return ArcInputStage::SecondPoint;
    }
    return ArcInputStage::Complete;
}

void ArcTool::setInputPoints(const QVector<QPointF> &points)
{
    interactionState_.inputPoints = points;
}

void ArcTool::appendInputPoint(const QPointF &point)
{
    interactionState_.inputPoints.append(point);
}

bool ArcTool::setInputPoint(int index, const QPointF &point)
{
    if (index < 0 || index >= interactionState_.inputPoints.size()) {
        return false;
    }
    interactionState_.inputPoints[index] = point;
    return true;
}

void ArcTool::clearInputPoints()
{
    interactionState_.inputPoints.clear();
}

void ArcTool::resetInputState()
{
    InteractionState &state = interactionState_;
    state.inputPoints.clear();
    state.referenceFrame = {};
    state.inputFrame = {};
    state.axisBaseFrame = {};
    state.referenceNormal = {};
    state.firstPointWorld = {};
    state.secondPointWorld = {};
    state.resolvedChordPointWorld = {};
    state.referenceFrameValid = false;
    state.inputFrameValid = false;
    state.axisBaseFrameValid = false;
    state.chordWorldPointsValid = false;
    state.resolvedChordPointValid = false;
    state.axisConstraintKey = 0;
    state.planeNormalLockKey = 0;
    state.verticalOverrideAxis = 0;
    state.twoPointPerpendicularNormal = {};
    state.twoPointPerpendicularNormalValid = false;
    state.wasVertical = false;
    state.previewStartAngle = 0.0;
    state.angleSnapEnabled = true;
    state.perpendicularPlaneActive = false;
    state.planeLocked = false;
    state.lockedFrame = {};
    state.lockedFrameValid = false;
    state.textInputMode = ArcTextInputMode::None;
    state.textInput.clear();
}

bool ArcTool::captureReferenceForFirstPoint(const QPointF &localPoint,
                                            const WorkPlaneFrame &frame)
{
    interactionState_.inputPoints = {localPoint};
    interactionState_.referenceFrame = frame;
    interactionState_.inputFrame = frame;
    interactionState_.referenceFrameValid = isValidWorkPlaneFrame(frame);
    interactionState_.inputFrameValid = interactionState_.referenceFrameValid;
    if (!interactionState_.referenceFrameValid) {
        return false;
    }

    interactionState_.referenceNormal = frame.normal;
    interactionState_.twoPointPerpendicularNormal = {};
    interactionState_.twoPointPerpendicularNormalValid = false;
    interactionState_.firstPointWorld =
        workPlaneFramePointToWorld(localPoint, frame);
    interactionState_.chordWorldPointsValid = false;
    interactionState_.resolvedChordPointValid = false;
    interactionState_.wasVertical = false;
    return true;
}

bool ArcTool::beginTextInput(ArcTextInputMode mode,
                             const QString &initialText,
                             bool hasPendingPoints)
{
    if (!hasPendingPoints || mode == ArcTextInputMode::None) {
        return false;
    }
    interactionState_.textInputMode = mode;
    interactionState_.textInput = initialText;
    return true;
}

void ArcTool::clearTextInput()
{
    interactionState_.textInputMode = ArcTextInputMode::None;
    interactionState_.textInput.clear();
}

void ArcTool::appendTextInput(const QString &text)
{
    interactionState_.textInput.append(
        text == QStringLiteral(",") ? QStringLiteral(".") : text);
}

void ArcTool::backspaceTextInput()
{
    interactionState_.textInput.chop(1);
}

ArcTextInputMode ArcTool::textInputMode() const
{
    return interactionState_.textInputMode;
}

const QString &ArcTool::textInput() const
{
    return interactionState_.textInput;
}

ArcKeyResult ArcTool::handleKeyInput(int key,
                                    const QString &text,
                                    Qt::KeyboardModifiers modifiers,
                                    bool autoRepeat)
{
    const int pendingPointCount = interactionState_.inputPoints.size();
    const auto result = [](ArcKeyAction action) {
        return ArcKeyResult{action, true};
    };
    const bool onePointMode = mode() == ArcMode::OnePoint;
    const bool twoPointMode = mode() == ArcMode::TwoPoint;
    const bool threePointMode = mode() == ArcMode::ThreePoint;

    if (key == Qt::Key_Escape) {
        resetInputState();
        resetPreviewTracking();
        return result(ArcKeyAction::CancelArc);
    }

    if (textInputMode() != ArcTextInputMode::None) {
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            return result(ArcKeyAction::ApplyTextInput);
        }
        if (key == Qt::Key_Backspace) {
            backspaceTextInput();
            return result(ArcKeyAction::ChangeTextInput);
        }

        bool accepted = !text.isEmpty();
        for (const QChar character : text) {
            const bool numeric = character.isDigit() ||
                                 character == QLatin1Char('.') ||
                                 character == QLatin1Char(',') ||
                                 character == QLatin1Char('-') ||
                                 character == QLatin1Char('+') ||
                                 character == QLatin1Char('/') ||
                                 character == QLatin1Char(' ') ||
                                 character == QLatin1Char('\'') ||
                                 character == QLatin1Char('"') ||
                                 character.isLetter();
            if (!numeric) {
                accepted = false;
                break;
            }
        }
        if (accepted) {
            appendTextInput(text);
            return result(ArcKeyAction::ChangeTextInput);
        }
        return {};
    }

    if (autoRepeat || modifiers != Qt::NoModifier) {
        return {};
    }

    if (onePointMode) {
        switch (key) {
        case Qt::Key_C:
            toggleAngleSnap();
            return result(ArcKeyAction::ToggleAngleSnap);
        case Qt::Key_L:
            return result(ArcKeyAction::TogglePlaneLock);
        case Qt::Key_P:
            return result(ArcKeyAction::TogglePerpendicularPlane);
        case Qt::Key_R:
            beginTextInput(ArcTextInputMode::Radius, {}, pendingPointCount > 0);
            return result(ArcKeyAction::BeginTextInput);
        case Qt::Key_A:
            if (pendingPointCount >= 2) {
                beginTextInput(ArcTextInputMode::Angle, {}, true);
                return result(ArcKeyAction::BeginTextInput);
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            if (pendingPointCount >= 2) {
                return result(ArcKeyAction::FinishArc);
            }
            break;
        default:
            break;
        }
    } else if (twoPointMode) {
        switch (key) {
        case Qt::Key_D:
            if (pendingPointCount == 1) {
                beginTextInput(ArcTextInputMode::ChordLength, {}, true);
                return result(ArcKeyAction::BeginTextInput);
            }
            break;
        case Qt::Key_H:
            if (pendingPointCount >= 2) {
                beginTextInput(ArcTextInputMode::Sagitta, {}, true);
                return result(ArcKeyAction::BeginTextInput);
            }
            break;
        case Qt::Key_P:
        case Qt::Key_L:
            return result(key == Qt::Key_P
                              ? ArcKeyAction::TogglePerpendicularPlane
                              : ArcKeyAction::TogglePlaneLock);
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            if (pendingPointCount >= 2) {
                return result(ArcKeyAction::FinishArc);
            }
            break;
        default:
            break;
        }
    } else if (threePointMode) {
        switch (key) {
        case Qt::Key_P:
            return result(ArcKeyAction::TogglePerpendicularPlane);
        case Qt::Key_L:
            return result(ArcKeyAction::TogglePlaneLock);
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            if (pendingPointCount >= 2) {
                return result(ArcKeyAction::FinishArc);
            }
            break;
        default:
            break;
        }
    }

    if ((onePointMode || twoPointMode) && pendingPointCount > 0 &&
        text.size() == 1 &&
        (text.front().isDigit() || text.front() == QLatin1Char('.') ||
         text.front() == QLatin1Char('-') ||
         text.front() == QLatin1Char('+'))) {
        const ArcTextInputMode inputMode = onePointMode
            ? (pendingPointCount >= 2 ? ArcTextInputMode::Angle
                                      : ArcTextInputMode::Radius)
            : (pendingPointCount >= 2 ? ArcTextInputMode::Sagitta
                                      : ArcTextInputMode::ChordLength);
        beginTextInput(inputMode, text, true);
        return result(ArcKeyAction::BeginTextInput);
    }

    return {};
}

ArcTextInputUpdate ArcTool::applyTextInput(
    const DocumentSettings &settings,
    const QPointF &cursorPoint,
    bool geometrySnapActive)
{
    ArcTextInputUpdate update;
    const QVector<QPointF> &points = interactionState_.inputPoints;
    update.inputMode = textInputMode();
    const QString input = textInput().trimmed();
    clearTextInput();

    if (mode() == ArcMode::TwoPoint &&
        (update.inputMode == ArcTextInputMode::ChordLength ||
         update.inputMode == ArcTextInputMode::Sagitta)) {
        qreal distance = 0.0;
        if (!parseDocumentLengthInput(input, settings.lengthUnit, &distance)) {
            return update;
        }
        distance = std::abs(distance);
        if (update.inputMode == ArcTextInputMode::ChordLength &&
            points.size() == 1) {
            update.hasLengthValue = true;
            update.lengthValue = distance;
        } else if (update.inputMode == ArcTextInputMode::Sagitta &&
                   points.size() >= 2) {
            update.updateCursor = applySagittaInput(distance, points,
                                                    cursorPoint,
                                                    &update.cursorPoint);
            update.updateRawCursor = update.updateCursor;
            update.clearGeometrySnap = update.updateCursor;
        }
        return update;
    }

    if (mode() == ArcMode::OnePoint &&
        update.inputMode == ArcTextInputMode::Radius) {
        qreal radius = 0.0;
        if (!parseDocumentLengthInput(input, settings.lengthUnit, &radius)) {
            return update;
        }
        const RadiusInputUpdate radiusUpdate = applyRadiusInput(
            radius, points, cursorPoint, geometrySnapActive);
        if (radiusUpdate.accepted && !radiusUpdate.appendStartPoint &&
            points.size() >= 2) {
            setInputPoint(1, radiusUpdate.startPoint);
        }
        if (radiusUpdate.updateCursor) {
            update.updateCursor = true;
            update.cursorPoint = radiusUpdate.cursorPoint;
        }
        return update;
    }

    if (mode() == ArcMode::OnePoint &&
        update.inputMode == ArcTextInputMode::Angle && points.size() >= 2) {
        QString angleInput = input;
        angleInput.remove(QStringLiteral("°"));
        bool valid = false;
        const qreal degrees = angleInput.toDouble(&valid);
        if (valid && applyAngleInput(degrees, points, &update.cursorPoint)) {
            update.updateCursor = true;
            update.clearGeometrySnap = true;
        }
    }
    return update;
}

ArcCommitResult ArcTool::commitAt(const QPointF &cursorPoint,
                                  bool geometrySnapActive,
                                  ToolContext &context)
{
    ArcCommitResult result;
    result.mode = mode();
    const QVector<QPointF> &inputPoints = interactionState_.inputPoints;
    if (inputPoints.size() < 2 ||
        !isValidWorkPlaneFrame(context.viewportTransform().workPlaneFrame())) {
        return result;
    }

    QVector<QPointF> points{inputPoints[0], inputPoints[1]};
    qreal sweep = 0.0;
    if (mode() == ArcMode::OnePoint) {
        updatePreviewTracking(cursorPoint, geometrySnapActive);
        constexpr qreal twoPi = 6.28318530717958647692;
        sweep = std::clamp(interactionState_.previewSweepAngle,
                           -twoPi + 1.0e-6,
                           twoPi - 1.0e-6);
        if (std::abs(sweep) <= 1.0e-12) {
            return result;
        }
        const QPointF radiusVector = points[1] - points[0];
        result.radius = std::hypot(radiusVector.x(), radiusVector.y());
        if (result.radius <= 1.0e-9) {
            return result;
        }
        const qreal startAngle = std::atan2(radiusVector.y(), radiusVector.x());
        points.append(points[0] + QPointF(
            result.radius * std::cos(startAngle + sweep),
            result.radius * std::sin(startAngle + sweep)));
        result.sweep = sweep;
    } else if (mode() == ArcMode::TwoPoint) {
        points.append(constrainTwoPointThroughPoint(cursorPoint,
                                                     geometrySnapActive,
                                                     false));
        const QPointF chord = points[1] - points[0];
        result.chordLength = std::hypot(chord.x(), chord.y());
        const QPointF midpoint = (points[0] + points[1]) * 0.5;
        const QPointF sagittaVector = points[2] - midpoint;
        result.sagitta = std::hypot(sagittaVector.x(), sagittaVector.y());
    } else if (mode() == ArcMode::ThreePoint) {
        points.append(cursorPoint);
        CircularArc2D arc;
        if (makeCircularArcThroughPoint(points[0], points[1], points[2], &arc)) {
            result.radius = arc.radius;
        }
    } else {
        return result;
    }

    Shape shape;
    if (!context.createShape(id(), points, mode(), sweep, &shape) ||
        !context.commitShape(id(), shape)) {
        return result;
    }
    result.committed = true;
    context.finishTool(ToolId::Select);
    return result;
}

ArcClickResult ArcTool::handleClick(const ToolInput &input,
                                    bool worldPositionValid,
                                    ToolContext &context)
{
    ArcClickResult result;
    if (input.button == Qt::RightButton) {
        result.handled = true;
        if (worldPositionValid && inputStage() == ArcInputStage::Complete) {
            result.commit = commitAt(input.worldPosition,
                                     input.snapResult.isValid(), context);
        }
        if (!result.commit.committed) {
            result.cancelled = true;
            context.finishTool(ToolId::Select);
        }
        return result;
    }
    if (input.button != Qt::LeftButton || !worldPositionValid) {
        return result;
    }

    result.handled = true;
    const ArcInputStage stage = inputStage();
    if (stage == ArcInputStage::FirstPoint) {
        WorkPlaneFrame frame = input.workPlaneFrame;
        if (!isValidWorkPlaneFrame(frame)) {
            frame = context.viewportTransform().workPlaneFrame();
        }
        Point3D firstWorld = input.resolvedWorldPoint();
        if (input.snapResult.isValid() && input.snapResult.hasWorldPoint &&
            isValidWorkPlaneFrame(frame)) {
            // Spatial snapping can place the center off the current drawing
            // plane. Move that plane along its normal so the center remains
            // exactly on the snapped world point while keeping the captured
            // orientation unchanged.
            const Point3D fromPlane = vectorSubtract(firstWorld, frame.origin);
            const qreal normalOffset = vectorDot(fromPlane, frame.normal);
            frame.origin = vectorAdd(frame.origin,
                                     vectorScale(frame.normal, normalOffset));
            context.viewportTransform().setWorkPlaneFrame(frame);
        }
        const QPointF firstPoint = worldPointToWorkPlaneFrame(firstWorld, frame);
        captureReferenceForFirstPoint(firstPoint, frame);
        return result;
    }

    if (stage == ArcInputStage::SecondPoint) {
        WorkPlaneFrame frame = isValidWorkPlaneFrame(interactionState_.inputFrame)
                                   ? interactionState_.inputFrame
                                   : input.workPlaneFrame;
        if (mode() == ArcMode::OnePoint) {
            const Point3D radiusWorld = input.resolvedWorldPoint();
            if (input.snapResult.isValid() && input.snapResult.hasWorldPoint &&
                isValidWorkPlaneFrame(frame)) {
                const qreal planeOffset = vectorDot(
                    vectorSubtract(radiusWorld, frame.origin), frame.normal);
                if (std::abs(planeOffset) > 1.0e-8) {
                    WorkPlaneFrame radiusFrame;
                    if (makeOnePointArcFrameThroughRadius(
                            interactionState_.firstPointWorld,
                            radiusWorld,
                            interactionState_.referenceNormal,
                            interactionState_.referenceFrame,
                            &radiusFrame)) {
                        frame = radiusFrame;
                        context.viewportTransform().setWorkPlaneFrame(frame);
                        interactionState_.inputFrame = frame;
                        interactionState_.inputFrameValid = true;
                        setInputPoint(
                            0,
                            worldPointToWorkPlaneFrame(
                                interactionState_.firstPointWorld, frame));
                    }
                }
            }
            const QPointF localPoint = worldPointToWorkPlaneFrame(radiusWorld,
                                                                  frame);
            appendInputPoint(localPoint);
            initializePreviewTracking();
        } else {
            const QPointF localPoint = input.positionInFrame(frame);
            interactionState_.secondPointWorld =
                interactionState_.resolvedChordPointValid
                    ? interactionState_.resolvedChordPointWorld
                    : input.resolvedWorldPoint();
            interactionState_.chordWorldPointsValid =
                interactionState_.referenceFrameValid;
            appendInputPoint(localPoint);
            interactionState_.axisConstraintKey = 0;
            interactionState_.resolvedChordPointValid = false;
            result.chordEndpointCaptured = true;
        }
        return result;
    }

    result.commit = commitAt(input.worldPosition,
                             input.snapResult.isValid(), context);
    return result;
}

ArcEndpointConstraintResult ArcTool::constrainChordEndpoint(
    const ToolInput &input,
    ToolContext &context)
{
    ArcEndpointConstraintResult result;
    result.point = input.rawWorldPosition;
    result.snapResult = input.snapResult;

    const WorkPlaneFrame oldFrame = context.viewportTransform().workPlaneFrame();
    if (!isValidWorkPlaneFrame(oldFrame) ||
        !interactionState_.referenceFrameValid) {
        interactionState_.resolvedChordPointValid = false;
        return result;
    }

    const Point3D rawWorld = workPlaneFramePointToWorld(
        input.rawWorldPosition, oldFrame);
    Point3D targetWorld = input.snapResult.isValid()
                              ? (input.snapResult.hasWorldPoint
                                     ? input.snapResult.worldPoint
                                     : workPlaneFramePointToWorld(
                                           input.snapResult.point, oldFrame))
                              : rawWorld;
    Point3D axisDirection;
    if (!input.modifiers.testFlag(Qt::AltModifier)) {
        if (interactionState_.axisConstraintKey != 0) {
            axisDirection = InputConstraintService::worldAxisDirection(
                interactionState_.axisConstraintKey);
        } else if (!input.snapResult.isValid()) {
            axisDirection = InputConstraintService::inferProjectedWorldAxis(
                interactionState_.firstPointWorld,
                input.screenPosition,
                context.viewportTransform(),
                input.viewportSize);
        }
    }

    if (vectorLength(axisDirection) > 1.0e-9) {
        if (input.snapResult.isValid()) {
            targetWorld = InputConstraintService::projectOntoWorldAxis(
                targetWorld, interactionState_.firstPointWorld, axisDirection);
        } else {
            Point3D axisPoint;
            if (InputConstraintService::worldPointOnScreenAxis(
                    input.screenPosition,
                    input.viewportSize,
                    interactionState_.firstPointWorld,
                    axisDirection,
                    context.viewportTransform(),
                    &axisPoint)) {
                targetWorld = axisPoint;
            }
        }
    }

    interactionState_.resolvedChordPointWorld = targetWorld;
    interactionState_.resolvedChordPointValid = true;
    if (mode() != ArcMode::TwoPoint || interactionState_.chordWorldPointsValid) {
        updateChordWorkPlane(context, &targetWorld);
    }

    const WorkPlaneFrame newFrame = context.viewportTransform().workPlaneFrame();
    result.point = worldPointToWorkPlaneFrame(targetWorld, newFrame);
    if (result.snapResult.isValid()) {
        result.snapResult.point = result.point;
    }

    if (!workPlaneFramesMatch(oldFrame, newFrame)) {
        if (!context.viewportTransform().screenToWorkPlane(
                input.screenPosition,
                input.viewportSize,
                newFrame,
                &result.rawCursorPoint)) {
            result.rawCursorPoint = worldPointToWorkPlaneFrame(rawWorld, newFrame);
        }
        result.updateRawCursor = true;
    }
    return result;
}

ArcChordLengthResult ArcTool::applyChordLength(qreal chordLength,
                                              const ToolInput &input,
                                              ToolContext &context)
{
    ArcChordLengthResult result;
    if (inputStage() != ArcInputStage::SecondPoint) {
        return result;
    }

    const WorkPlaneFrame frame = context.viewportTransform().workPlaneFrame();
    if (!isValidWorkPlaneFrame(frame)) {
        return result;
    }

    const Point3D cursorWorldPoint = workPlaneFramePointToWorld(
        input.worldPosition, frame);
    const bool axisConstraintActive = interactionState_.axisConstraintKey != 0;
    const Point3D axisDirection = axisConstraintActive
                                      ? InputConstraintService::worldAxisDirection(
                                            interactionState_.axisConstraintKey)
                                      : Point3D{};
    Point3D endpointWorld;
    if (!resolveChordLengthEndpoint(chordLength,
                                    cursorWorldPoint,
                                    axisConstraintActive,
                                    axisDirection,
                                    &endpointWorld)) {
        return result;
    }

    updateChordWorkPlane(context, &interactionState_.secondPointWorld);
    const WorkPlaneFrame updatedFrame = context.viewportTransform().workPlaneFrame();
    setInputPoint(0, worldPointToWorkPlaneFrame(
                         interactionState_.firstPointWorld, updatedFrame));
    appendInputPoint(worldPointToWorkPlaneFrame(
        interactionState_.secondPointWorld, updatedFrame));

    result.applied = true;
    result.cursorPoint = interactionState_.inputPoints[1];
    return result;
}

bool ArcTool::restoreChordReferenceFrame(ToolContext &context)
{
    InteractionState &state = interactionState_;
    if (!state.referenceFrameValid) {
        return false;
    }

    context.viewportTransform().setWorkPlaneFrame(state.referenceFrame);
    state.inputFrame = state.referenceFrame;
    state.inputFrameValid = true;
    if (!state.inputPoints.isEmpty()) {
        setInputPoint(0, worldPointToWorkPlaneFrame(state.firstPointWorld,
                                                    state.referenceFrame));
    }
    return true;
}

bool ArcTool::updateChordWorkPlane(
    ToolContext &context,
    const Point3D *previewEndpointWorld)
{
    InteractionState &state = interactionState_;
    if ((mode() != ArcMode::TwoPoint && mode() != ArcMode::ThreePoint) ||
        !state.referenceFrameValid) {
        return false;
    }

    const bool haveEndpoint = previewEndpointWorld != nullptr ||
                              state.chordWorldPointsValid;
    if (!haveEndpoint) {
        return false;
    }
    // In the add-on, P changes the chord-picking plane immediately. Keep that
    // selected plane fixed while the second endpoint moves, then derive the
    // final arc frame after the chord has been accepted.
    if (mode() == ArcMode::TwoPoint && state.inputPoints.size() == 1 &&
        !state.chordWorldPointsValid) {
        return false;
    }

    const Point3D endpoint = previewEndpointWorld != nullptr
                                 ? *previewEndpointWorld
                                 : state.secondPointWorld;
    const Point3D chord = vectorSubtract(endpoint, state.firstPointWorld);
    const qreal chordLength = vectorLength(chord);
    if (!std::isfinite(chordLength) || chordLength <= 1.0e-9) {
        return false;
    }

    const Point3D chordDirection = vectorScale(chord, 1.0 / chordLength);
    const qreal verticalThreshold = state.wasVertical ? 0.98 : 0.995;
    const bool isVertical = std::abs(vectorDot(chordDirection,
                                               state.referenceNormal)) >
                            verticalThreshold;
    state.wasVertical = isVertical;

    const Point3D viewForward = context.viewportTransform().viewDirection();
    const auto verticalPlaneNormal = [&state, &viewForward]() {
        Point3D referenceNormal = state.referenceNormal;
        const qreal normalLength = vectorLength(referenceNormal);
        if (normalLength <= 1.0e-9) {
            return Point3D{};
        }
        referenceNormal = vectorScale(referenceNormal, 1.0 / normalLength);
        const Point3D worldX{1.0, 0.0, 0.0};
        const Point3D worldY{0.0, 1.0, 0.0};
        const Point3D referenceAxis =
            std::abs(vectorDot(referenceNormal, worldX)) < 0.99
                ? worldX
                : worldY;
        Point3D basisY = vectorCross(referenceNormal, referenceAxis);
        const qreal basisYLength = vectorLength(basisY);
        if (basisYLength <= 1.0e-9) {
            return Point3D{};
        }
        basisY = vectorScale(basisY, 1.0 / basisYLength);
        Point3D basisX = vectorCross(basisY, referenceNormal);
        const qreal basisXLength = vectorLength(basisX);
        if (basisXLength <= 1.0e-9) {
            return Point3D{};
        }
        basisX = vectorScale(basisX, 1.0 / basisXLength);
        if (state.verticalOverrideAxis == Qt::Key_X) {
            return basisX;
        }
        if (state.verticalOverrideAxis == Qt::Key_Y) {
            return basisY;
        }
        return std::abs(vectorDot(viewForward, basisX)) >
                       std::abs(vectorDot(viewForward, basisY))
                   ? basisX
                   : basisY;
    };

    WorkPlaneFrame frame = state.referenceFrame;
    if (mode() == ArcMode::TwoPoint) {
        Point3D planeNormal;
        if (isVertical) {
            planeNormal = verticalPlaneNormal();
        } else if (state.perpendicularPlaneActive) {
            planeNormal = state.twoPointPerpendicularNormalValid
                              ? state.twoPointPerpendicularNormal
                              : vectorCross(chordDirection, state.referenceNormal);
            if (vectorDot(planeNormal, viewForward) > 0.0) {
                planeNormal = vectorScale(planeNormal, -1.0);
            }
        } else {
            planeNormal = state.referenceNormal;
        }

        // Keep the selected normal as close as possible to its intended
        // direction while making the final plane contain the chord.
        planeNormal = vectorSubtract(
            planeNormal,
            vectorScale(chordDirection, vectorDot(planeNormal, chordDirection)));
        const qreal planeNormalLength = vectorLength(planeNormal);
        if (planeNormalLength > 1.0e-9) {
            planeNormal = vectorScale(planeNormal, 1.0 / planeNormalLength);
            Point3D arcY = vectorCross(planeNormal, chordDirection);
            const qreal arcYLength = vectorLength(arcY);
            if (arcYLength > 1.0e-9) {
                arcY = vectorScale(arcY, 1.0 / arcYLength);
                frame.origin = vectorScale(
                    vectorAdd(state.firstPointWorld, endpoint), 0.5);
                frame.xAxis = chordDirection;
                frame.yAxis = arcY;
                frame.normal = vectorCross(chordDirection, arcY);
                const qreal frameNormalLength = vectorLength(frame.normal);
                if (frameNormalLength > 1.0e-9) {
                    frame.normal = vectorScale(frame.normal,
                                              1.0 / frameNormalLength);
                    frame.valid = true;
                }
            }
        }
    } else if (isVertical) {
        const Point3D midpoint = vectorScale(
            vectorAdd(state.firstPointWorld, endpoint), 0.5);
        frame = makeWorkPlaneFrameFromNormal(midpoint, verticalPlaneNormal());
    } else if (state.perpendicularPlaneActive) {
        const Point3D midpoint = vectorScale(
            vectorAdd(state.firstPointWorld, endpoint), 0.5);
        Point3D normal = vectorCross(chordDirection, state.referenceNormal);
        const qreal normalLength = vectorLength(normal);
        if (normalLength > 1.0e-9) {
            normal = vectorScale(normal, 1.0 / normalLength);
            frame = makeWorkPlaneFrameFromNormal(midpoint, normal);
        }
    }
    if (!isValidWorkPlaneFrame(frame)) {
        return false;
    }

    context.viewportTransform().setWorkPlaneFrame(frame);
    state.inputFrame = frame;
    state.inputFrameValid = true;
    if (!state.inputPoints.isEmpty()) {
        setInputPoint(0, worldPointToWorkPlaneFrame(state.firstPointWorld, frame));
    }
    if (state.inputPoints.size() >= 2 && state.chordWorldPointsValid) {
        setInputPoint(1, worldPointToWorkPlaneFrame(state.secondPointWorld, frame));
    }
    return true;
}

ArcAxisKeyResult ArcTool::handleAxisKey(int key,
                                        const ToolInput &input,
                                        ToolContext &context)
{
    ArcAxisKeyResult result;
    if (key != Qt::Key_X && key != Qt::Key_Y && key != Qt::Key_Z) {
        return result;
    }

    result.handled = true;
    InteractionState &state = interactionState_;
    ViewportTransform &transform = context.viewportTransform();
    if (mode() == ArcMode::OnePoint) {
        if (inputStage() != ArcInputStage::FirstPoint) {
            return result;
        }

        const WorkPlaneFrame currentFrame = transform.workPlaneFrame();
        if (state.planeNormalLockKey == key && state.axisBaseFrameValid) {
            transform.setWorkPlaneFrame(state.axisBaseFrame);
            state.planeNormalLockKey = 0;
            state.axisBaseFrame = {};
            state.axisBaseFrameValid = false;
            result.drawingFrame = {};
            result.drawingPlaneLocked = false;
            result.drawingFrameChanged = true;
        } else {
            if (!state.axisBaseFrameValid) {
                state.axisBaseFrame = currentFrame;
                state.axisBaseFrameValid = isValidWorkPlaneFrame(currentFrame);
            }
            QPointF cursorLocal;
            Point3D planeOrigin = currentFrame.origin;
            if (transform.screenToWorkPlane(input.screenPosition,
                                            input.viewportSize,
                                            currentFrame,
                                            &cursorLocal)) {
                planeOrigin = workPlaneFramePointToWorld(cursorLocal,
                                                         currentFrame);
            }
            const Point3D axisDirection = key == Qt::Key_X
                                             ? Point3D{1.0, 0.0, 0.0}
                                             : key == Qt::Key_Y
                                                   ? Point3D{0.0, 1.0, 0.0}
                                                   : Point3D{0.0, 0.0, 1.0};
            const WorkPlaneFrame lockedFrame = makeWorkPlaneFrameFromNormal(
                planeOrigin, axisDirection);
            if (isValidWorkPlaneFrame(lockedFrame)) {
                transform.setWorkPlaneFrame(lockedFrame);
                state.planeNormalLockKey = key;
                result.drawingFrame = lockedFrame;
                result.drawingPlaneLocked = true;
                result.drawingFrameChanged = true;
            }
        }
        result.refreshCursor = true;
        return result;
    }

    if (inputStage() == ArcInputStage::FirstPoint) {
        state.axisConstraintKey = state.axisConstraintKey == key ? 0 : key;
        return result;
    }

    if (inputStage() == ArcInputStage::SecondPoint) {
        state.axisConstraintKey = state.axisConstraintKey == key ? 0 : key;
        if (!(mode() == ArcMode::TwoPoint && state.perpendicularPlaneActive)) {
            restoreChordReferenceFrame(context);
        }
        result.refreshCursor = true;
        return result;
    }

    if (state.chordWorldPointsValid && key != Qt::Key_Z) {
        const Point3D chord = vectorSubtract(state.secondPointWorld,
                                             state.firstPointWorld);
        const qreal chordLength = vectorLength(chord);
        if (chordLength > 1.0e-9 &&
            std::abs(vectorDot(vectorScale(chord, 1.0 / chordLength),
                               state.referenceNormal)) > 0.99) {
            state.verticalOverrideAxis = key;
            updateChordWorkPlane(context);
            result.refreshCursor = true;
        }
    }
    return result;
}

ArcPlaneToggleResult ArcTool::togglePerpendicularPlane(
    const QPointF &cursorPoint,
    ToolContext &context)
{
    ArcPlaneToggleResult result;
    InteractionState &state = interactionState_;
    ViewportTransform &transform = context.viewportTransform();

    if (mode() == ArcMode::OnePoint) {
        if (state.inputPoints.isEmpty() || !state.referenceFrameValid ||
            !state.inputFrameValid) {
            return result;
        }

        const WorkPlaneFrame oldFrame = transform.workPlaneFrame();
        const Point3D centerWorld = state.firstPointWorld;
        Point3D startWorld = centerWorld;
        if (state.inputPoints.size() >= 2) {
            startWorld = workPlaneFramePointToWorld(state.inputPoints[1], oldFrame);
        }

        WorkPlaneFrame nextFrame;
        if (state.perpendicularPlaneActive) {
            nextFrame = state.referenceFrame;
        } else {
            const QPointF bridgeLocal = state.inputPoints.size() >= 2
                                            ? state.inputPoints[1] - state.inputPoints[0]
                                            : cursorPoint - state.inputPoints[0];
            const Point3D bridgeWorld = vectorSubtract(
                workPlaneFramePointToWorld(state.inputPoints[0] + bridgeLocal,
                                           oldFrame),
                centerWorld);
            const qreal bridgeLength = vectorLength(bridgeWorld);
            if (bridgeLength <= 1.0e-9) {
                return result;
            }
            const Point3D bridgeDirection =
                vectorScale(bridgeWorld, 1.0 / bridgeLength);
            const Point3D normal = vectorCross(bridgeDirection,
                                                state.referenceNormal);
            const qreal normalLength = vectorLength(normal);
            if (normalLength <= 1.0e-9) {
                return result;
            }
            const Point3D perpendicularNormal =
                vectorScale(normal, 1.0 / normalLength);
            const Point3D xAxis = vectorCross(state.referenceNormal,
                                              perpendicularNormal);
            const qreal xLength = vectorLength(xAxis);
            if (xLength <= 1.0e-9) {
                return result;
            }
            nextFrame.origin = centerWorld;
            nextFrame.xAxis = vectorScale(xAxis, 1.0 / xLength);
            nextFrame.yAxis = state.referenceNormal;
            nextFrame.normal = perpendicularNormal;
            nextFrame.valid = true;
        }

        if (!isValidWorkPlaneFrame(nextFrame)) {
            return result;
        }
        const qreal previousSweep = state.previewSweepAngle;
        transform.setWorkPlaneFrame(nextFrame);
        state.inputFrame = nextFrame;
        state.inputFrameValid = true;
        setInputPoint(0, worldPointToWorkPlaneFrame(centerWorld, nextFrame));
        if (state.inputPoints.size() >= 2) {
            QPointF startLocal = worldPointToWorkPlaneFrame(startWorld, nextFrame);
            const QPointF centerLocal = state.inputPoints[0];
            const qreal startRadius = std::hypot(startLocal.x() - centerLocal.x(),
                                                 startLocal.y() - centerLocal.y());
            if (startRadius <= 1.0e-9) {
                const QPointF oldRadius = state.inputPoints[1] - state.inputPoints[0];
                startLocal = centerLocal + QPointF(
                    std::hypot(oldRadius.x(), oldRadius.y()), 0.0);
            }
            setInputPoint(1, startLocal);
        }
        state.perpendicularPlaneActive = !state.perpendicularPlaneActive;
        state.previewSweepAngle = previousSweep;
        result.changed = true;
        result.refreshCursor = true;
        result.forceCursorRefresh = true;
        result.rebaseOnePointPreview = state.inputPoints.size() >= 2;
        return result;
    }

    if (mode() == ArcMode::TwoPoint &&
        inputStage() == ArcInputStage::SecondPoint &&
        state.referenceFrameValid) {
        if (state.perpendicularPlaneActive) {
            state.perpendicularPlaneActive = false;
            state.twoPointPerpendicularNormal = {};
            state.twoPointPerpendicularNormalValid = false;
            state.verticalOverrideAxis = 0;
            restoreChordReferenceFrame(context);
        } else {
            const Point3D endpoint = state.resolvedChordPointValid
                                         ? state.resolvedChordPointWorld
                                         : workPlaneFramePointToWorld(
                                               cursorPoint,
                                               transform.workPlaneFrame());
            const Point3D bridge = vectorSubtract(endpoint,
                                                  state.firstPointWorld);
            const qreal bridgeLength = vectorLength(bridge);
            if (bridgeLength <= 1.0e-9) {
                return result;
            }
            const Point3D bridgeDirection =
                vectorScale(bridge, 1.0 / bridgeLength);
            Point3D normal = vectorCross(bridgeDirection,
                                         state.referenceNormal);
            const qreal normalLength = vectorLength(normal);
            if (normalLength <= 1.0e-9) {
                return result;
            }
            normal = vectorScale(normal, 1.0 / normalLength);
            Point3D floorNormal = state.referenceNormal;
            const qreal floorNormalLength = vectorLength(floorNormal);
            if (floorNormalLength <= 1.0e-9) {
                return result;
            }
            floorNormal = vectorScale(floorNormal, 1.0 / floorNormalLength);
            const Point3D planeX = vectorCross(floorNormal, normal);
            const qreal planeXLength = vectorLength(planeX);
            if (planeXLength <= 1.0e-9) {
                return result;
            }

            WorkPlaneFrame frame;
            frame.origin = state.firstPointWorld;
            frame.xAxis = vectorScale(planeX, 1.0 / planeXLength);
            frame.yAxis = floorNormal;
            frame.normal = normal;
            frame.valid = true;
            if (!isValidWorkPlaneFrame(frame)) {
                return result;
            }

            state.perpendicularPlaneActive = true;
            state.twoPointPerpendicularNormal = normal;
            state.twoPointPerpendicularNormalValid = true;
            state.verticalOverrideAxis = 0;
            transform.setWorkPlaneFrame(frame);
            state.inputFrame = frame;
            state.inputFrameValid = true;
            setInputPoint(0, worldPointToWorkPlaneFrame(state.firstPointWorld,
                                                        frame));
        }
        result.changed = true;
        result.refreshCursor = true;
        return result;
    }

    if (inputStage() == ArcInputStage::SecondPoint &&
        state.referenceFrameValid) {
        state.perpendicularPlaneActive = !state.perpendicularPlaneActive;
        state.verticalOverrideAxis = 0;
        restoreChordReferenceFrame(context);
        result.changed = true;
        result.refreshCursor = true;
        return result;
    }

    if (inputStage() != ArcInputStage::Complete ||
        !state.chordWorldPointsValid) {
        return result;
    }

    const Point3D chord = vectorSubtract(state.secondPointWorld,
                                         state.firstPointWorld);
    const qreal chordLength = vectorLength(chord);
    if (chordLength <= 1.0e-9) {
        return result;
    }
    const Point3D chordDirection = vectorScale(chord, 1.0 / chordLength);
    const qreal verticalThreshold = state.wasVertical ? 0.98 : 0.995;
    const bool isVertical = std::abs(vectorDot(chordDirection,
                                               state.referenceNormal)) >
                            verticalThreshold;
    state.wasVertical = isVertical;
    if (isVertical) {
        state.verticalOverrideAxis = state.verticalOverrideAxis == Qt::Key_X
                                         ? Qt::Key_Y
                                         : Qt::Key_X;
    } else {
        state.perpendicularPlaneActive = !state.perpendicularPlaneActive;
        state.verticalOverrideAxis = 0;
        if (state.perpendicularPlaneActive) {
            state.twoPointPerpendicularNormal = vectorCross(
                chordDirection, state.referenceNormal);
            const qreal normalLength = vectorLength(
                state.twoPointPerpendicularNormal);
            if (normalLength > 1.0e-9) {
                state.twoPointPerpendicularNormal = vectorScale(
                    state.twoPointPerpendicularNormal, 1.0 / normalLength);
                state.twoPointPerpendicularNormalValid = true;
            } else {
                state.twoPointPerpendicularNormal = {};
                state.twoPointPerpendicularNormalValid = false;
            }
        } else {
            state.twoPointPerpendicularNormal = {};
            state.twoPointPerpendicularNormalValid = false;
        }
    }

    updateChordWorkPlane(context);
    result.changed = true;
    result.refreshCursor = true;
    return result;
}

void ArcTool::rebaseOnePointPreview(const QPointF &cursorPoint)
{
    const QVector<QPointF> &points = interactionState_.inputPoints;
    if (mode() != ArcMode::OnePoint || points.size() < 2) {
        return;
    }

    const QPointF radiusVector = points[1] - points[0];
    interactionState_.previewStartAngle =
        std::atan2(radiusVector.y(), radiusVector.x());
    const QPointF cursorVector = cursorPoint - points[0];
    if (std::hypot(cursorVector.x(), cursorVector.y()) > 1.0e-9) {
        interactionState_.previewPreviousAngle =
            std::atan2(cursorVector.y(), cursorVector.x());
        interactionState_.previewInitialized = true;
    } else {
        interactionState_.previewPreviousAngle =
            interactionState_.previewStartAngle +
            interactionState_.previewSweepAngle;
        interactionState_.previewInitialized = true;
    }
}

bool ArcTool::toggleAngleSnap()
{
    interactionState_.angleSnapEnabled = !interactionState_.angleSnapEnabled;
    return interactionState_.angleSnapEnabled;
}

bool ArcTool::togglePlaneLock(const WorkPlaneFrame &currentFrame,
                              bool hasPendingPoints)
{
    interactionState_.planeLocked = !interactionState_.planeLocked;
    if (interactionState_.planeLocked && !hasPendingPoints) {
        interactionState_.lockedFrame = currentFrame;
        interactionState_.lockedFrameValid = isValidWorkPlaneFrame(currentFrame);
    } else if (!interactionState_.planeLocked) {
        interactionState_.lockedFrame = {};
        interactionState_.lockedFrameValid = false;
        return !hasPendingPoints;
    }
    return false;
}

ArcTool::RadiusInputUpdate ArcTool::applyRadiusInput(
    qreal radius,
    const QVector<QPointF> &points,
    const QPointF &cursorPoint,
    bool geometrySnapActive)
{
    RadiusInputUpdate update;
    if (points.isEmpty() || !std::isfinite(radius)) {
        return update;
    }

    radius = std::max<qreal>(0.1, std::abs(radius));
    const QPointF center = points.first();
    QPointF direction = points.size() >= 2
                            ? points[1] - center
                            : cursorPoint - center;
    qreal directionLength = std::hypot(direction.x(), direction.y());
    if (directionLength <= 1.0e-9) {
        direction = QPointF(1.0, 0.0);
        directionLength = 1.0;
    }
    direction /= directionLength;
    qreal angle = std::atan2(direction.y(), direction.x());
    if (points.size() == 1 && interactionState_.angleSnapEnabled &&
        !geometrySnapActive) {
        angle = snapPreviewAngle(angle);
    }

    interactionState_.previewStartAngle = angle;
    update.startPoint = center +
                        QPointF(radius * std::cos(angle),
                                radius * std::sin(angle));
    update.accepted = true;
    if (points.size() == 1) {
        update.appendStartPoint = true;
        appendInputPoint(update.startPoint);
        initializePreviewTracking();
        return update;
    }

    interactionState_.previewPreviousAngle =
        angle + interactionState_.previewSweepAngle;
    interactionState_.previewInitialized = true;
    update.updateCursor = true;
    update.cursorPoint = center +
        QPointF(radius * std::cos(interactionState_.previewPreviousAngle),
                radius * std::sin(interactionState_.previewPreviousAngle));
    return update;
}

bool ArcTool::applyAngleInput(qreal degrees,
                              const QVector<QPointF> &points,
                              QPointF *cursorPoint)
{
    if (!std::isfinite(degrees) || points.size() < 2 || cursorPoint == nullptr) {
        return false;
    }

    constexpr qreal pi = 3.14159265358979323846;
    interactionState_.previewSweepAngle = -degrees * pi / 180.0;
    const QPointF radiusVector = points[1] - points[0];
    interactionState_.previewStartAngle =
        std::atan2(radiusVector.y(), radiusVector.x());
    interactionState_.previewPreviousAngle =
        interactionState_.previewStartAngle +
        interactionState_.previewSweepAngle;
    interactionState_.previewInitialized = true;
    const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
    *cursorPoint = points[0] +
        QPointF(radius * std::cos(interactionState_.previewPreviousAngle),
                radius * std::sin(interactionState_.previewPreviousAngle));
    interactionState_.angleValueLocked = true;
    return true;
}

bool ArcTool::applySagittaInput(qreal sagitta,
                                const QVector<QPointF> &points,
                                const QPointF &cursorPoint,
                                QPointF *updatedCursorPoint) const
{
    if (updatedCursorPoint == nullptr || points.size() < 2 ||
        !std::isfinite(sagitta)) {
        return false;
    }
    const QPointF chord = points[1] - points[0];
    const qreal chordLength = std::hypot(chord.x(), chord.y());
    if (chordLength <= 1.0e-9) {
        return false;
    }
    const QPointF midpoint = (points[0] + points[1]) * 0.5;
    const QPointF perpendicular(-chord.y() / chordLength,
                                chord.x() / chordLength);
    const qreal currentHeight = QPointF::dotProduct(cursorPoint - midpoint,
                                                    perpendicular);
    const qreal sign = currentHeight < 0.0 ? -1.0 : 1.0;
    *updatedCursorPoint = midpoint + perpendicular * (sign * std::abs(sagitta));
    return true;
}

bool ArcTool::resolveChordLengthEndpoint(qreal chordLength,
                                         const Point3D &cursorWorldPoint,
                                         bool axisConstraintActive,
                                         const Point3D &axisDirection,
                                         Point3D *endpointWorldPoint)
{
    if (endpointWorldPoint == nullptr ||
        !interactionState_.referenceFrameValid ||
        !std::isfinite(chordLength) || chordLength <= 1.0e-9) {
        return false;
    }

    Point3D direction = vectorSubtract(cursorWorldPoint,
                                       interactionState_.firstPointWorld);
    if (axisConstraintActive) {
        const qreal directionLength = vectorLength(direction);
        const qreal sign = directionLength > 1.0e-9 &&
                                   vectorDot(direction, axisDirection) < 0.0
                               ? -1.0
                               : 1.0;
        direction = vectorScale(axisDirection, sign);
    } else {
        const qreal directionLength = vectorLength(direction);
        if (directionLength <= 1.0e-9) {
            direction = interactionState_.referenceFrame.xAxis;
        } else {
            direction = vectorScale(direction, 1.0 / directionLength);
        }
    }

    *endpointWorldPoint = vectorAdd(
        interactionState_.firstPointWorld,
        vectorScale(direction, chordLength));
    interactionState_.secondPointWorld = *endpointWorldPoint;
    interactionState_.chordWorldPointsValid = true;
    interactionState_.axisConstraintKey = 0;
    interactionState_.resolvedChordPointValid = false;
    return true;
}

void ArcTool::resetPreviewTracking()
{
    interactionState_.previewInitialized = false;
    interactionState_.previewPreviousAngle = 0.0;
    interactionState_.previewSweepAngle = 0.0;
    interactionState_.angleValueLocked = false;
}

void ArcTool::initializePreviewTracking()
{
    resetPreviewTracking();
    const QVector<QPointF> &points = interactionState_.inputPoints;
    if (mode() != ArcMode::OnePoint || points.size() < 2) {
        return;
    }

    const QPointF radiusVector = points[1] - points[0];
    const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
    if (radius <= 1.0e-9) {
        return;
    }

    interactionState_.previewStartAngle =
        std::atan2(radiusVector.y(), radiusVector.x());
    interactionState_.previewPreviousAngle =
        interactionState_.previewStartAngle;
    interactionState_.previewInitialized = true;
}

void ArcTool::updatePreviewTracking(const QPointF &cursorPoint,
                                   bool geometrySnapActive)
{
    const QVector<QPointF> &points = interactionState_.inputPoints;
    if (mode() != ArcMode::OnePoint || points.size() < 2 ||
        interactionState_.angleValueLocked) {
        return;
    }

    const QPointF cursorVector = cursorPoint - points[0];
    const qreal radius = std::hypot(cursorVector.x(), cursorVector.y());
    if (radius <= 1.0e-9) {
        return;
    }

    qreal angle = std::atan2(cursorVector.y(), cursorVector.x());
    if (interactionState_.angleSnapEnabled && !geometrySnapActive) {
        angle = snapPreviewAngle(angle);
    }
    if (!interactionState_.previewInitialized) {
        const QPointF radiusVector = points[1] - points[0];
        interactionState_.previewStartAngle =
            std::atan2(radiusVector.y(), radiusVector.x());
        interactionState_.previewPreviousAngle = angle;
        interactionState_.previewInitialized = true;
        return;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    qreal delta = angle - interactionState_.previewPreviousAngle;
    if (delta > pi) {
        delta -= twoPi;
    } else if (delta < -pi) {
        delta += twoPi;
    }

    interactionState_.previewSweepAngle += delta;
    constexpr qreal sweepLimit = 2.0 * pi;
    if (std::abs(interactionState_.previewSweepAngle) > sweepLimit &&
        std::cos(angle - interactionState_.previewStartAngle) > 0.8) {
        qreal phase = std::fmod(interactionState_.previewSweepAngle + pi, twoPi);
        if (phase < 0.0) {
            phase += twoPi;
        }
        phase -= pi;
        interactionState_.previewSweepAngle =
            std::copysign(sweepLimit, interactionState_.previewSweepAngle) + phase;
    }
    interactionState_.previewPreviousAngle = angle;
}

QPointF ArcTool::constrainOnePointEndpoint(const QPointF &rawPoint) const
{
    const QVector<QPointF> &points = interactionState_.inputPoints;
    if (points.size() < 2) {
        return rawPoint;
    }
    const QPointF center = points[0];
    const QPointF startVector = points[1] - center;
    const qreal radius = std::hypot(startVector.x(), startVector.y());
    if (radius <= 1.0e-9) {
        return rawPoint;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal halfPi = pi / 2.0;
    constexpr qreal twoPi = 2.0 * pi;
    const qreal startAngle = std::atan2(startVector.y(), startVector.x());
    const QPointF rawVector = rawPoint - center;
    const qreal rawAngle = std::atan2(rawVector.y(), rawVector.x());

    qreal candidateSweep = rawAngle - startAngle;
    if (interactionState_.previewInitialized) {
        qreal delta = rawAngle - interactionState_.previewPreviousAngle;
        if (delta > pi) {
            delta -= twoPi;
        } else if (delta < -pi) {
            delta += twoPi;
        }
        candidateSweep = interactionState_.previewSweepAngle + delta;
    } else if (candidateSweep > pi) {
        candidateSweep -= twoPi;
    } else if (candidateSweep < -pi) {
        candidateSweep += twoPi;
    }

    const qreal snappedSweep = std::round(candidateSweep / halfPi) * halfPi;
    const qreal snappedAngle = startAngle + snappedSweep;
    return center + QPointF(radius * std::cos(snappedAngle),
                            radius * std::sin(snappedAngle));
}

QPointF ArcTool::constrainTwoPointThroughPoint(const QPointF &cursorPoint,
                                              bool geometrySnap,
                                              bool altModifier) const
{
    const QVector<QPointF> &points = interactionState_.inputPoints;
    if (points.size() < 2) {
        return cursorPoint;
    }
    const QPointF chord = points[1] - points[0];
    const qreal chordLength = std::hypot(chord.x(), chord.y());
    if (!std::isfinite(chordLength) || chordLength <= 1.0e-9) {
        return cursorPoint;
    }

    const QPointF midpoint = (points[0] + points[1]) / 2.0;
    const QPointF perpendicular(-chord.y() / chordLength,
                                chord.x() / chordLength);
    qreal height = QPointF::dotProduct(cursorPoint - midpoint, perpendicular);
    const qreal halfChord = chordLength / 2.0;
    if (!geometrySnap && !altModifier) {
        constexpr qreal defaultSnapStrengthPercent = 6.0;
        const qreal heightSnapTolerance =
            chordLength * defaultSnapStrengthPercent / 100.0;
        if (std::abs(std::abs(height) - halfChord) < heightSnapTolerance) {
            height = std::copysign(halfChord, height);
        }
    }

    return midpoint + perpendicular * height;
}

qreal ArcTool::snapPreviewAngle(qreal angle)
{
    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    constexpr qreal angleIncrement = pi / 12.0;
    constexpr qreal snapTolerance = 6.0 * pi / 180.0;
    const qreal nearest = std::round(angle / angleIncrement) * angleIncrement;
    return std::abs(std::remainder(angle - nearest, twoPi)) <= snapTolerance
               ? nearest
               : angle;
}

bool ArcTool::buildShape(const ToolContext &context, Shape *shape) const
{
    return context.createShape(id(),
                               points(),
                               context.arcMode(),
                               context.arcSweep(),
                               shape);
}

} // namespace classiCAD
