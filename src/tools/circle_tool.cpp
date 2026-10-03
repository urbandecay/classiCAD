#include "circle_tool.h"

#include "core/document/document_settings.h"
#include "core/geometry/circle_construction.h"
#include "tool_context.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qreal kEpsilon = 1.0e-9;

qreal pointLength(const QPointF &point)
{
    return std::hypot(point.x(), point.y());
}

QPointF normalizedPoint(const QPointF &point)
{
    const qreal magnitude = pointLength(point);
    return magnitude > kEpsilon ? point / magnitude : QPointF();
}

Point3D subtractPoint(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D scalePoint(const Point3D &point, qreal scale)
{
    return {point.x * scale, point.y * scale, point.z * scale};
}

qreal dotPoint(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D crossPoint(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

Point3D normalizedPoint(const Point3D &point)
{
    const qreal magnitude = std::sqrt(dotPoint(point, point));
    return magnitude > kEpsilon ? scalePoint(point, 1.0 / magnitude)
                                : Point3D{};
}

bool isLengthCharacter(QChar character)
{
    return character.isDigit() || character.isLetter() ||
           character == QLatin1Char('.') || character == QLatin1Char(',') ||
           character == QLatin1Char('-') || character == QLatin1Char('+') ||
           character == QLatin1Char('/') || character == QLatin1Char('\'') ||
           character == QLatin1Char('"') || character == QLatin1Char(' ');
}

bool canStartLengthInput(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }
    const QChar first = text.front();
    return first.isDigit() || first == QLatin1Char('.') ||
           first == QLatin1Char(',') || first == QLatin1Char('-') ||
           first == QLatin1Char('+');
}

QString unitSuffix(DocumentLengthUnit unit)
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

void setLegacyPlaneMetadata(Shape *shape, const WorkPlaneFrame &frame)
{
    if (shape == nullptr) {
        return;
    }
    shape->workPlaneFrame = frame;
    shape->workPlane = WorkPlane::XY;
    shape->workPlaneOffset = frame.origin.z;
    constexpr qreal tolerance = 1.0e-8;
    if (std::abs(frame.normal.z) >= 1.0 - tolerance) {
        shape->workPlane = WorkPlane::XY;
        shape->workPlaneOffset = frame.origin.z;
    } else if (std::abs(frame.normal.y) >= 1.0 - tolerance) {
        shape->workPlane = WorkPlane::XZ;
        shape->workPlaneOffset = frame.origin.y;
    } else if (std::abs(frame.normal.x) >= 1.0 - tolerance) {
        shape->workPlane = WorkPlane::YZ;
        shape->workPlaneOffset = frame.origin.x;
    }
}

QPointF projectedWorldAxis(const WorkPlaneFrame &frame, int key)
{
    Point3D axis;
    switch (key) {
    case Qt::Key_X: axis = {1.0, 0.0, 0.0}; break;
    case Qt::Key_Y: axis = {0.0, 1.0, 0.0}; break;
    case Qt::Key_Z: axis = {0.0, 0.0, 1.0}; break;
    default: return {};
    }
    return normalizedPoint(QPointF(axis.x * frame.xAxis.x +
                                       axis.y * frame.xAxis.y +
                                       axis.z * frame.xAxis.z,
                                   axis.x * frame.yAxis.x +
                                       axis.y * frame.yAxis.y +
                                       axis.z * frame.yAxis.z));
}

} // namespace

CircleTool::CircleTool(ToolId tool)
    : tool_(tool)
{
}

ToolId CircleTool::id() const
{
    return tool_;
}

void CircleTool::begin(ToolContext &context)
{
    reset();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    status_.state = ToolLifecycleState::Active;
    updateStatus(context);
    publish(context);
}

bool CircleTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }
    if (input.button != Qt::LeftButton) {
        return false;
    }

    lastInput_ = input;
    if (points_.isEmpty()) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
            drawingFrame_ = context.viewportTransform().workPlaneFrame();
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
            return true;
        }
        referenceFrame_ = drawingFrame_;
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        frameCaptured_ = true;
        planeLocked_ = true;
        cursorPoint_ = input.positionInFrame(drawingFrame_);
        hasCursorPoint_ = true;
    } else {
        cursorPoint_ = resolveCursor(input);
        if (!(isThreePoint() && threePointPointsInDrawingFrame_)) {
            updatePerpendicularDrawingFrame(cursorPoint_, context);
        }
        hasCursorPoint_ = true;
    }

    if (isThreePoint() && points_.size() == 2) {
        QVector<QPointF> definition;
        if (!makeCircleDefinitionFromThreePoints(points_[0], points_[1],
                                                 cursorPoint_, &definition)) {
            status_.text = QStringLiteral("3-point circle: choose a point off the chord");
            hudInstructionsLine_ = status_.text;
            publish(context);
            return true;
        }
    }

    points_.append(cursorPoint_);
    if (isThreePoint() && points_.size() == 2 && perpendicularMode_ &&
        !threePointPointsInDrawingFrame_) {
        points_ = pointsInDrawingFrame(points_);
        cursorPoint_ = pointInDrawingFrame(cursorPoint_);
        threePointPointsInDrawingFrame_ = true;
    }
    numericInput_ = NumericInput::None;
    numericText_.clear();
    axisConstraintKey_ = 0;
    if (points_.size() == requiredPoints(tool_)) {
        finish(context);
        return true;
    }

    updateStatus(context);
    publish(context);
    return true;
}

bool CircleTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    lastInput_ = input;
    if (points_.isEmpty() && !planeLocked_ &&
        isValidWorkPlaneFrame(input.workPlaneFrame)) {
        drawingFrame_ = input.workPlaneFrame;
    }
    if (!points_.isEmpty()) {
        const WorkPlaneFrame activeFrame =
            isThreePoint() && threePointPointsInDrawingFrame_
                ? drawingFrame_
                : referenceFrame_;
        if (isValidWorkPlaneFrame(activeFrame)) {
            context.viewportTransform().setWorkPlaneFrame(activeFrame);
        }
    } else if (frameCaptured_ && isValidWorkPlaneFrame(drawingFrame_)) {
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    }

    cursorPoint_ = points_.isEmpty()
                       ? input.positionInFrame(drawingFrame_)
                       : resolveCursor(input);
    if (!points_.isEmpty() &&
        !(isThreePoint() && threePointPointsInDrawingFrame_)) {
        updatePerpendicularDrawingFrame(cursorPoint_, context);
    }
    hasCursorPoint_ = true;
    updateStatus(context);
    publish(context);
    return true;
}

bool CircleTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key == Qt::Key_Escape) {
        if (numericInput_ != NumericInput::None) {
            numericInput_ = NumericInput::None;
            numericText_.clear();
            updateStatus(context);
            publish(context);
            return true;
        }
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }

    if (numericInput_ != NumericInput::None) {
        if (input.key == Qt::Key_Backspace || input.key == Qt::Key_Delete) {
            numericText_.chop(1);
            updateStatus(context);
            publish(context);
            return true;
        }
        if (input.key == Qt::Key_Return || input.key == Qt::Key_Enter) {
            applyNumericInput(context);
            return true;
        }
        if (!input.text.isEmpty() &&
            !(input.modifiers & (Qt::ControlModifier | Qt::AltModifier |
                                 Qt::MetaModifier))) {
            for (QChar character : input.text) {
                if (isLengthCharacter(character)) {
                    numericText_.append(character == QLatin1Char(',')
                                            ? QLatin1Char('.')
                                            : character.toLower());
                }
            }
            updateStatus(context);
            publish(context);
            return true;
        }
        return true;
    }

    if (input.key == Qt::Key_L && points_.isEmpty()) {
        if (planeLocked_) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeLock_)
                                ? frameBeforeLock_
                                : context.viewportTransform().workPlaneFrame();
            planeLocked_ = false;
            normalAxisLockKey_ = 0;
        } else if (isValidWorkPlaneFrame(input.workPlaneFrame)) {
            frameBeforeLock_ = drawingFrame_;
            drawingFrame_ = input.workPlaneFrame;
            planeLocked_ = true;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        updateStatus(context);
        publish(context);
        return true;
    }

    if (input.key == Qt::Key_P && !points_.isEmpty()) {
        const QPointF cursorInCurrentFrame = resolveCursor(input);
        perpendicularMode_ = !perpendicularMode_;
        hasCursorPoint_ = true;

        if (isThreePoint()) {
            if (perpendicularMode_) {
                if (points_.size() == 1) {
                    updatePerpendicularDrawingFrame(cursorInCurrentFrame, context);
                    cursorPoint_ = cursorInCurrentFrame;
                } else if (!threePointPointsInDrawingFrame_) {
                    updatePerpendicularDrawingFrame(points_[1], context);
                    if (isValidWorkPlaneFrame(drawingFrame_)) {
                        points_ = pointsInDrawingFrame(points_);
                        cursorPoint_ = pointInDrawingFrame(cursorInCurrentFrame);
                        threePointPointsInDrawingFrame_ = true;
                    }
                } else {
                    cursorPoint_ = cursorInCurrentFrame;
                }
            } else {
                if (threePointPointsInDrawingFrame_ &&
                    isValidWorkPlaneFrame(drawingFrame_) &&
                    isValidWorkPlaneFrame(referenceFrame_)) {
                    for (QPointF &point : points_) {
                        point = worldPointToWorkPlaneFrame(
                            workPlaneFramePointToWorld(point, drawingFrame_),
                            referenceFrame_);
                    }
                    cursorPoint_ = worldPointToWorkPlaneFrame(
                        workPlaneFramePointToWorld(cursorInCurrentFrame,
                                                   drawingFrame_),
                        referenceFrame_);
                } else {
                    cursorPoint_ = cursorInCurrentFrame;
                }
                threePointPointsInDrawingFrame_ = false;
                drawingFrame_ = referenceFrame_;
            }
        } else {
            cursorPoint_ = cursorInCurrentFrame;
            if (perpendicularMode_) {
                updatePerpendicularDrawingFrame(cursorPoint_, context);
            } else if (isValidWorkPlaneFrame(referenceFrame_)) {
                drawingFrame_ = referenceFrame_;
            }
        }
        updateStatus(context);
        publish(context);
        return true;
    }

    if ((input.key == Qt::Key_X || input.key == Qt::Key_Y ||
         input.key == Qt::Key_Z) && !points_.isEmpty() &&
        (!isThreePoint() || points_.size() == 1)) {
        if (pointLength(projectedWorldAxis(referenceFrame_, input.key)) <= kEpsilon) {
            status_.text = QStringLiteral("%1: %2 is normal to the circle plane")
                               .arg(toolName(tool_), QChar(input.key));
            hudInstructionsLine_ = status_.text;
            publish(context);
            return true;
        }
        axisConstraintKey_ = axisConstraintKey_ == input.key ? 0 : input.key;
        updateStatus(context);
        publish(context);
        return true;
    }

    if (input.key == Qt::Key_R &&
        ((isOnePoint() && points_.size() == 1) ||
         (isThreePoint() && points_.size() == 2))) {
        beginNumericInput(NumericInput::Radius);
        updateStatus(context);
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_D &&
        ((isDiameter() && points_.size() == 1) ||
         (isThreePoint() && points_.size() == 1))) {
        beginNumericInput(NumericInput::Diameter);
        updateStatus(context);
        publish(context);
        return true;
    }
    const bool plainTextInput =
        !(input.modifiers & (Qt::ControlModifier | Qt::AltModifier |
                             Qt::MetaModifier));
    if (!points_.isEmpty() && plainTextInput && canStartLengthInput(input.text)) {
        NumericInput mode = NumericInput::None;
        if (isOnePoint() && points_.size() == 1) {
            mode = NumericInput::Radius;
        } else if (isDiameter() && points_.size() == 1) {
            mode = NumericInput::Diameter;
        } else if (isThreePoint() && points_.size() == 1) {
            mode = NumericInput::Diameter;
        } else if (isThreePoint() && points_.size() == 2) {
            mode = NumericInput::Radius;
        }
        if (mode != NumericInput::None) {
            beginNumericInput(mode, input.text);
            updateStatus(context);
            publish(context);
            return true;
        }
    }
    return false;
}

void CircleTool::cancel(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview CircleTool::preview() const
{
    ToolPreview result;
    result.points = points_;
    result.workPlaneFrame = isThreePoint() && threePointPointsInDrawingFrame_
                                ? drawingFrame_
                                : isValidWorkPlaneFrame(referenceFrame_)
                                      ? referenceFrame_
                                      : drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(result.workPlaneFrame);
    result.planeLocked = planeLocked_ || frameCaptured_;
    result.cursorPoint = cursorPoint_;
    result.hasCursorPoint = hasCursorPoint_;
    result.statusText = status_.text;
    result.hudDimensionsLine = hudDimensionsLine_;
    result.hudInstructionsLine = hudInstructionsLine_;

    QVector<QPointF> candidatePoints = points_;
    if (hasCursorPoint_ && candidatePoints.size() < requiredPoints(tool_)) {
        candidatePoints.append(cursorPoint_);
    }
    QVector<QPointF> definition;
    if (circleDefinition(candidatePoints, &definition)) {
        result.shape = makeCircleShape(definition);
        result.hasShape = validateNurbsCurve(result.shape.nurbs);
    }
    return result;
}

ToolStatus CircleTool::status() const
{
    return status_;
}

bool CircleTool::isOnePoint() const
{
    return tool_ == ToolId::Circle;
}

bool CircleTool::isDiameter() const
{
    return tool_ == ToolId::CircleDiameter;
}

bool CircleTool::isThreePoint() const
{
    return tool_ == ToolId::CircleThreePoint;
}

QPointF CircleTool::pointInDrawingFrame(const QPointF &point) const
{
    if (!isValidWorkPlaneFrame(referenceFrame_) ||
        !isValidWorkPlaneFrame(drawingFrame_)) {
        return point;
    }
    const Point3D worldPoint = workPlaneFramePointToWorld(point, referenceFrame_);
    return worldPointToWorkPlaneFrame(worldPoint, drawingFrame_);
}

QVector<QPointF> CircleTool::pointsInDrawingFrame(
    const QVector<QPointF> &points) const
{
    QVector<QPointF> converted;
    converted.reserve(points.size());
    for (const QPointF &point : points) {
        converted.append(pointInDrawingFrame(point));
    }
    return converted;
}

void CircleTool::updatePerpendicularDrawingFrame(const QPointF &cursorPoint,
                                                 const ToolContext &context)
{
    if (!perpendicularMode_ || points_.isEmpty() ||
        (isThreePoint() && threePointPointsInDrawingFrame_) ||
        (!isOnePoint() && !isDiameter() && !isThreePoint()) ||
        !isValidWorkPlaneFrame(referenceFrame_)) {
        return;
    }

    const Point3D anchor =
        workPlaneFramePointToWorld(points_.first(), referenceFrame_);
    const Point3D target =
        workPlaneFramePointToWorld(cursorPoint, referenceFrame_);
    const Point3D referenceNormal = normalizedPoint(referenceFrame_.normal);
    Point3D bridge = subtractPoint(target, anchor);
    bridge = subtractPoint(bridge,
                           scalePoint(referenceNormal,
                                      dotPoint(bridge, referenceNormal)));
    Point3D xAxis = normalizedPoint(bridge);
    if (dotPoint(xAxis, xAxis) <= kEpsilon) {
        return;
    }

    Point3D normal = normalizedPoint(crossPoint(xAxis, referenceNormal));
    const Point3D viewDirection =
        normalizedPoint(context.viewportTransform().viewDirection());
    if (dotPoint(normal, viewDirection) > 0.0) {
        normal = scalePoint(normal, -1.0);
    }

    Point3D yAxis;
    if (isDiameter() || isThreePoint()) {
        yAxis = referenceNormal;
        xAxis = normalizedPoint(crossPoint(yAxis, normal));
    } else {
        yAxis = normalizedPoint(crossPoint(normal, xAxis));
    }
    WorkPlaneFrame perpendicularFrame;
    perpendicularFrame.origin = anchor;
    perpendicularFrame.xAxis = xAxis;
    perpendicularFrame.yAxis = yAxis;
    perpendicularFrame.normal = normal;
    perpendicularFrame.valid = true;
    if (isValidWorkPlaneFrame(perpendicularFrame)) {
        drawingFrame_ = perpendicularFrame;
    }
}

QPointF CircleTool::resolveCursor(const ToolInput &input) const
{
    const WorkPlaneFrame &inputFrame =
        isThreePoint() && threePointPointsInDrawingFrame_
            ? drawingFrame_ : referenceFrame_;
    QPointF target = isValidWorkPlaneFrame(inputFrame)
                         ? input.positionInFrame(inputFrame)
                         : input.worldPosition;
    const bool altBypass = input.modifiers.testFlag(Qt::AltModifier);
    const int pointCount = points_.size();
    if (pointCount == 0 || !isValidWorkPlaneFrame(drawingFrame_)) {
        return target;
    }

    const QPointF anchor = points_.first();
    const bool threePointThird = isThreePoint() && pointCount >= 2;
    if (!altBypass && axisConstraintKey_ != 0) {
        const WorkPlaneFrame &constraintFrame =
            isThreePoint() && threePointPointsInDrawingFrame_
                ? drawingFrame_
                : referenceFrame_;
        const QPointF axis = projectedWorldAxis(constraintFrame,
                                                axisConstraintKey_);
        if (pointLength(axis) > kEpsilon) {
            target = anchor + axis * QPointF::dotProduct(target - anchor, axis);
        }
        return target;
    }

    const bool canAutoSnap = !altBypass && input.snapType == SnapType::None &&
                             !threePointThird;
    if (canAutoSnap) {
        const QPointF delta = target - anchor;
        const qreal deltaLength = pointLength(delta);
        if (deltaLength > kEpsilon) {
            constexpr qreal threshold = 0.9945218953682733; // cos(6 degrees)
            QPointF bestAxis;
            qreal bestAlignment = threshold;
            for (const int key : {Qt::Key_X, Qt::Key_Y, Qt::Key_Z}) {
                const WorkPlaneFrame &axisFrame =
                    isThreePoint() && threePointPointsInDrawingFrame_
                        ? drawingFrame_
                        : referenceFrame_;
                const QPointF axis = projectedWorldAxis(axisFrame, key);
                if (pointLength(axis) <= kEpsilon) {
                    continue;
                }
                const qreal alignment =
                    std::abs(QPointF::dotProduct(delta / deltaLength, axis));
                if (alignment >= bestAlignment) {
                    bestAlignment = alignment;
                    bestAxis = axis;
                }
            }
            if (pointLength(bestAxis) > kEpsilon) {
                if (QPointF::dotProduct(delta, bestAxis) < 0.0) {
                    bestAxis = -bestAxis;
                }
                target = anchor + bestAxis * QPointF::dotProduct(delta, bestAxis);
            }
        }
    }
    return target;
}

bool CircleTool::circleDefinition(const QVector<QPointF> &candidatePoints,
                                  QVector<QPointF> *definition) const
{
    if (definition == nullptr) {
        return false;
    }
    const QVector<QPointF> drawingPoints =
        isThreePoint() && threePointPointsInDrawingFrame_
            ? candidatePoints
            : pointsInDrawingFrame(candidatePoints);
    if (isOnePoint()) {
        if (drawingPoints.size() < 2 ||
            pointLength(drawingPoints[1] - drawingPoints[0]) <= kEpsilon) {
            return false;
        }
        *definition = {drawingPoints[0], drawingPoints[1]};
        return true;
    }
    if (isDiameter()) {
        return drawingPoints.size() >= 2 &&
               makeCircleDefinitionFromDiameter(drawingPoints[0],
                                                drawingPoints[1],
                                                definition);
    }
    return drawingPoints.size() >= 3 &&
           makeCircleDefinitionFromThreePoints(drawingPoints[0],
                                               drawingPoints[1],
                                               drawingPoints[2],
                                               definition);
}

Shape CircleTool::makeCircleShape(const QVector<QPointF> &definition) const
{
    Shape shape;
    shape.geometryType = GeometryType::Circle;
    shape.points = definition;
    setLegacyPlaneMetadata(&shape, drawingFrame_);
    shape.nurbs = makeCircleNurbs(definition);
    return shape;
}

void CircleTool::applyNumericInput(ToolContext &context)
{
    if (numericText_.isEmpty()) {
        numericInput_ = NumericInput::None;
        updateStatus(context);
        publish(context);
        return;
    }

    qreal valueMillimeters = 0.0;
    if (!parseDocumentLengthInput(numericText_,
                                  context.document().settings().lengthUnit,
                                  &valueMillimeters)) {
        status_.text = QStringLiteral("Circle: enter a valid length");
        hudInstructionsLine_ = status_.text;
        publish(context);
        return;
    }
    const qreal value = std::abs(valueMillimeters);
    if (numericInput_ == NumericInput::Radius && isOnePoint() &&
        points_.size() == 1) {
        QPointF direction = normalizedPoint(cursorPoint_ - points_[0]);
        if (pointLength(direction) <= kEpsilon) {
            direction = {1.0, 0.0};
        }
        cursorPoint_ = points_[0] + direction * value;
        hasCursorPoint_ = true;
    } else if (numericInput_ == NumericInput::Diameter &&
               points_.size() == 1 && (isDiameter() || isThreePoint())) {
        QPointF direction = normalizedPoint(cursorPoint_ - points_[0]);
        if (pointLength(direction) <= kEpsilon) {
            direction = {1.0, 0.0};
        }
        cursorPoint_ = points_[0] + direction * value;
        hasCursorPoint_ = true;
    } else if (numericInput_ == NumericInput::Radius && isThreePoint() &&
               points_.size() == 2) {
        const QPointF chord = points_[1] - points_[0];
        const qreal chordLength = pointLength(chord);
        if (chordLength <= kEpsilon || value < chordLength * 0.5) {
            status_.text = QStringLiteral("3-point circle: radius must be at least half the chord");
            hudInstructionsLine_ = status_.text;
            publish(context);
            return;
        }
        const QPointF chordUnit = chord / chordLength;
        const QPointF perpendicular(-chordUnit.y(), chordUnit.x());
        const QPointF midpoint = (points_[0] + points_[1]) * 0.5;
        qreal side = QPointF::dotProduct(cursorPoint_ - midpoint, perpendicular);
        if (std::abs(side) <= kEpsilon) {
            side = 1.0;
        }
        const qreal halfChord = chordLength * 0.5;
        const qreal centerOffset = std::sqrt(std::max<qreal>(
            0.0, value * value - halfChord * halfChord));
        const qreal sagitta = value - centerOffset;
        cursorPoint_ = midpoint + perpendicular * std::copysign(sagitta, side);
        hasCursorPoint_ = true;
    }

    updatePerpendicularDrawingFrame(cursorPoint_, context);

    numericInput_ = NumericInput::None;
    numericText_.clear();
    updateStatus(context);
    publish(context);
}

void CircleTool::beginNumericInput(NumericInput mode, const QString &initialText)
{
    numericInput_ = mode;
    numericText_ = initialText;
}

void CircleTool::updateStatus(const ToolContext &context)
{
    const DocumentSettings settings = context.document().settings();
    const qreal unitsPerMillimeter =
        1.0 / millimetersPerDocumentUnit(settings.lengthUnit);
    const QString suffix = unitSuffix(settings.lengthUnit);
    const auto formatLength = [&](qreal millimeters) {
        return QStringLiteral("%1 %2")
            .arg(millimeters * unitsPerMillimeter, 0, 'f', 3)
            .arg(suffix);
    };

    qreal distance = 0.0;
    if (hasCursorPoint_ && !points_.isEmpty()) {
        distance = pointLength(cursorPoint_ - points_.first());
    }
    QString dimensions;
    if (numericInput_ == NumericInput::Radius) {
        dimensions = QStringLiteral("R: %1| %2")
                         .arg(numericText_)
                         .arg(suffix);
    } else if (numericInput_ == NumericInput::Diameter) {
        dimensions = QStringLiteral("D: %1| %2")
                         .arg(numericText_)
                         .arg(suffix);
    } else if (isOnePoint()) {
        dimensions = points_.isEmpty()
                         ? QStringLiteral("R: —")
                         : QStringLiteral("R: %1").arg(formatLength(distance));
    } else if (isDiameter()) {
        dimensions = points_.isEmpty()
                         ? QStringLiteral("D: —")
                         : QStringLiteral("D: %1").arg(formatLength(distance));
    } else if (points_.size() < 2) {
        dimensions = QStringLiteral("Chord: —");
    } else {
        QVector<QPointF> candidatePoints = points_;
        if (hasCursorPoint_ && candidatePoints.size() < 3) {
            candidatePoints.append(cursorPoint_);
        }
        QVector<QPointF> definition;
        qreal radius = 0.0;
        if (circleDefinition(candidatePoints, &definition)) {
            radius = pointLength(definition[1] - definition[0]);
        }
        if (radius > kEpsilon) {
            dimensions = QStringLiteral("R: %1").arg(formatLength(radius));
        } else {
            dimensions = QStringLiteral("Chord: %1").arg(
                formatLength(pointLength(points_[1] - points_[0])));
        }
    }
    QString instructions;
    if (points_.isEmpty()) {
        instructions = QStringLiteral("Click %1  •  L plane lock  •  Esc exits")
                           .arg(isOnePoint() ? QStringLiteral("center")
                                             : isDiameter()
                                                   ? QStringLiteral("first diameter point")
                                                   : QStringLiteral("first circle point"));
    } else if (isOnePoint()) {
        instructions = QStringLiteral("Click radius  •  R radius  •  P perpendicular  •  X/Y/Z axis  •  Alt bypass");
    } else if (isDiameter()) {
        instructions = QStringLiteral("Click opposite diameter point  •  D diameter  •  P perpendicular  •  X/Y/Z axis  •  Alt bypass");
    } else if (points_.size() == 1) {
        instructions = QStringLiteral("Click second point  •  D chord length  •  P perpendicular  •  X/Y/Z axis  •  Alt bypass");
    } else {
        instructions = QStringLiteral("Click point on circle  •  R radius  •  P perpendicular");
    }
    if (!points_.isEmpty()) {
        instructions += QStringLiteral("  •  Perp %1")
                           .arg(perpendicularMode_ ? QStringLiteral("on")
                                                   : QStringLiteral("off"));
    }
    if (numericInput_ != NumericInput::None) {
        instructions = QStringLiteral("Enter applies value  •  Esc cancels input");
    }

    status_.state = ToolLifecycleState::Active;
    status_.canCommit = false;
    status_.text = QStringLiteral("%1  •  %2")
                       .arg(toolName(tool_), dimensions);
    hudDimensionsLine_ = dimensions;
    hudInstructionsLine_ = instructions;
}

void CircleTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

void CircleTool::finish(ToolContext &context)
{
    QVector<QPointF> definition;
    Shape shape;
    if (circleDefinition(points_, &definition)) {
        shape = makeCircleShape(definition);
    }
    if (validateNurbsCurve(shape.nurbs) && context.commitShape(tool_, shape)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("%1 committed").arg(toolName(tool_));
        numericInput_ = NumericInput::None;
        numericText_.clear();
        publish(context);
        context.finishTool(ToolId::Select);
        return;
    }

    const QString failure = isThreePoint()
                                ? QStringLiteral("3-point circle: the three points must not be collinear")
                                : QStringLiteral("Circle: choose distinct points");
    points_.removeLast();
    numericInput_ = NumericInput::None;
    numericText_.clear();
    updateStatus(context);
    status_.text = failure;
    hudInstructionsLine_ = failure;
    publish(context);
}

void CircleTool::reset()
{
    points_.clear();
    referenceFrame_ = {};
    drawingFrame_ = {};
    frameBeforeLock_ = {};
    cursorPoint_ = {};
    lastInput_ = {};
    numericText_.clear();
    numericInput_ = NumericInput::None;
    axisConstraintKey_ = 0;
    normalAxisLockKey_ = 0;
    hasCursorPoint_ = false;
    frameCaptured_ = false;
    planeLocked_ = false;
    perpendicularMode_ = false;
    threePointPointsInDrawingFrame_ = false;
    status_.canCommit = false;
}

} // namespace classiCAD
