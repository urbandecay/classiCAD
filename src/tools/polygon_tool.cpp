#include "polygon_tool.h"

#include "core/document/document_settings.h"
#include "tool_context.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qreal kEpsilon = 1.0e-9;
constexpr qreal kAxisSnapCosine = 0.9945218953682733; // cos(6 degrees)

qreal length(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

bool isLengthCharacter(QChar character)
{
    return character.isDigit() || character.isLetter() ||
           character == QLatin1Char('.') || character == QLatin1Char(',') ||
           character == QLatin1Char('-') || character == QLatin1Char('+') ||
           character == QLatin1Char('/') || character == QLatin1Char('\'') ||
           character == QLatin1Char('"') || character == QLatin1Char(' ');
}

bool isNumericStart(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }
    const QChar first = text.front();
    return first.isDigit() || first == QLatin1Char('.') ||
           first == QLatin1Char(',') || first == QLatin1Char('-') ||
           first == QLatin1Char('+');
}

Point3D normalForAxisKey(int key)
{
    switch (key) {
    case Qt::Key_X:
        return {1.0, 0.0, 0.0};
    case Qt::Key_Y:
        return {0.0, 1.0, 0.0};
    case Qt::Key_Z:
        return {0.0, 0.0, 1.0};
    default:
        return {};
    }
}

Point3D addPoint(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D subtractPoint(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D scalePoint(const Point3D &point, qreal amount)
{
    return {point.x * amount, point.y * amount, point.z * amount};
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

qreal lengthPoint(const Point3D &point)
{
    return std::sqrt(dotPoint(point, point));
}

Point3D normalizedPoint(const Point3D &point)
{
    const qreal magnitude = lengthPoint(point);
    return magnitude > kEpsilon ? scalePoint(point, 1.0 / magnitude) : Point3D{};
}

Point3D projectPointToFrame(const Point3D &point,
                            const WorkPlaneFrame &frame)
{
    if (!isValidWorkPlaneFrame(frame)) {
        return {};
    }
    const Point3D normal = normalizedPoint(frame.normal);
    return subtractPoint(point,
                         scalePoint(normal,
                                    dotPoint(subtractPoint(point, frame.origin),
                                             normal)));
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

void setPolygonWorkPlaneFrame(Shape *shape, const WorkPlaneFrame &frame)
{
    if (shape == nullptr) {
        return;
    }
    shape->workPlaneFrame = frame;
}

} // namespace

PolygonTool::PolygonTool(ToolId tool)
    : tool_(tool)
{
}

ToolId PolygonTool::id() const
{
    return tool_;
}

void PolygonTool::begin(ToolContext &context)
{
    reset();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    sideCount_ = effectiveSideCount(context.polygonSideCount());
    context.setPolygonSideCount(sideCount_);
    status_.state = ToolLifecycleState::Active;
    updateStatus(context);
    publish(context);
}

bool PolygonTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button != Qt::LeftButton) {
        return false;
    }

    lastInput_ = input;
    if (!frameCaptured_) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        if (!isValidWorkPlaneFrame(drawingFrame_)) {
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
        const QPointF firstPoint = input.worldPosition;
        anchorWorld_ = workPlaneFramePointToWorld(firstPoint, referenceFrame_);
        points_.append(firstPoint);
        cursorPoint_ = firstPoint;
        hasCursorPoint_ = true;
        frameCaptured_ = true;
        planeLocked_ = true;
        numericPointLocked_ = false;
        updateStatus(context);
        publish(context);
        return true;
    }

    if (!numericPointLocked_) {
        updateFromInput(input, context);
    }
    numericPointLocked_ = false;

    const QVector<QPointF> vertices = polygonVertices();
    if (vertices.size() < 3) {
        status_.text = QStringLiteral("Polygon: choose a point away from the first point");
        hudInstructionsLine_ = status_.text;
        publish(context);
        return true;
    }

    finish(context);
    return true;
}

bool PolygonTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    lastInput_ = input;
    if (!frameCaptured_ && !planeLocked_ &&
        isValidWorkPlaneFrame(input.workPlaneFrame)) {
        drawingFrame_ = input.workPlaneFrame;
    }
    if (frameCaptured_ && isValidWorkPlaneFrame(drawingFrame_)) {
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    }

    numericPointLocked_ = false;
    updateFromInput(input, context);
    updateStatus(context);
    publish(context);
    return true;
}

bool PolygonTool::handleWheel(const ToolInput &input, ToolContext &context)
{
    wheelRemainder_ += input.wheelAngleDelta;
    int steps = wheelRemainder_ / 120;
    if (steps == 0 && input.wheelPixelDelta != 0) {
        wheelRemainder_ += input.wheelPixelDelta * 120 / 100;
        steps = wheelRemainder_ / 120;
    }
    if (steps == 0) {
        return true;
    }
    wheelRemainder_ %= 120;

    const int stepSize = mode() == PolygonMode::Edge ? 2 : 1;
    int requested = std::clamp(sideCount_ + steps * stepSize, 3, 256);
    if (mode() == PolygonMode::Edge && requested % 2 == 0) {
        requested += steps > 0 ? 1 : -1;
    }
    setSideCount(requested, context, true);
    updateStatus(context);
    publish(context);
    return true;
}

bool PolygonTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key == Qt::Key_Escape) {
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
                const bool accepted = numericInput_ == NumericInput::SideCount
                                          ? character.isDigit()
                                          : isLengthCharacter(character);
                if (accepted) {
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

    if (points_.isEmpty() && input.key == Qt::Key_L) {
        if (planeLocked_) {
            if (planeAxisLockKey_ != 0 &&
                isValidWorkPlaneFrame(frameBeforeLock_)) {
                drawingFrame_ = frameBeforeLock_;
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            } else if (isValidWorkPlaneFrame(frameBeforeLock_)) {
                drawingFrame_ = frameBeforeLock_;
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
            planeAxisLockKey_ = 0;
            planeLocked_ = false;
            manualPlaneLock_ = false;
        } else if (isValidWorkPlaneFrame(input.workPlaneFrame)) {
            frameBeforeLock_ = drawingFrame_;
            drawingFrame_ = input.workPlaneFrame;
            planeLocked_ = true;
            manualPlaneLock_ = true;
            context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        }
        updateStatus(context);
        publish(context);
        return true;
    }

    if (points_.isEmpty() &&
        (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
         input.key == Qt::Key_Z)) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) {
            drawingFrame_ = input.workPlaneFrame;
        }
        if (planeAxisLockKey_ == input.key) {
            drawingFrame_ = isValidWorkPlaneFrame(frameBeforeLock_)
                                ? frameBeforeLock_
                                : input.workPlaneFrame;
            planeAxisLockKey_ = 0;
            planeLocked_ = manualPlaneLock_;
            if (isValidWorkPlaneFrame(drawingFrame_)) {
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
        } else {
            if (planeAxisLockKey_ == 0) {
                frameBeforeLock_ = drawingFrame_;
            }
            const Point3D normal = normalForAxisKey(input.key);
            QPointF cursor = input.rawWorldPosition;
            if (!std::isfinite(cursor.x()) || !std::isfinite(cursor.y())) {
                cursor = {};
            }
            const Point3D worldOrigin = workPlaneFramePointToWorld(cursor,
                                                                    input.workPlaneFrame);
            const WorkPlaneFrame axisFrame = makeWorkPlaneFrameFromNormal(
                worldOrigin, normal, drawingFrame_.xAxis);
            if (isValidWorkPlaneFrame(axisFrame)) {
                drawingFrame_ = axisFrame;
                planeAxisLockKey_ = input.key;
                planeLocked_ = true;
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
        }
        updateStatus(context);
        publish(context);
        return true;
    }

    if (!points_.isEmpty() &&
        (input.key == Qt::Key_X || input.key == Qt::Key_Y)) {
        // The add-on's final handle_input definition makes X/Y choose the
        // fallback plane when the sizing direction is vertical. Earlier
        // definitions with axis constraints are shadowed in Python.
        verticalOverrideAxis_ = input.key;
        numericPointLocked_ = false;
        updateFromInput(lastInput_, context);
        updateStatus(context);
        publish(context);
        return true;
    }
    if (!points_.isEmpty() && input.key == Qt::Key_P) {
        perpendicularMode_ = !perpendicularMode_;
        verticalOverrideAxis_ = 0;
        numericPointLocked_ = false;
        updateFromInput(lastInput_, context);
        updateStatus(context);
        publish(context);
        return true;
    }

    if (!points_.isEmpty() && input.key == Qt::Key_S) {
        beginNumericInput(NumericInput::SideCount);
        updateStatus(context);
        publish(context);
        return true;
    }
    if (!points_.isEmpty() &&
        ((mode() == PolygonMode::CenterCorner && input.key == Qt::Key_R) ||
         (mode() == PolygonMode::CenterTangent && input.key == Qt::Key_A) ||
         ((mode() == PolygonMode::CornerCorner || mode() == PolygonMode::Edge) &&
          input.key == Qt::Key_L))) {
        beginNumericInput(NumericInput::Size);
        updateStatus(context);
        publish(context);
        return true;
    }

    if (!points_.isEmpty() && isNumericStart(input.text) &&
        !(input.modifiers & (Qt::ControlModifier | Qt::AltModifier |
                             Qt::MetaModifier))) {
        beginNumericInput(NumericInput::Size, input.text);
        updateStatus(context);
        publish(context);
        return true;
    }

    return false;
}

void PolygonTool::cancel(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview PolygonTool::preview() const
{
    ToolPreview result;
    result.points = points_;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = planeLocked_ || frameCaptured_;
    result.hasCursorPoint = hasCursorPoint_ && result.hasWorkPlaneFrame;
    result.cursorPoint = result.hasCursorPoint ? cursorPoint_ : QPointF();
    result.cursorVisible = result.hasCursorPoint;
    result.statusText = status_.text;
    result.hudDimensionsLine = hudDimensionsLine_;
    result.hudInstructionsLine = hudInstructionsLine_;

    const QVector<QPointF> vertices = polygonVertices();
    if (vertices.size() >= 3) {
        result.shape = makePolygonShape(vertices);
        result.hasShape = validateNurbsCurve(result.shape.nurbs);
    }
    return result;
}

ToolStatus PolygonTool::status() const
{
    return status_;
}

PolygonMode PolygonTool::mode() const
{
    return polygonModeForTool(tool_);
}

Point3D PolygonTool::cursorWorldPoint(const ToolInput &input,
                                      const ToolContext &context) const
{
    const WorkPlaneFrame &eventFrame = isValidWorkPlaneFrame(input.workPlaneFrame)
                                           ? input.workPlaneFrame
                                           : drawingFrame_;
    const Point3D eventWorld = workPlaneFramePointToWorld(input.worldPosition,
                                                           eventFrame);
    if (input.snapType != SnapType::None || input.orthoEnabled ||
        !isValidWorkPlaneFrame(referenceFrame_)) {
        return eventWorld;
    }

    // ToolInput positions are local to the frame active when the viewport
    // created the event. While P changes the drawing frame, keep resolving an
    // unsnapped mouse ray on the captured reference plane.
    QPointF referencePoint;
    if (context.viewportTransform().screenToWorkPlane(input.screenPosition,
                                                       input.viewportSize,
                                                       referenceFrame_,
                                                       &referencePoint)) {
        return workPlaneFramePointToWorld(referencePoint, referenceFrame_);
    }
    return eventWorld;
}

Point3D PolygonTool::constrainedCursorWorld(const ToolInput &input,
                                           const ToolContext &context) const
{
    Point3D target = cursorWorldPoint(input, context);
    if (points_.isEmpty() || input.modifiers.testFlag(Qt::AltModifier) ||
        input.snapType != SnapType::None) {
        return target;
    }

    QPointF anchorScreen;
    if (!context.viewportTransform().worldPointToScreenUnclipped(
            anchorWorld_, input.viewportSize, &anchorScreen)) {
        return target;
    }
    const QPointF screenDelta = input.screenPosition - anchorScreen;
    const qreal screenMagnitude = length(screenDelta);
    if (screenMagnitude <= kEpsilon) {
        return target;
    }

    Point3D bestAxis;
    qreal bestAlignment = kAxisSnapCosine;
    for (const Point3D axis : {Point3D{1.0, 0.0, 0.0},
                               Point3D{0.0, 1.0, 0.0},
                               Point3D{0.0, 0.0, 1.0}}) {
        QPointF axisScreen;
        const bool projected =
            context.viewportTransform().worldPointToScreenUnclipped(
                addPoint(anchorWorld_, axis), input.viewportSize, &axisScreen) ||
            context.viewportTransform().worldPointToScreenUnclipped(
                subtractPoint(anchorWorld_, axis), input.viewportSize, &axisScreen);
        if (!projected) {
            continue;
        }
        const QPointF screenAxis = axisScreen - anchorScreen;
        const qreal axisScreenLength = length(screenAxis);
        if (axisScreenLength <= kEpsilon) {
            continue;
        }
        const qreal alignment = std::abs(
            QPointF::dotProduct(screenDelta, screenAxis) /
            (screenMagnitude * axisScreenLength));
        if (alignment >= bestAlignment) {
            bestAlignment = alignment;
            bestAxis = axis;
        }
    }

    Point3D inferred;
    if (lengthPoint(bestAxis) > kEpsilon &&
        context.viewportTransform().screenToWorldAxis(input.screenPosition,
                                                      input.viewportSize,
                                                      anchorWorld_,
                                                      bestAxis,
                                                      &inferred)) {
        target = inferred;
    }
    return target;
}

void PolygonTool::updateDrawingFrame(const Point3D &targetWorld,
                                    ToolContext &context)
{
    if (!isValidWorkPlaneFrame(referenceFrame_) || points_.isEmpty()) {
        return;
    }

    const Point3D referenceNormal = normalizedPoint(referenceFrame_.normal);
    const Point3D bridge = subtractPoint(targetWorld, anchorWorld_);
    const qreal bridgeLength = lengthPoint(bridge);
    if (bridgeLength <= kEpsilon) {
        return;
    }

    const Point3D direction = scalePoint(bridge, 1.0 / bridgeLength);
    const bool vertical = std::abs(dotPoint(direction, referenceNormal)) >
                          (wasVertical_ ? 0.98 : 0.995);
    wasVertical_ = vertical;
    if (!perpendicularMode_ && !vertical) {
        drawingFrame_ = referenceFrame_;
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        return;
    }

    const Point3D viewDirection =
        normalizedPoint(context.viewportTransform().viewDirection());
    Point3D frameNormal;
    Point3D preferredXAxis;
    if (vertical) {
        const Point3D worldX{1.0, 0.0, 0.0};
        const Point3D worldY{0.0, 1.0, 0.0};
        const Point3D basisReference =
            std::abs(dotPoint(referenceNormal, worldX)) < 0.99
                ? worldX
                : worldY;
        const Point3D verticalY =
            normalizedPoint(crossPoint(referenceNormal, basisReference));
        const Point3D verticalX =
            normalizedPoint(crossPoint(verticalY, referenceNormal));
        if (verticalOverrideAxis_ == Qt::Key_X) {
            frameNormal = verticalX;
        } else if (verticalOverrideAxis_ == Qt::Key_Y) {
            frameNormal = verticalY;
        } else {
            frameNormal = std::abs(dotPoint(viewDirection, verticalX)) >
                                  std::abs(dotPoint(viewDirection, verticalY))
                              ? verticalX
                              : verticalY;
        }
        frameNormal = normalizedPoint(frameNormal);
        if (dotPoint(frameNormal, viewDirection) > 0.0) {
            frameNormal = scalePoint(frameNormal, -1.0);
        }
        preferredXAxis = crossPoint(referenceNormal, frameNormal);
    } else {
        frameNormal = normalizedPoint(crossPoint(direction, referenceNormal));
        if (dotPoint(frameNormal, viewDirection) > 0.0) {
            frameNormal = scalePoint(frameNormal, -1.0);
        }
        preferredXAxis = direction;
    }

    const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
        anchorWorld_, frameNormal, preferredXAxis);
    if (isValidWorkPlaneFrame(frame)) {
        drawingFrame_ = frame;
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
    }
}

QVector<QPointF> PolygonTool::polygonVertices() const
{
    if (points_.size() != 1 || !hasCursorPoint_) {
        return {};
    }
    QVector<QPointF> definition = points_;
    definition.append(cursorPoint_);
    QVector<QPointF> vertices = makeRegularPolygonPoints(mode(),
                                                         definition,
                                                         sideCount_);
    return vertices;
}

Shape PolygonTool::makePolygonShape(const QVector<QPointF> &vertices) const
{
    Shape shape;
    shape.geometryType = GeometryType::Polygon;
    shape.points = vertices;
    QVector<QPointF> closedPoints = vertices;
    closedPoints.append(vertices.first());
    shape.nurbs = makeDegreeOneNurbs(closedPoints);
    setPolygonWorkPlaneFrame(&shape, drawingFrame_);
    return shape;
}

void PolygonTool::updateFromInput(const ToolInput &input, ToolContext &context)
{
    if (!frameCaptured_) {
        hasCursorPoint_ = true;
        cursorPoint_ = input.worldPosition;
        return;
    }
    if (numericPointLocked_) {
        return;
    }
    const Point3D targetWorld = constrainedCursorWorld(input, context);
    updateDrawingFrame(targetWorld, context);
    points_[0] = worldPointToWorkPlaneFrame(anchorWorld_, drawingFrame_);
    cursorPoint_ = worldPointToWorkPlaneFrame(
        projectPointToFrame(targetWorld, drawingFrame_), drawingFrame_);
    hasCursorPoint_ = true;
}

void PolygonTool::applyNumericInput(ToolContext &context)
{
    if (numericText_.isEmpty()) {
        numericInput_ = NumericInput::None;
        updateStatus(context);
        publish(context);
        return;
    }

    if (numericInput_ == NumericInput::SideCount) {
        bool valid = false;
        const int requested = numericText_.toInt(&valid);
        if (!valid) {
            status_.text = QStringLiteral("Polygon: enter a whole number of sides");
            hudInstructionsLine_ = status_.text;
            publish(context);
            return;
        }
        setSideCount(std::clamp(requested, 3, 1000), context);
    } else {
        qreal valueMillimeters = 0.0;
        if (!parseDocumentLengthInput(numericText_,
                                      context.document().settings().lengthUnit,
                                      &valueMillimeters)) {
            status_.text = QStringLiteral("Polygon: enter a valid length");
            hudInstructionsLine_ = status_.text;
            publish(context);
            return;
        }

        const QPointF anchor = points_.isEmpty() ? QPointF() : points_.first();
        QPointF direction = cursorPoint_ - anchor;
        if (length(direction) <= kEpsilon) {
            const QPointF fallback = worldPointToWorkPlaneFrame(
                constrainedCursorWorld(lastInput_, context), drawingFrame_);
            direction = fallback - anchor;
        }
        if (length(direction) <= kEpsilon) {
            direction = QPointF(1.0, 0.0);
        } else {
            direction /= length(direction);
        }
        cursorPoint_ = anchor + direction * std::abs(valueMillimeters);
        hasCursorPoint_ = true;
        numericPointLocked_ = true;
    }

    numericInput_ = NumericInput::None;
    numericText_.clear();
    updateStatus(context);
    publish(context);
}

void PolygonTool::setSideCount(int sideCount,
                               ToolContext &context,
                               bool fromWheel)
{
    const int maximum = fromWheel ? 256 : 1000;
    sideCount_ = effectiveSideCount(std::clamp(sideCount, 3, maximum));
    context.setPolygonSideCount(sideCount_);
}

int PolygonTool::effectiveSideCount(int sideCount) const
{
    sideCount = std::clamp(sideCount, 3, 1001);
    if (mode() == PolygonMode::Edge && sideCount % 2 == 0) {
        ++sideCount;
    } else if (mode() != PolygonMode::Edge && sideCount > 1000) {
        --sideCount;
    }
    return sideCount;
}

void PolygonTool::updateStatus(const ToolContext &context)
{
    const DocumentSettings settings = context.document().settings();
    const qreal unitsPerMillimeter =
        1.0 / millimetersPerDocumentUnit(settings.lengthUnit);
    const QString suffix = lengthUnitSuffix(settings.lengthUnit);
    const auto formatLength = [&](qreal value) {
        return QStringLiteral("%1 %2")
            .arg(value * unitsPerMillimeter, 0, 'f', 3)
            .arg(suffix);
    };

    const qreal currentSize = points_.isEmpty() || !hasCursorPoint_
                                  ? 0.0
                                  : length(cursorPoint_ - points_.first());
    QString sizeLabel;
    switch (mode()) {
    case PolygonMode::CenterCorner:
        sizeLabel = QStringLiteral("R");
        break;
    case PolygonMode::CenterTangent:
        sizeLabel = QStringLiteral("A");
        break;
    case PolygonMode::CornerCorner:
    case PolygonMode::Edge:
        sizeLabel = QStringLiteral("L");
        break;
    }

    if (numericInput_ == NumericInput::SideCount) {
        hudDimensionsLine_ = QStringLiteral("Sides: %1|    Size: %2")
                                 .arg(numericText_, formatLength(currentSize));
    } else if (numericInput_ == NumericInput::Size) {
        hudDimensionsLine_ = QStringLiteral("%1: %2|    Sides: %3")
                                 .arg(sizeLabel, numericText_)
                                 .arg(sideCount_);
    } else if (points_.isEmpty()) {
        hudDimensionsLine_ = QStringLiteral("%1    Sides: %2")
                                 .arg(toolName(tool_))
                                 .arg(sideCount_);
    } else {
        hudDimensionsLine_ = QStringLiteral("%1: %2    Sides: %3")
                                 .arg(sizeLabel, formatLength(currentSize))
                                 .arg(sideCount_);
    }

    if (points_.isEmpty()) {
        hudInstructionsLine_ = planeLocked_
                                   ? QStringLiteral("Click first point  •  L unlocks plane  •  Esc exits")
                                   : QStringLiteral("Click first point  •  L locks plane  •  Esc exits");
    } else if (numericInput_ != NumericInput::None) {
        hudInstructionsLine_ = QStringLiteral("Type a value  •  Enter applies  •  Esc exits");
    } else {
        const QString sizingKey = mode() == PolygonMode::CenterCorner
                                      ? QStringLiteral("R radius")
                                  : mode() == PolygonMode::CenterTangent
                                      ? QStringLiteral("A apothem")
                                      : QStringLiteral("L length");
        hudInstructionsLine_ = QStringLiteral("Click to finish  •  %1  •  S sides  •  Scroll sides  •  P perp  •  Alt bypass")
                                   .arg(sizingKey);
        if (perpendicularMode_) {
            hudInstructionsLine_ += QStringLiteral("  •  Perp ON");
        }
        if (wasVertical_ && verticalOverrideAxis_ != 0) {
            const QString overrideName = verticalOverrideAxis_ == Qt::Key_X
                                             ? QStringLiteral("X")
                                             : QStringLiteral("Y");
            hudInstructionsLine_ += QStringLiteral("  •  Vertical plane: %1")
                                        .arg(overrideName);
        }
        hudInstructionsLine_ += QStringLiteral("  •  Esc exits");
    }

    status_.state = ToolLifecycleState::Active;
    status_.text = hudInstructionsLine_;
    status_.canCommit = polygonVertices().size() >= 3;
}

void PolygonTool::beginNumericInput(NumericInput input,
                                    const QString &initialText)
{
    numericInput_ = input;
    numericText_ = initialText;
}

void PolygonTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

void PolygonTool::finish(ToolContext &context)
{
    const QVector<QPointF> vertices = polygonVertices();
    if (vertices.size() < 3) {
        status_.text = QStringLiteral("Polygon: choose a valid size");
        hudInstructionsLine_ = status_.text;
        publish(context);
        return;
    }

    const Shape shape = makePolygonShape(vertices);
    QString error;
    if (!validateNurbsCurve(shape.nurbs, &error)) {
        status_.text = QStringLiteral("Polygon: curve definition is invalid");
        hudInstructionsLine_ = status_.text;
        publish(context);
        return;
    }
    if (context.commitShape(tool_, shape)) {
        status_.state = ToolLifecycleState::Completed;
        status_.text = QStringLiteral("%1 committed").arg(toolName(tool_));
        points_.clear();
        hasCursorPoint_ = false;
        publish(context);
        context.finishTool(ToolId::Select);
    }
}

void PolygonTool::reset()
{
    points_.clear();
    drawingFrame_ = {};
    referenceFrame_ = {};
    frameBeforeLock_ = {};
    lastInput_ = {};
    anchorWorld_ = {};
    cursorPoint_ = {};
    numericText_.clear();
    numericInput_ = NumericInput::None;
    wheelRemainder_ = 0;
    planeAxisLockKey_ = 0;
    verticalOverrideAxis_ = 0;
    hasCursorPoint_ = false;
    frameCaptured_ = false;
    planeLocked_ = false;
    manualPlaneLock_ = false;
    perpendicularMode_ = false;
    wasVertical_ = false;
    numericPointLocked_ = false;
    hudDimensionsLine_.clear();
    hudInstructionsLine_.clear();
}

} // namespace classiCAD
