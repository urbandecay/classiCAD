#pragma once

#include "core/model.h"

#include <QPointF>
#include <QSize>
#include <QString>
#include <Qt>

namespace classiCAD {

// ToolInput is the Qt-free event payload shared by interaction tools. The
// viewport translates Q*Event objects into this value before dispatching.
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
