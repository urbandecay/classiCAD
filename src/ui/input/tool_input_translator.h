#pragma once

#include "core/tool_id.h"
#include "tools/tool_input.h"

#include <QPointF>
#include <QSize>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace classiCAD {

class ToolInputTranslator final {
public:
    static ToolInput fromMouseEvent(const QMouseEvent &event,
                                    ToolId activeTool,
                                    const QPointF &screenPosition,
                                    const QPointF &rawWorldPosition,
                                    const QPointF &worldPosition,
                                    const WorkPlaneFrame &workPlaneFrame,
                                    bool orthoEnabled,
                                    const QSize &viewportSize,
                                    const SnapResult &snapResult);

    static ToolInput fromKeyEvent(const QKeyEvent &event,
                                  const QPointF &screenPosition,
                                  const QPointF &rawWorldPosition,
                                  const WorkPlaneFrame &workPlaneFrame,
                                  const QSize &viewportSize);

    static ToolInput fromWheelEvent(const QWheelEvent &event,
                                    const QPointF &screenPosition,
                                    const WorkPlaneFrame &workPlaneFrame,
                                    const QSize &viewportSize);
};

} // namespace classiCAD
