#include "tool_input_translator.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cmath>

namespace classiCAD {

ToolInput ToolInputTranslator::fromMouseEvent(
    const QMouseEvent &event,
    ToolId activeTool,
    const QPointF &screenPosition,
    const QPointF &rawWorldPosition,
    const QPointF &worldPosition,
    const WorkPlaneFrame &workPlaneFrame,
    bool orthoEnabled,
    const QSize &viewportSize,
    const SnapResult &snapResult,
    bool preserveWorldSnap)
{
    ToolInput input;
    input.screenPosition = screenPosition;
    input.rawWorldPosition = rawWorldPosition;
    input.worldPosition = worldPosition;
    input.workPlaneFrame = workPlaneFrame;
    input.orthoEnabled = orthoEnabled;
    input.viewportSize = viewportSize;
    input.snapResult = snapResult;
    input.snapType = snapResult.type;
    input.button = event.button();
    input.buttons = event.buttons();
    input.modifiers = event.modifiers();

    if (!preserveWorldSnap && activeTool != Tool::Line &&
        activeTool != Tool::PointEdgeCenter &&
        activeTool != Tool::PointByLine && activeTool != Tool::PointByArcs &&
        input.snapResult.isValid() && input.snapResult.hasWorldPoint &&
        isValidWorkPlaneFrame(workPlaneFrame)) {
        const QPointF snapPosition = worldPointToWorkPlaneFrame(
            input.snapResult.worldPoint, workPlaneFrame);
        if (std::hypot(snapPosition.x() - worldPosition.x(),
                       snapPosition.y() - worldPosition.y()) > 1.0e-7) {
            // A tool constraint may project or redirect the acquired snap.
            // Keep that constrained preview point authoritative instead of
            // letting a consumer restore the unconstrained target.
            input.snapResult = SnapResult{};
        }
    }
    return input;
}

ToolInput ToolInputTranslator::fromKeyEvent(
    const QKeyEvent &event,
    const QPointF &screenPosition,
    const QPointF &rawWorldPosition,
    const WorkPlaneFrame &workPlaneFrame,
    const QSize &viewportSize)
{
    ToolInput input;
    input.workPlaneFrame = workPlaneFrame;
    input.viewportSize = viewportSize;
    input.screenPosition = screenPosition;
    input.rawWorldPosition = rawWorldPosition;
    input.worldPosition = rawWorldPosition;
    input.key = event.key();
    input.text = event.text();
    input.modifiers = event.modifiers();
    input.autoRepeat = event.isAutoRepeat();
    return input;
}

ToolInput ToolInputTranslator::fromWheelEvent(
    const QWheelEvent &event,
    const QPointF &screenPosition,
    const WorkPlaneFrame &workPlaneFrame,
    const QSize &viewportSize)
{
    ToolInput input;
    input.screenPosition = screenPosition;
    input.workPlaneFrame = workPlaneFrame;
    input.viewportSize = viewportSize;
    input.modifiers = event.modifiers();
    input.wheelAngleDelta = event.angleDelta().y();
    input.wheelPixelDelta = event.pixelDelta().y();
    return input;
}

} // namespace classiCAD
