#pragma once

#include "core/geometry/work_plane.h"
#include "services/snapping/snap_types.h"

#include <QPointF>
#include <QSize>
#include <QString>
#include <Qt>

namespace classiCAD {

// ToolInput is the event payload shared by interaction tools. It contains no
// widget or Qt event types; src/ui/input translates events into this value.
struct ToolInput {
    QPointF screenPosition;
    QPointF rawWorldPosition;
    QPointF worldPosition;
    WorkPlaneFrame workPlaneFrame;
    bool orthoEnabled = false;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int key = 0;
    QString text;
    bool autoRepeat = false;
    int wheelAngleDelta = 0;
    int wheelPixelDelta = 0;
    QSize viewportSize;
    SnapResult snapResult;
    SnapType snapType = SnapType::None;

    Point3D resolvedWorldPoint() const
    {
        if (snapResult.isValid() && snapResult.hasWorldPoint) {
            return snapResult.worldPoint;
        }
        const WorkPlaneFrame frame = isValidWorkPlaneFrame(workPlaneFrame)
                                         ? workPlaneFrame
                                         : makeWorkPlaneFrame(WorkPlane::XY, 0.0);
        return workPlaneFramePointToWorld(worldPosition, frame);
    }

    QPointF positionInFrame(const WorkPlaneFrame &frame) const
    {
        const WorkPlaneFrame target = isValidWorkPlaneFrame(frame)
                                          ? frame
                                          : (isValidWorkPlaneFrame(workPlaneFrame)
                                                 ? workPlaneFrame
                                                 : makeWorkPlaneFrame(
                                                       WorkPlane::XY, 0.0));
        return worldPointToWorkPlaneFrame(resolvedWorldPoint(), target);
    }
};

} // namespace classiCAD
