#include "ellipse_tool.h"

#include "core/document/document_settings.h"
#include "tool_context.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qreal kEpsilon = 1.0e-9;
constexpr qreal kAxisSnapCosine = 0.9945218953682733; // cos(6 degrees)

Point3D add(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D scale(const Point3D &point, qreal value)
{
    return {point.x * value, point.y * value, point.z * value};
}

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D cross(const Point3D &first, const Point3D &second)
{
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

qreal length(const Point3D &point)
{
    return std::sqrt(dot(point, point));
}

Point3D normalized(const Point3D &point)
{
    const qreal magnitude = length(point);
    return magnitude > kEpsilon ? scale(point, 1.0 / magnitude) : Point3D{};
}

bool finite(const Point3D &point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z);
}

Point3D projectToPlane(const Point3D &point, const WorkPlaneFrame &frame)
{
    if (!isValidWorkPlaneFrame(frame) || !finite(point)) {
        return {};
    }
    const Point3D normal = normalized(frame.normal);
    return subtract(point,
                    scale(normal,
                          dot(subtract(point, frame.origin), normal)));
}

Point3D pointFromFrame(const QPointF &point, const WorkPlaneFrame &frame)
{
    return workPlaneFramePointToWorld(point, frame);
}

QColor axisGuideColor(const Point3D &direction)
{
    const Point3D unit = normalized(direction);
    if (length(unit) <= kEpsilon) {
        return QColor(128, 128, 128, 180);
    }
    if (std::abs(unit.x) > 0.9999) return QColor(255, 26, 26);
    if (std::abs(unit.y) > 0.9999) return QColor(26, 179, 26);
    if (std::abs(unit.z) > 0.9999) return QColor(51, 128, 255);
    return QColor(128, 128, 128, 180);
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

} // namespace

EllipseTool::EllipseTool(ToolId tool)
    : tool_(tool)
{
}

ToolId EllipseTool::id() const
{
    return tool_;
}

void EllipseTool::begin(ToolContext &context)
{
    reset();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    status_.state = ToolLifecycleState::Active;
    updateStatus(context);
    publish(context);
}

bool EllipseTool::handleMousePress(const ToolInput &input, ToolContext &context)
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
        if (isCornersMode()) {
            const Point3D worldUp{0.0, 0.0, 1.0};
            if (std::abs(dot(normalized(referenceFrame_.normal), worldUp)) > 0.99) {
                const Point3D worldX{1.0, 0.0, 0.0};
                initialYAxis_ = normalized(cross(referenceFrame_.normal, worldX));
                initialXAxis_ = normalized(cross(initialYAxis_,
                                                  referenceFrame_.normal));
            } else {
                initialXAxis_ = referenceFrame_.xAxis;
                initialYAxis_ = referenceFrame_.yAxis;
            }
            const WorkPlaneFrame initialFrame = makeWorkPlaneFrameFromNormal(
                referenceFrame_.origin, referenceFrame_.normal, initialXAxis_);
            if (isValidWorkPlaneFrame(initialFrame)) {
                referenceFrame_ = initialFrame;
                drawingFrame_ = initialFrame;
            }
        }
        frameCaptured_ = true;
        planeLocked_ = true;
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        cursorWorld_ = projectedCursorPoint(input, referenceFrame_, context);
        hasCursorPoint_ = true;
        points_.append(cursorWorld_);
        if (!isCornersMode()) {
            initialXAxis_ = referenceFrame_.xAxis;
            initialYAxis_ = referenceFrame_.yAxis;
        }
        majorAxisWorld_ = referenceFrame_.xAxis;
        updateStatus(context);
        publish(context);
        return true;
    }

    if (!numericPointLocked_) {
        updateFromInput(input, context);
    } else {
        lastInput_ = input;
    }
    numericPointLocked_ = false;
    if (isCornersMode()) {
        finish(context);
        return true;
    }

    if (points_.size() == 1) {
        if (length(subtract(cursorWorld_, points_.first())) <= kEpsilon) {
            status_.text = QStringLiteral("%1: choose a point away from the first point")
                               .arg(toolName(tool_));
            hudInstructionsLine_ = status_.text;
            publish(context);
            return true;
        }
        points_.append(cursorWorld_);
        majorAxisWorld_ = normalized(subtract(points_[1], points_[0]));
        majorRadius_ = isRadiusMode()
                           ? length(subtract(points_[1], points_[0]))
                           : length(subtract(points_[1], points_[0])) * 0.5;
        updateEllipseFrame(points_[1], context);
        updateStatus(context);
        publish(context);
        return true;
    }

    if (points_.size() == 2) {
        QVector<QPointF> definition;
        QPointF center;
        if (!ellipseDefinition(&definition, &center)) {
            status_.text = isFociMode()
                               ? QStringLiteral("Ellipse: move the point outside the focal segment")
                               : QStringLiteral("Ellipse: choose a nonzero minor radius");
            hudInstructionsLine_ = status_.text;
            publish(context);
            return true;
        }
        const Shape shape = makeEllipseShape(definition, center);
        if (context.commitShape(tool_, shape)) {
            status_.state = ToolLifecycleState::Completed;
            status_.text = QStringLiteral("%1 committed").arg(toolName(tool_));
            publish(context);
            context.finishTool(ToolId::Select);
        }
        return true;
    }

    return true;
}

bool EllipseTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    lastInput_ = input;
    numericPointLocked_ = false;
    if (points_.isEmpty() && !planeLocked_ &&
        isValidWorkPlaneFrame(input.workPlaneFrame)) {
        drawingFrame_ = input.workPlaneFrame;
    }
    updateFromInput(input, context);
    updateStatus(context);
    publish(context);
    return true;
}

bool EllipseTool::handleKey(const ToolInput &input, ToolContext &context)
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

    if (input.key == Qt::Key_L) {
        if (points_.isEmpty()) {
            if (planeLocked_) {
                drawingFrame_ = isValidWorkPlaneFrame(frameBeforeLock_)
                                    ? frameBeforeLock_
                                    : context.viewportTransform().workPlaneFrame();
                planeLocked_ = false;
            } else if (isValidWorkPlaneFrame(input.workPlaneFrame)) {
                frameBeforeLock_ = drawingFrame_;
                drawingFrame_ = input.workPlaneFrame;
                planeLocked_ = true;
                context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
            }
        }
        updateStatus(context);
        publish(context);
        return true;
    }

    if (input.key == Qt::Key_P && !points_.isEmpty()) {
        perpendicularMode_ = !perpendicularMode_;
        verticalOverrideAxis_ = 0;
        updateFromInput(input, context);
        updateStatus(context);
        publish(context);
        return true;
    }

    if ((input.key == Qt::Key_X || input.key == Qt::Key_Y) &&
        !points_.isEmpty()) {
        verticalOverrideAxis_ = input.key;
        updateFromInput(input, context);
        updateStatus(context);
        publish(context);
        return true;
    }

    if (isFociMode() && input.key == Qt::Key_K) {
        keepFoci_ = !keepFoci_;
        updateStatus(context);
        publish(context);
        return true;
    }

    if (input.key == Qt::Key_D && !isCornersMode() && points_.size() == 1 &&
        (isRadiusMode() || isEndpointsMode())) {
        beginNumericInput(NumericInput::MajorDiameter);
        updateStatus(context);
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_R && !isCornersMode() && points_.size() == 2 &&
        (isRadiusMode() || isEndpointsMode())) {
        beginNumericInput(NumericInput::MinorRadius);
        updateStatus(context);
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_F && isFociMode() && points_.size() == 1) {
        beginNumericInput(NumericInput::FocusSpacing);
        updateStatus(context);
        publish(context);
        return true;
    }

    const bool plainTextInput =
        !(input.modifiers & (Qt::ControlModifier | Qt::AltModifier |
                             Qt::MetaModifier));
    if (!points_.isEmpty() && plainTextInput && canStartLengthInput(input.text)) {
        NumericInput requested = NumericInput::None;
        if (points_.size() == 1 && isFociMode()) {
            requested = NumericInput::FocusSpacing;
        } else if (points_.size() == 1 &&
                   (isRadiusMode() || isEndpointsMode())) {
            requested = NumericInput::MajorDiameter;
        } else if (points_.size() == 2 &&
                   (isRadiusMode() || isEndpointsMode())) {
            requested = NumericInput::MinorRadius;
        }
        if (requested != NumericInput::None) {
            beginNumericInput(requested, input.text);
            updateStatus(context);
            publish(context);
            return true;
        }
    }
    return false;
}

void EllipseTool::cancel(ToolContext &context)
{
    reset();
    status_.state = ToolLifecycleState::Cancelled;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview EllipseTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = planeLocked_ || frameCaptured_;
    result.hasCursorPoint = hasCursorPoint_ && result.hasWorkPlaneFrame;
    result.cursorPoint = result.hasCursorPoint
                             ? worldPointToWorkPlaneFrame(cursorWorld_, drawingFrame_)
                             : QPointF();
    result.statusText = status_.text;
    result.hudDimensionsLine = hudDimensionsLine_;
    result.hudInstructionsLine = hudInstructionsLine_;
    result.points.reserve(points_.size());
    for (const Point3D &point : points_) {
        result.points.append(worldPointToWorkPlaneFrame(point, drawingFrame_));
    }

    const auto addGuide = [&](const Point3D &first,
                              const Point3D &second,
                              const QColor &color,
                              bool dashed = false) {
        result.guides.append({QLineF(worldPointToWorkPlaneFrame(first, drawingFrame_),
                                     worldPointToWorkPlaneFrame(second, drawingFrame_)),
                              color,
                              dashed});
    };
    if (hasCursorPoint_ && !points_.isEmpty() && result.hasWorkPlaneFrame) {
        const Point3D center = isEndpointsMode() && points_.size() >= 2
                                   ? scale(add(points_[0], points_[1]), 0.5)
                               : isFociMode() && points_.size() >= 2
                                   ? scale(add(points_[0], points_[1]), 0.5)
                                   : points_.first();
        const Point3D axisX = isValidWorkPlaneFrame(drawingFrame_)
                                  ? drawingFrame_.xAxis
                                  : majorAxisWorld_;
        const Point3D axisY = isValidWorkPlaneFrame(drawingFrame_)
                                  ? drawingFrame_.yAxis
                                  : initialYAxis_;
        if (isCornersMode()) {
            const Point3D delta = subtract(cursorWorld_, points_.first());
            const qreal width = dot(delta, axisX);
            const qreal height = dot(delta, axisY);
            const Point3D cornerX = add(points_.first(), scale(axisX, width));
            const Point3D opposite = add(cornerX, scale(axisY, height));
            const Point3D cornerY = add(points_.first(), scale(axisY, height));
            const QColor boxColor(0, 210, 85);
            addGuide(points_.first(), cornerX, boxColor);
            addGuide(cornerX, opposite, boxColor);
            addGuide(opposite, cornerY, boxColor);
            addGuide(cornerY, points_.first(), boxColor);
        } else if (isRadiusMode() && points_.size() == 1) {
            const Point3D left = subtract(center, scale(axisX, majorRadius_));
            const Point3D right = add(center, scale(axisX, majorRadius_));
            addGuide(left, right, axisGuideColor(axisX));
        } else if (isEndpointsMode()) {
            const Point3D endpoint = points_.size() >= 2
                                         ? points_[1]
                                         : cursorWorld_;
            addGuide(points_.first(), endpoint,
                     axisGuideColor(subtract(endpoint, points_.first())));
        } else if (isFociMode()) {
            const Point3D focus = points_.size() >= 2
                                      ? points_[1]
                                      : cursorWorld_;
            addGuide(points_.first(), focus,
                     axisGuideColor(subtract(focus, points_.first())));
        }

        if (points_.size() >= 2 && !isCornersMode()) {
            if (isRadiusMode()) {
                const Point3D left = subtract(center, scale(axisX, majorRadius_));
                const Point3D right = add(center, scale(axisX, majorRadius_));
                addGuide(left, right, axisGuideColor(axisX));
                addGuide(center, cursorWorld_, axisGuideColor(axisY));
            } else if (isEndpointsMode()) {
                addGuide(points_[0], points_[1],
                         axisGuideColor(subtract(points_[1], points_[0])));
                addGuide(center, cursorWorld_, axisGuideColor(axisY));
            } else if (isFociMode()) {
                const QColor focusColor(0, 220, 90);
                addGuide(points_[0], cursorWorld_, focusColor);
                addGuide(points_[1], cursorWorld_, focusColor);
            }
        }
    }

    QVector<QPointF> definition;
    QPointF center;
    if (result.hasCursorPoint && ellipseDefinition(&definition, &center)) {
        result.shape = makeEllipseShape(definition, center);
        result.hasShape = validateNurbsCurve(result.shape.nurbs);
    }
    return result;
}

ToolStatus EllipseTool::status() const
{
    return status_;
}

EllipseMode EllipseTool::mode() const
{
    return ellipseModeForTool(tool_);
}

int EllipseTool::constructionStage() const
{
    return points_.size();
}

int EllipseTool::requiredPointCount() const
{
    return requiredPoints(tool_);
}

bool EllipseTool::isRadiusMode() const
{
    return mode() == EllipseMode::CenterAxisRadius;
}

bool EllipseTool::isEndpointsMode() const
{
    return mode() == EllipseMode::AxisEndpoints;
}

bool EllipseTool::isCornersMode() const
{
    return mode() == EllipseMode::Corners;
}

bool EllipseTool::isFociMode() const
{
    return mode() == EllipseMode::FociPoint;
}

Point3D EllipseTool::inputWorldPoint(const ToolInput &input,
                                    const WorkPlaneFrame &frame,
                                    const ToolContext &context) const
{
    Q_UNUSED(context);
    const WorkPlaneFrame inputFrame = isValidWorkPlaneFrame(input.workPlaneFrame)
                                          ? input.workPlaneFrame
                                          : frame;
    if (!isValidWorkPlaneFrame(inputFrame) ||
        !std::isfinite(input.worldPosition.x()) ||
        !std::isfinite(input.worldPosition.y())) {
        return {};
    }
    return input.snapResult.isValid() && input.snapResult.hasWorldPoint
               ? input.resolvedWorldPoint()
               : pointFromFrame(input.worldPosition, inputFrame);
}

Point3D EllipseTool::projectedCursorPoint(const ToolInput &input,
                                          const WorkPlaneFrame &frame,
                                          const ToolContext &context) const
{
    if (!isValidWorkPlaneFrame(frame)) {
        return {};
    }

    if (input.snapType != SnapType::None || input.orthoEnabled) {
        return projectToPlane(inputWorldPoint(input, frame, context), frame);
    }

    QPointF projected;
    if (context.viewportTransform().screenToWorkPlane(input.screenPosition,
                                                       input.viewportSize,
                                                       frame,
                                                       &projected)) {
        return pointFromFrame(projected, frame);
    }
    return projectToPlane(inputWorldPoint(input, frame, context), frame);
}

Point3D EllipseTool::resolveMajorPoint(const ToolInput &input,
                                      const ToolContext &context) const
{
    Point3D target = projectedCursorPoint(input, referenceFrame_, context);
    if (input.snapType != SnapType::None ||
        input.modifiers.testFlag(Qt::AltModifier) || points_.isEmpty()) {
        return target;
    }

    const Point3D origin = points_.first();
    QPointF originScreen;
    if (!context.viewportTransform().worldPointToScreenUnclipped(
            origin, input.viewportSize, &originScreen)) {
        return target;
    }
    const QPointF screenDelta = input.screenPosition - originScreen;
    const qreal screenLength = std::hypot(screenDelta.x(), screenDelta.y());
    if (screenLength < 1.0) {
        return target;
    }
    const QPointF mouseDirection = screenDelta / screenLength;

    Point3D bestAxis;
    qreal bestAlignment = kAxisSnapCosine;
    for (const Point3D worldAxis : {Point3D{1.0, 0.0, 0.0},
                                    Point3D{0.0, 1.0, 0.0},
                                    Point3D{0.0, 0.0, 1.0}}) {
        const Point3D projected = subtract(
            worldAxis,
            scale(referenceFrame_.normal,
                  dot(worldAxis, referenceFrame_.normal)));
        const Point3D axis = normalized(projected);
        if (length(axis) <= kEpsilon) {
            continue;
        }
        QPointF axisScreen;
        if (!context.viewportTransform().worldPointToScreenUnclipped(
                add(origin, axis), input.viewportSize, &axisScreen) &&
            !context.viewportTransform().worldPointToScreenUnclipped(
                subtract(origin, axis), input.viewportSize, &axisScreen)) {
            continue;
        }
        const QPointF screenAxis = axisScreen - originScreen;
        const qreal screenAxisLength = std::hypot(screenAxis.x(), screenAxis.y());
        if (screenAxisLength <= kEpsilon) {
            continue;
        }
        const QPointF unitScreenAxis = screenAxis / screenAxisLength;
        const qreal alignment = std::abs(QPointF::dotProduct(mouseDirection,
                                                              unitScreenAxis));
        if (alignment >= bestAlignment) {
            bestAlignment = alignment;
            bestAxis = axis;
        }
    }
    if (length(bestAxis) > kEpsilon) {
        Point3D inferred;
        if (context.viewportTransform().screenToWorldAxis(input.screenPosition,
                                                          input.viewportSize,
                                                          origin,
                                                          bestAxis,
                                                          &inferred)) {
            return projectToPlane(inferred, referenceFrame_);
        }
    }
    return target;
}

void EllipseTool::updateFromInput(const ToolInput &input, ToolContext &context)
{
    if (!isValidWorkPlaneFrame(drawingFrame_)) {
        drawingFrame_ = isValidWorkPlaneFrame(input.workPlaneFrame)
                            ? input.workPlaneFrame
                            : context.viewportTransform().workPlaneFrame();
    }
    if (!points_.isEmpty() && !isValidWorkPlaneFrame(referenceFrame_)) {
        referenceFrame_ = drawingFrame_;
    }

    if (points_.isEmpty()) {
        if (isValidWorkPlaneFrame(input.workPlaneFrame) && !planeLocked_) {
            drawingFrame_ = input.workPlaneFrame;
        }
        cursorWorld_ = projectedCursorPoint(input, drawingFrame_, context);
        hasCursorPoint_ = finite(cursorWorld_);
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        return;
    }

    if (isCornersMode()) {
        const Point3D referenceTarget = projectedCursorPoint(input,
                                                             referenceFrame_,
                                                             context);
        updateEllipseFrame(referenceTarget, context);
        cursorWorld_ = perpendicularMode_
                           ? projectedCursorPoint(input, drawingFrame_, context)
                           : projectToPlane(referenceTarget, drawingFrame_);
        const Point3D delta = subtract(cursorWorld_, points_.first());
        majorRadius_ = std::abs(dot(delta, drawingFrame_.xAxis)) * 0.5;
        minorRadius_ = std::abs(dot(delta, drawingFrame_.yAxis)) * 0.5;
        hasCursorPoint_ = finite(cursorWorld_);
        return;
    }

    if (points_.size() == 1) {
        cursorWorld_ = resolveMajorPoint(input, context);
        updateEllipseFrame(cursorWorld_, context);
        majorAxisWorld_ = normalized(subtract(cursorWorld_, points_.first()));
        if (length(majorAxisWorld_) <= kEpsilon) {
            majorAxisWorld_ = drawingFrame_.xAxis;
        }
        majorRadius_ = length(subtract(cursorWorld_, points_.first()));
        if (isEndpointsMode()) {
            majorRadius_ *= 0.5;
        }
        hasCursorPoint_ = finite(cursorWorld_);
        return;
    }

    if (isValidWorkPlaneFrame(drawingFrame_)) {
        updateEllipseFrame(points_[1], context);
    }
    const Point3D target = projectedCursorPoint(input, drawingFrame_, context);
    if (isFociMode()) {
        cursorWorld_ = pointOnEllipse(target);
        const qreal focalDistance = length(subtract(points_[1], points_[0])) * 0.5;
        majorRadius_ = (length(subtract(target, points_[0])) +
                        length(subtract(target, points_[1]))) * 0.5;
        majorRadius_ = std::max(majorRadius_, focalDistance + 1.0e-6);
        minorRadius_ = std::sqrt(std::max<qreal>(
            0.0, majorRadius_ * majorRadius_ - focalDistance * focalDistance));
        hasCursorPoint_ = finite(cursorWorld_);
        return;
    }

    const Point3D center = isEndpointsMode()
                               ? scale(add(points_[0], points_[1]), 0.5)
                               : points_[0];
    const qreal minorOffset = dot(subtract(target, center), drawingFrame_.yAxis);
    minorRadius_ = std::abs(minorOffset);
    cursorWorld_ = add(center, scale(drawingFrame_.yAxis, minorOffset));
    hasCursorPoint_ = finite(cursorWorld_);
}

void EllipseTool::updateEllipseFrame(const Point3D &majorPoint,
                                    ToolContext &context)
{
    if (!isValidWorkPlaneFrame(referenceFrame_) || points_.isEmpty()) {
        return;
    }
    const Point3D origin = points_.first();
    if (isCornersMode()) {
        const Point3D referenceNormal = normalized(referenceFrame_.normal);
        const Point3D bridge = subtract(majorPoint, origin);
        const qreal bridgeLength = length(bridge);
        const bool vertical = bridgeLength > kEpsilon &&
            std::abs(dot(scale(bridge, 1.0 / bridgeLength), referenceNormal)) >
                (wasVertical_ ? 0.98 : 0.995);
        wasVertical_ = vertical;
        if (perpendicularMode_ || vertical) {
            Point3D planeNormal;
            if (vertical) {
                if (verticalOverrideAxis_ == Qt::Key_X) {
                    planeNormal = referenceFrame_.xAxis;
                } else if (verticalOverrideAxis_ == Qt::Key_Y) {
                    planeNormal = referenceFrame_.yAxis;
                } else {
                    const Point3D view = normalized(context.viewportTransform().viewDirection());
                    planeNormal = std::abs(dot(view, referenceFrame_.xAxis)) >
                                          std::abs(dot(view, referenceFrame_.yAxis))
                                      ? referenceFrame_.xAxis
                                      : referenceFrame_.yAxis;
                }
            } else {
                const Point3D view = normalized(context.viewportTransform().viewDirection());
                planeNormal = std::abs(dot(view, initialXAxis_)) >
                                      std::abs(dot(view, initialYAxis_))
                                  ? initialXAxis_
                                  : initialYAxis_;
            }
            planeNormal = normalized(planeNormal);
            if (dot(planeNormal, context.viewportTransform().viewDirection()) > 0.0) {
                planeNormal = scale(planeNormal, -1.0);
            }
            const Point3D preferredX = normalized(cross(referenceNormal, planeNormal));
            const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
                origin, planeNormal, preferredX);
            if (isValidWorkPlaneFrame(frame)) {
                setFrame(frame, context);
            }
        } else {
            const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
                origin, referenceNormal, initialXAxis_);
            if (isValidWorkPlaneFrame(frame)) {
                setFrame(frame, context);
            }
        }
        return;
    }

    const Point3D bridge = subtract(majorPoint, origin);
    Point3D majorDirection = normalized(bridge);
    if (length(majorDirection) <= kEpsilon) {
        return;
    }
    const Point3D referenceNormal = normalized(referenceFrame_.normal);
    const bool vertical =
        std::abs(dot(majorDirection, referenceNormal)) >
        (wasVertical_ ? 0.98 : 0.995);
    wasVertical_ = vertical;

    if (perpendicularMode_ || vertical) {
        const WorkPlaneFrame frame = makePerpendicularFrame(majorDirection, context);
        if (isValidWorkPlaneFrame(frame)) {
            setFrame(frame, context);
        }
    } else {
        const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
            origin, referenceNormal, majorDirection);
        if (isValidWorkPlaneFrame(frame)) {
            setFrame(frame, context);
        }
    }
}

WorkPlaneFrame EllipseTool::makePerpendicularFrame(
    const Point3D &majorDirection,
    const ToolContext &context) const
{
    WorkPlaneFrame frame;
    if (!isValidWorkPlaneFrame(referenceFrame_) || points_.isEmpty()) {
        return frame;
    }
    const Point3D referenceNormal = normalized(referenceFrame_.normal);
    const bool vertical =
        std::abs(dot(majorDirection, referenceNormal)) >
        (wasVertical_ ? 0.98 : 0.995);
    Point3D planeNormal;
    Point3D preferredX;
    if (vertical) {
        if (verticalOverrideAxis_ == Qt::Key_X) {
            planeNormal = referenceFrame_.xAxis;
        } else if (verticalOverrideAxis_ == Qt::Key_Y) {
            planeNormal = referenceFrame_.yAxis;
        } else {
            const Point3D view = normalized(context.viewportTransform().viewDirection());
            planeNormal = std::abs(dot(view, referenceFrame_.xAxis)) >
                                  std::abs(dot(view, referenceFrame_.yAxis))
                              ? referenceFrame_.xAxis
                              : referenceFrame_.yAxis;
        }
        const Point3D view = normalized(context.viewportTransform().viewDirection());
        if (dot(planeNormal, view) > 0.0) {
            planeNormal = scale(planeNormal, -1.0);
        }
        preferredX = cross(referenceNormal, planeNormal);
    } else {
        planeNormal = cross(majorDirection, referenceNormal);
        const Point3D view = normalized(context.viewportTransform().viewDirection());
        if (dot(planeNormal, view) > 0.0) {
            planeNormal = scale(planeNormal, -1.0);
        }
        preferredX = majorDirection;
    }
    return makeWorkPlaneFrameFromNormal(points_.first(),
                                        planeNormal,
                                        preferredX);
}

void EllipseTool::setFrame(const WorkPlaneFrame &frame, ToolContext &context)
{
    if (!isValidWorkPlaneFrame(frame)) {
        return;
    }
    drawingFrame_ = frame;
    context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
}

Point3D EllipseTool::pointOnEllipse(const Point3D &point) const
{
    if (points_.size() < 2 || !isValidWorkPlaneFrame(drawingFrame_)) {
        return point;
    }
    const Point3D firstFocus = points_[0];
    const Point3D secondFocus = points_[1];
    const Point3D center = scale(add(firstFocus, secondFocus), 0.5);
    const Point3D focusAxis = subtract(secondFocus, firstFocus);
    const qreal focalDistance = length(focusAxis) * 0.5;
    Point3D major = normalized(focusAxis);
    if (length(major) <= kEpsilon) {
        major = normalized(drawingFrame_.xAxis);
    }
    Point3D minor = normalized(cross(drawingFrame_.normal, major));
    if (length(minor) <= kEpsilon) {
        minor = normalized(drawingFrame_.yAxis);
    }
    const Point3D relative = subtract(projectToPlane(point, drawingFrame_), center);
    const qreal a = std::max<qreal>(
        (length(subtract(point, firstFocus)) +
         length(subtract(point, secondFocus))) * 0.5,
        focalDistance + 1.0e-6);
    const qreal b = std::sqrt(std::max<qreal>(0.0, a * a - focalDistance * focalDistance));
    const qreal angle = std::atan2(dot(relative, minor), dot(relative, major));
    return add(center,
               add(scale(major, a * std::cos(angle)),
                   scale(minor, b * std::sin(angle))));
}

bool EllipseTool::ellipseDefinition(QVector<QPointF> *definition,
                                   QPointF *center) const
{
    if (definition == nullptr || !hasCursorPoint_ ||
        !isValidWorkPlaneFrame(drawingFrame_)) {
        return false;
    }
    QVector<Point3D> worldDefinition = points_;
    if (worldDefinition.size() < requiredPointCount()) {
        worldDefinition.append(cursorWorld_);
    }
    if (worldDefinition.size() < requiredPointCount()) {
        return false;
    }

    definition->clear();
    definition->reserve(requiredPointCount());
    for (int i = 0; i < requiredPointCount(); ++i) {
        definition->append(worldPointToWorkPlaneFrame(worldDefinition[i],
                                                       drawingFrame_));
    }

    QPointF localCenter;
    if (isRadiusMode()) {
        localCenter = definition->first();
    } else {
        localCenter = (definition->first() + definition->at(1)) * 0.5;
    }
    if (center != nullptr) {
        *center = localCenter;
    }
    const Shape::NurbsCurve2D curve = makeEllipseNurbs(mode(), *definition);
    return validateNurbsCurve(curve);
}

Shape EllipseTool::makeEllipseShape(const QVector<QPointF> &definition,
                                   const QPointF &center) const
{
    Shape shape;
    shape.geometryType = GeometryType::Ellipse;
    shape.points = {center};
    setLegacyPlaneMetadata(&shape, drawingFrame_);
    shape.nurbs = makeEllipseNurbs(mode(), definition);
    return shape;
}

void EllipseTool::applyNumericInput(ToolContext &context)
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
        status_.text = QStringLiteral("Ellipse: enter a valid length");
        hudInstructionsLine_ = status_.text;
        publish(context);
        return;
    }
    const qreal value = std::abs(valueMillimeters);

    if (numericInput_ == NumericInput::MajorDiameter && points_.size() == 1) {
        const Point3D direction = normalized(drawingFrame_.xAxis);
        cursorWorld_ = add(points_.first(),
                           scale(direction,
                                 isRadiusMode() ? value * 0.5 : value));
        majorAxisWorld_ = direction;
        majorRadius_ = value * 0.5;
        points_.append(cursorWorld_);
        updateEllipseFrame(cursorWorld_, context);
    } else if (numericInput_ == NumericInput::FocusSpacing &&
               isFociMode() && points_.size() == 1) {
        Point3D direction = normalized(majorAxisWorld_);
        if (length(direction) <= kEpsilon) {
            direction = normalized(drawingFrame_.xAxis);
        }
        cursorWorld_ = add(points_.first(), scale(direction, value));
        majorAxisWorld_ = direction;
        points_.append(cursorWorld_);
        updateEllipseFrame(cursorWorld_, context);
    } else if (numericInput_ == NumericInput::MinorRadius &&
               points_.size() == 2 && (isRadiusMode() || isEndpointsMode())) {
        const Point3D center = isEndpointsMode()
                                   ? scale(add(points_[0], points_[1]), 0.5)
                                   : points_[0];
        cursorWorld_ = add(center, scale(drawingFrame_.yAxis, value));
        minorRadius_ = value;
        hasCursorPoint_ = true;
        numericPointLocked_ = true;
    }

    numericInput_ = NumericInput::None;
    numericText_.clear();
    updateStatus(context);
    publish(context);
}

void EllipseTool::beginNumericInput(NumericInput mode,
                                    const QString &initialText)
{
    numericInput_ = mode;
    numericText_ = initialText;
}

void EllipseTool::updateStatus(const ToolContext &context)
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

    QString dimensions;
    if (numericInput_ == NumericInput::MajorDiameter) {
        dimensions = QStringLiteral("D: %1| %2").arg(numericText_, suffix);
    } else if (numericInput_ == NumericInput::MinorRadius) {
        dimensions = QStringLiteral("R: %1| %2").arg(numericText_, suffix);
    } else if (numericInput_ == NumericInput::FocusSpacing) {
        dimensions = QStringLiteral("F: %1| %2").arg(numericText_, suffix);
    } else if (points_.isEmpty()) {
        dimensions = QStringLiteral("%1").arg(toolName(tool_));
    } else if (isRadiusMode()) {
        dimensions = points_.size() < 2
                         ? QStringLiteral("D: %1")
                               .arg(formatLength(hasCursorPoint_ ? majorRadius_ * 2.0 : 0.0))
                         : QStringLiteral("D: %1    R: %2")
                               .arg(formatLength(majorRadius_ * 2.0),
                                    formatLength(minorRadius_));
    } else if (isEndpointsMode()) {
        const qreal diameter = points_.size() >= 2
                                   ? length(subtract(points_[1], points_[0]))
                                   : (hasCursorPoint_ ? majorRadius_ * 2.0 : 0.0);
        dimensions = points_.size() < 2
                         ? QStringLiteral("D: %1").arg(formatLength(diameter))
                         : QStringLiteral("D: %1    R: %2")
                               .arg(formatLength(diameter),
                                    formatLength(minorRadius_));
    } else if (isFociMode()) {
        if (points_.size() < 2) {
            const qreal spacing = hasCursorPoint_ && !points_.isEmpty()
                                      ? length(subtract(cursorWorld_, points_.first()))
                                      : 0.0;
            dimensions = QStringLiteral("F: %1").arg(formatLength(spacing));
        } else {
            dimensions = QStringLiteral("Foci: %1    A: %2")
                             .arg(formatLength(length(subtract(points_[1], points_[0]))),
                                  formatLength(majorRadius_));
        }
    } else if (hasCursorPoint_ && !points_.isEmpty()) {
        const Point3D delta = subtract(cursorWorld_, points_.first());
        dimensions = QStringLiteral("W: %1    H: %2")
                         .arg(formatLength(std::abs(dot(delta, drawingFrame_.xAxis))),
                              formatLength(std::abs(dot(delta, drawingFrame_.yAxis))));
    } else {
        dimensions = QStringLiteral("Width: —    Height: —");
    }

    QString instructions;
    if (points_.isEmpty()) {
        instructions = QStringLiteral("Click first point  •  L locks plane");
    } else if (isRadiusMode() && points_.size() == 1) {
        instructions = QStringLiteral("Click major-axis radius  •  D diameter  •  P perp  •  Alt bypass");
    } else if (isEndpointsMode() && points_.size() == 1) {
        instructions = QStringLiteral("Click opposite axis endpoint  •  D diameter  •  P perp  •  Alt bypass");
    } else if (isFociMode() && points_.size() == 1) {
        instructions = QStringLiteral("Click second focus  •  F spacing  •  P perp  •  Alt bypass");
    } else if (isCornersMode()) {
        instructions = QStringLiteral("Click opposite corner  •  P perp");
    } else if (isFociMode()) {
        instructions = QStringLiteral("Click point on ellipse  •  P perp  •  K keep foci %1")
                           .arg(keepFoci_ ? QStringLiteral("ON")
                                          : QStringLiteral("OFF"));
    } else {
        instructions = QStringLiteral("Click minor radius  •  R radius  •  P perp  •  Alt bypass");
    }
    if (points_.size() >= 1 && verticalOverrideAxis_ != 0) {
        instructions += QStringLiteral("  •  %1 plane")
                            .arg(verticalOverrideAxis_ == Qt::Key_X
                                     ? QStringLiteral("X")
                                     : QStringLiteral("Y"));
    }
    if (perpendicularMode_ && !points_.isEmpty()) {
        instructions += QStringLiteral("  •  Perp ON");
    }
    instructions += QStringLiteral("  •  Esc exits");

    hudDimensionsLine_ = dimensions;
    hudInstructionsLine_ = instructions;
    status_.state = ToolLifecycleState::Active;
    status_.text = instructions;
    status_.canCommit = false;
}

void EllipseTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

void EllipseTool::finish(ToolContext &context)
{
    QVector<QPointF> definition;
    QPointF center;
    if (!ellipseDefinition(&definition, &center)) {
        status_.text = QStringLiteral("Ellipse: choose a valid width and height");
        hudInstructionsLine_ = status_.text;
        publish(context);
        return;
    }
    const Shape shape = makeEllipseShape(definition, center);
    if (!validateNurbsCurve(shape.nurbs)) {
        status_.text = QStringLiteral("Ellipse: curve definition is invalid");
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

void EllipseTool::reset()
{
    points_.clear();
    referenceFrame_ = {};
    drawingFrame_ = {};
    frameBeforeLock_ = {};
    lastInput_ = {};
    cursorWorld_ = {};
    majorAxisWorld_ = {};
    initialXAxis_ = {};
    initialYAxis_ = {};
    majorRadius_ = 0.0;
    minorRadius_ = 0.0;
    numericText_.clear();
    numericInput_ = NumericInput::None;
    verticalOverrideAxis_ = 0;
    axisConstraintKey_ = 0;
    hasCursorPoint_ = false;
    frameCaptured_ = false;
    planeLocked_ = false;
    perpendicularMode_ = false;
    keepFoci_ = false;
    wasVertical_ = false;
    numericPointLocked_ = false;
    hudDimensionsLine_.clear();
    hudInstructionsLine_.clear();
}

} // namespace classiCAD
