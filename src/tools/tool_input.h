#pragma once

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
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int key = 0;
    QString text;
    int wheelAngleDelta = 0;
    int wheelPixelDelta = 0;
    QSize viewportSize;
};

} // namespace classiCAD
