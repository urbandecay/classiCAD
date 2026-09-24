#pragma once

#include <QIcon>
#include <QMouseEvent>
#include <QPointF>
#include <QString>
#include <QWheelEvent>

namespace classiCAD {

QPointF eventPosition(const QMouseEvent *event);
QPointF eventPosition(const QWheelEvent *event);
QString mouseButtonName(Qt::MouseButton button);
QString inputButtonName(Qt::MouseButton button);
QIcon makeEraserIcon();
QIcon makeTrimIcon();

} // namespace classiCAD
