#include "line_tool.h"

#include "core/geometry/curve_construction.h"
#include "core/document/document.h"
#include "services/input/input_constraint_service.h"
#include "services/snapping/snap_engine.h"
#include "tool_context.h"
#include "services/viewport/viewport_transform.h"
#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {
Point3D subtract(const Point3D &a, const Point3D &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
qreal dot(const Point3D &a, const Point3D &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
qreal length(const Point3D &v) { return std::sqrt(dot(v, v)); }
Point3D normalized(const Point3D &v)
{
    const qreal magnitude = length(v);
    return magnitude > 1.0e-12
        ? Point3D{v.x / magnitude, v.y / magnitude, v.z / magnitude} : Point3D{};
}
bool isLengthCharacter(const QChar character)
{
    return character.isDigit() || character.isLetter() ||
           character == QLatin1Char('.') || character == QLatin1Char(',') ||
           character == QLatin1Char('-') || character == QLatin1Char('+') ||
           character == QLatin1Char('/') || character == QLatin1Char('\'') ||
           character == QLatin1Char('"') || character == QLatin1Char(' ');
}
bool canStartLengthInput(const QString &text)
{
    if (text.isEmpty()) return false;
    const QChar first = text.front();
    return first.isDigit() || first == QLatin1Char('.') ||
           first == QLatin1Char(',') || first == QLatin1Char('-') ||
           first == QLatin1Char('+');
}
QString lengthUnitSuffix(DocumentLengthUnit unit)
{
    switch (unit) {
    case DocumentLengthUnit::Millimeter: return QStringLiteral("mm");
    case DocumentLengthUnit::Centimeter: return QStringLiteral("cm");
    case DocumentLengthUnit::Meter: return QStringLiteral("m");
    case DocumentLengthUnit::Inch: return QStringLiteral("in");
    case DocumentLengthUnit::Foot: return QStringLiteral("ft");
    }
    return QStringLiteral("mm");
}
// Preserve the longest planar runs as local component curves. The completed
// drawing remains one object even when its components need different frames.
QVector<Shape> planarRuns(const QVector<Point3D> &points,
                         const WorkPlaneFrame &preferredFrame)
{
    QVector<Shape> result;
    for (int first = 0; first + 1 < points.size();) {
        WorkPlaneFrame inherited = preferredFrame;
        const qreal planeOffset = signedDistanceFromWorkPlaneFrame(points[first], inherited);
        inherited.origin.x += inherited.normal.x * planeOffset;
        inherited.origin.y += inherited.normal.y * planeOffset;
        inherited.origin.z += inherited.normal.z * planeOffset;
        const Point3D direction = normalized(subtract(points[first + 1], points[first]));
        const qreal normalAlongDirection = dot(preferredFrame.normal, direction);
        Point3D segmentNormal{
            preferredFrame.normal.x - direction.x * normalAlongDirection,
            preferredFrame.normal.y - direction.y * normalAlongDirection,
            preferredFrame.normal.z - direction.z * normalAlongDirection};
        if (length(segmentNormal) <= 1.0e-8) {
            const Point3D helper = std::abs(direction.z) < 0.9
                ? Point3D{0.0, 0.0, 1.0} : Point3D{0.0, 1.0, 0.0};
            const qreal along = dot(helper, direction);
            segmentNormal = {helper.x - direction.x * along,
                             helper.y - direction.y * along,
                             helper.z - direction.z * along};
        }
        QVector<WorkPlaneFrame> candidates = {
            inherited,
            makeWorkPlaneFrame(WorkPlane::XY, points[first].z),
            makeWorkPlaneFrame(WorkPlane::XZ, points[first].y),
            makeWorkPlaneFrame(WorkPlane::YZ, points[first].x),
            makeWorkPlaneFrameFromNormal(points[first], segmentNormal, direction)};
        // A snapped loop can occupy an arbitrary plane unrelated to the
        // drawing frame or principal planes. Its first noncollinear point
        // defines that plane; do not split a planar loop into loose edges.
        for (int index = first + 2; index < points.size(); ++index) {
            const Point3D bridge = subtract(points[index], points[first]);
            const Point3D normal{
                direction.y * bridge.z - direction.z * bridge.y,
                direction.z * bridge.x - direction.x * bridge.z,
                direction.x * bridge.y - direction.y * bridge.x};
            if (length(normal) > 1.0e-8 * std::max<qreal>(1.0, length(bridge))) {
                candidates.append(makeWorkPlaneFrameFromNormal(
                    points[first], normal, direction));
                break;
            }
        }
        int last = first;
        WorkPlaneFrame frame;
        for (const WorkPlaneFrame &candidate : candidates) {
            if (!isValidWorkPlaneFrame(candidate)) continue;
            int end = first;
            while (end + 1 < points.size() &&
                   std::abs(signedDistanceFromWorkPlaneFrame(points[end + 1], candidate)) <= 1.0e-8) {
                ++end;
            }
            if (end > last) {
                last = end;
                frame = candidate;
            }
        }
        if (last == first) return {};
        Shape shape;
        shape.geometryType = GeometryType::Line;
        shape.workPlaneFrame = frame;
        for (const WorkPlane plane : {WorkPlane::XY, WorkPlane::XZ, WorkPlane::YZ}) {
            if (std::abs(dot(frame.normal, workPlaneNormal(plane))) > 1.0 - 1.0e-8) {
                shape.workPlane = plane;
                shape.workPlaneOffset = plane == WorkPlane::XY ? frame.origin.z
                    : plane == WorkPlane::XZ ? frame.origin.y : frame.origin.x;
                break;
            }
        }
        for (int index = first; index <= last; ++index) {
            shape.points.append(worldPointToWorkPlaneFrame(points[index], frame));
        }
        shape.nurbs = makeDegreeOneNurbs(shape.points);
        if (!validateNurbsCurve(shape.nurbs)) return {};
        result.append(shape);
        first = last;
    }
    return result;
}
} // namespace

ToolId LineTool::id() const { return ToolId::Line; }
void LineTool::reset()
{
    points_.clear();
    drawingFrame_ = {};
    cursorPoint_ = {};
    hasCursorPoint_ = false;
    constraintAxisKey_ = 0;
    normalLock_ = false;
    planeLocked_ = false;
    lengthInput_.clear();
    shiftLockDirection_ = {};
    shiftLockActive_ = false;
    lastInput_ = {};
    snap_ = {};
    status_.canCommit = false;
}
void LineTool::begin(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Active;
    updateStatus(context);
    publish(context);
}
bool LineTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        commit(context);
        return true;
    }
    if (input.button != Qt::LeftButton) return false;
    lastInput_ = input;
    appendCurrentPoint(input, context);
    return true;
}
bool LineTool::appendCurrentPoint(const ToolInput &input,
                                  ToolContext &context,
                                  bool applyPendingLength)
{
    if (points_.isEmpty()) {
        if (!planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                ? input.workPlaneFrame : context.viewportTransform().workPlaneFrame();
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) return true;
        snap_ = input.snapResult.isValid() && input.snapResult.hasWorldPoint
            ? input.snapResult
            : context.snapEngine().findSpatialSnapPoint(
                  context.document(), input.screenPosition, nullptr,
                  context.viewportTransform(), input.viewportSize);
        cursorPoint_ = snap_.hasWorldPoint ? snap_.worldPoint
            : workPlaneFramePointToWorld(input.worldPosition, drawingFrame_);
        planeLocked_ = true;
    } else {
        cursorPoint_ = resolveCursorPoint(input, context);
        if (applyPendingLength) applyTypedLengthToCursor(context);
        if (length(subtract(cursorPoint_, points_.back())) <= 1.0e-8) return true;
    }
    points_.append(cursorPoint_);
    // The addon locks the normal, while moving the plane through each new pivot.
    const qreal offset = signedDistanceFromWorkPlaneFrame(cursorPoint_, drawingFrame_);
    drawingFrame_.origin.x += drawingFrame_.normal.x * offset;
    drawingFrame_.origin.y += drawingFrame_.normal.y * offset;
    drawingFrame_.origin.z += drawingFrame_.normal.z * offset;
    context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    if (snap_.hasWorldPoint) {
        QPointF snapScreen;
        if (context.viewportTransform().worldPointToScreen(snap_.worldPoint,
                input.viewportSize, &snapScreen)) {
            snap_.point = context.viewportTransform().screenToWorld(snapScreen, input.viewportSize);
        }
    }
    hasCursorPoint_ = true;
    if (points_.size() > 1) {
        constraintAxisKey_ = 0;
        normalLock_ = false;
    }
    lengthInput_.clear();
    shiftLockActive_ = false;
    status_.canCommit = points_.size() >= 2;
    updateStatus(context);
    publish(context);
    return true;
}
bool LineTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    lastInput_ = input;
    if (!planeLocked_) {
        drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
            ? input.workPlaneFrame : context.viewportTransform().workPlaneFrame();
    }
    if (points_.isEmpty()) {
        cursorPoint_ = workPlaneFramePointToWorld(input.worldPosition, drawingFrame_);
        return false;
    }
    cursorPoint_ = resolveCursorPoint(input, context);
    hasCursorPoint_ = true;
    updateStatus(context);
    publish(context);
    return true;
}
bool LineTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key == Qt::Key_Escape) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }

    if (!lengthInput_.isEmpty()) {
        if (input.key == Qt::Key_Backspace) {
            lengthInput_.chop(1);
            updateStatus(context);
            publish(context);
            return true;
        }
        if (input.key == Qt::Key_Return || input.key == Qt::Key_Enter) {
            qreal distance = 0.0;
            if (!points_.isEmpty() &&
                parseDocumentLengthInput(lengthInput_,
                                         context.document().settings().lengthUnit,
                                         &distance)) {
                appendCurrentPoint(lastInput_, context, true);
            } else {
                status_.text = QStringLiteral("Line: enter a valid segment length");
                publish(context);
            }
            return true;
        }
        const QString typedText = input.text;
        if (!typedText.isEmpty() &&
            !(input.modifiers & (Qt::ControlModifier | Qt::AltModifier |
                                 Qt::MetaModifier))) {
            bool accepted = true;
            for (const QChar character : typedText) {
                if (!isLengthCharacter(character)) {
                    accepted = false;
                    break;
                }
            }
            if (accepted) {
                lengthInput_.append(typedText == QStringLiteral(",")
                                        ? QStringLiteral(".")
                                        : typedText.toLower());
                updateStatus(context);
                publish(context);
                return true;
            }
        }
    }

    if (input.key == Qt::Key_L) {
        planeLocked_ = !planeLocked_;
        if (planeLocked_) {
            if (!isValidWorkPlaneFrame(drawingFrame_)) {
                drawingFrame_ = context.viewportTransform().workPlaneFrame();
            }
            const qreal offset = signedDistanceFromWorkPlaneFrame(cursorPoint_, drawingFrame_);
            drawingFrame_.origin.x += drawingFrame_.normal.x * offset;
            drawingFrame_.origin.y += drawingFrame_.normal.y * offset;
            drawingFrame_.origin.z += drawingFrame_.normal.z * offset;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        updateStatus(context);
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) {
        if (points_.size() <= 1) return false;
        points_.removeLast();
        const qreal offset = signedDistanceFromWorkPlaneFrame(points_.back(), drawingFrame_);
        drawingFrame_.origin.x += drawingFrame_.normal.x * offset;
        drawingFrame_.origin.y += drawingFrame_.normal.y * offset;
        drawingFrame_.origin.z += drawingFrame_.normal.z * offset;
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        constraintAxisKey_ = 0;
        normalLock_ = false;
        shiftLockActive_ = false;
        cursorPoint_ = resolveCursorPoint(lastInput_, context);
        status_.canCommit = points_.size() >= 2;
        updateStatus(context);
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
        input.key == Qt::Key_Z || input.key == Qt::Key_N) {
        if (input.key == Qt::Key_N) {
            if (points_.isEmpty()) return true;
            normalLock_ = !normalLock_;
            constraintAxisKey_ = 0;
        } else {
            normalLock_ = false;
            constraintAxisKey_ = constraintAxisKey_ == input.key ? 0 : input.key;
        }
        shiftLockActive_ = false;
        if (!points_.isEmpty()) cursorPoint_ = resolveCursorPoint(lastInput_, context);
        updateStatus(context);
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_Return || input.key == Qt::Key_Enter ||
        input.key == Qt::Key_Space) {
        commit(context);
        return true;
    }

    const bool plainTextInput =
        !(input.modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (!points_.isEmpty() && plainTextInput && canStartLengthInput(input.text)) {
        lengthInput_ = input.text == QStringLiteral(",")
            ? QStringLiteral(".") : input.text.toLower();
        updateStatus(context);
        publish(context);
        return true;
    }
    return false;
}
void LineTool::cancel(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("Line cancelled");
    publish(context);
}
void LineTool::commit(ToolContext &context)
{
    const QVector<Shape> shapes = planarRuns(points_, drawingFrame_);
    Shape connected;
    if (!shapes.isEmpty()) {
        connected = shapes.first();
        if (shapes.size() > 1) {
            connected.geometryType = GeometryType::PolyCurve;
            connected.nurbs = {};
            connected.points.clear();
            for (const Point3D &point : points_) {
                connected.points.append(worldPointToWorkPlaneFrame(
                    point, connected.workPlaneFrame));
            }
            for (const Shape &component : shapes) {
                connected.components.append(component.nurbs);
                connected.componentWorkPlaneFrames.append(component.workPlaneFrame);
            }
        }
    }
    if (!shapes.isEmpty() && context.commitShape(id(), connected)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("Line committed");
    } else {
        status_.text = QStringLiteral("Line discarded: at least two distinct points required");
    }
    reset();
    publish(context);
    context.finishTool(ToolId::Select);
}
ToolPreview LineTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = drawingFrame_;
    result.planeLocked = planeLocked_;
    result.snap = snap_;
    result.overridesSnap = true;
    result.worldPoints = points_;
    for (const Point3D &point : points_) {
        result.points.append(worldPointToWorkPlaneFrame(point, drawingFrame_));
    }
    result.worldCursorPoint = cursorPoint_;
    result.cursorPoint = worldPointToWorkPlaneFrame(cursorPoint_, drawingFrame_);
    result.hasCursorPoint = hasCursorPoint_ && !points_.isEmpty();
    QVector<Point3D> previewPoints = points_;
    if (result.hasCursorPoint && length(subtract(cursorPoint_, points_.back())) > 1.0e-8) {
        previewPoints.append(cursorPoint_);
    }
    result.shapes = planarRuns(previewPoints, drawingFrame_);
    result.statusText = status_.text;
    return result;
}
ToolStatus LineTool::status() const { return status_; }
Point3D LineTool::inferredAxisDirection(const ToolInput &input,
                                       const ToolContext &context) const
{
    return InputConstraintService::inferProjectedWorldAxis(
        points_.back(),
        input.screenPosition,
        context.viewportTransform(),
        input.viewportSize);
}
Point3D LineTool::resolveCursorPoint(const ToolInput &input, const ToolContext &context)
{
    const Point3D reference = points_.back();
    snap_ = input.snapResult.isValid() && input.snapResult.hasWorldPoint
        ? input.snapResult
        : context.snapEngine().findSpatialSnapPoint(
              context.document(), input.screenPosition, &reference,
              context.viewportTransform(), input.viewportSize, points_);
    const bool geometrySnap = snap_.hasWorldPoint || input.snapType != SnapType::None;
    const WorkPlaneFrame inputFrame = isValidWorkPlaneFrame(input.workPlaneFrame)
        ? input.workPlaneFrame : drawingFrame_;
    Point3D source = workPlaneFramePointToWorld(input.worldPosition, inputFrame);
    if (snap_.hasWorldPoint) source = snap_.worldPoint;
    if (planeLocked_ && !geometrySnap && !input.orthoEnabled &&
        !input.viewportSize.isEmpty()) {
        QPointF point;
        source = context.viewportTransform().screenToWorkPlane(
            input.screenPosition, input.viewportSize, drawingFrame_, &point)
            ? workPlaneFramePointToWorld(point, drawingFrame_) : reference;
    }
    Point3D direction = normalLock_
                            ? drawingFrame_.normal
                            : InputConstraintService::worldAxisDirection(
                                  constraintAxisKey_);
    if (input.modifiers.testFlag(Qt::ShiftModifier)) {
        if (!shiftLockActive_) {
            if (length(direction) <= 1.0e-12) direction = inferredAxisDirection(input, context);
            if (length(direction) <= 1.0e-12) direction = normalized(subtract(source, reference));
            shiftLockDirection_ = direction;
            shiftLockActive_ = length(direction) > 1.0e-12;
        }
        direction = shiftLockDirection_;
    } else {
        shiftLockActive_ = false;
        if (length(direction) <= 1.0e-12 && !geometrySnap) {
            direction = inferredAxisDirection(input, context);
        }
    }
    if (length(direction) <= 1.0e-12) return source;
    direction = normalized(direction);
    Point3D axisPoint;
    if (!geometrySnap &&
        InputConstraintService::worldPointOnScreenAxis(
            input.screenPosition,
            input.viewportSize,
            reference,
            direction,
            context.viewportTransform(),
            &axisPoint)) {
        return axisPoint;
    }
    return InputConstraintService::projectOntoWorldAxis(source,
                                                        reference,
                                                        direction);
}
void LineTool::applyTypedLengthToCursor(ToolContext &context)
{
    if (points_.isEmpty() || lengthInput_.isEmpty()) return;
    qreal distance = 0.0;
    if (!parseDocumentLengthInput(lengthInput_,
                                 context.document().settings().lengthUnit,
                                 &distance)) {
        return;
    }
    distance = std::max<qreal>(0.0001, std::abs(distance));
    const Point3D reference = points_.back();
    Point3D direction = normalized(subtract(cursorPoint_, reference));
    if (length(direction) <= 1.0e-12) {
        direction = InputConstraintService::worldAxisDirection(
            constraintAxisKey_);
        if (length(direction) <= 1.0e-12 && normalLock_) {
            direction = drawingFrame_.normal;
        }
        if (length(direction) <= 1.0e-12 && shiftLockActive_) {
            direction = shiftLockDirection_;
        }
        if (length(direction) <= 1.0e-12 && points_.size() >= 2) {
            direction = subtract(points_.back(), points_[points_.size() - 2]);
        }
        if (length(direction) <= 1.0e-12) direction = drawingFrame_.xAxis;
        direction = normalized(direction);
    }
    cursorPoint_ = {reference.x + direction.x * distance,
                    reference.y + direction.y * distance,
                    reference.z + direction.z * distance};
    hasCursorPoint_ = true;
}
void LineTool::updateStatus(const ToolContext &context)
{
    status_.canCommit = points_.size() >= 2;
    if (points_.isEmpty()) {
        status_.text = QStringLiteral("Line: click the first point");
        return;
    }

    const DocumentLengthUnit unit = context.document().settings().lengthUnit;
    const qreal unitScale = millimetersPerDocumentUnit(unit);
    const qreal segmentLength = hasCursorPoint_
        ? length(subtract(cursorPoint_, points_.back())) / unitScale : 0.0;
    if (!lengthInput_.isEmpty()) {
        const bool hasExplicitUnit = std::any_of(
            lengthInput_.cbegin(), lengthInput_.cend(),
            [](QChar character) {
                return character.isLetter() || character == QLatin1Char('\'') ||
                       character == QLatin1Char('"');
            });
        status_.text = QStringLiteral("Line: segment %1%2")
            .arg(lengthInput_ + QLatin1Char('|'),
                 hasExplicitUnit ? QString() : QStringLiteral(" %1").arg(lengthUnitSuffix(unit)));
    } else {
        status_.text = QStringLiteral("Line: segment %1 %2")
            .arg(QString::number(segmentLength, 'g', 5), lengthUnitSuffix(unit));
    }
    if (constraintAxisKey_ != 0) {
        status_.text += QStringLiteral(" • %1 axis")
            .arg(QChar(constraintAxisKey_));
    } else if (normalLock_) {
        status_.text += QStringLiteral(" • normal locked");
    } else if (shiftLockActive_) {
        status_.text += QStringLiteral(" • direction locked");
    }
    if (planeLocked_) status_.text += QStringLiteral(" • plane locked");
}
void LineTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}
} // namespace classiCAD
