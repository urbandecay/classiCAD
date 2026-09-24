#include "input_helpers.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace classiCAD {

QPointF eventPosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

QPointF eventPosition(const QWheelEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->posF();
#endif
}

QString mouseButtonName(Qt::MouseButton button)
{
    return button == Qt::RightButton ? QStringLiteral("RMB") : QStringLiteral("MMB");
}

QString inputButtonName(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return QStringLiteral("LMB");
    case Qt::MiddleButton:
        return QStringLiteral("MMB");
    case Qt::RightButton:
        return QStringLiteral("RMB");
    case Qt::BackButton:
        return QStringLiteral("Back");
    case Qt::ForwardButton:
        return QStringLiteral("Forward");
    default:
        return QStringLiteral("Button(%1)").arg(static_cast<int>(button));
    }
}

QIcon makeEraserIcon()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(QStringLiteral("#171717")), 1.5));
    painter.setBrush(QColor(QStringLiteral("#e6b85c")));

    QPainterPath eraser;
    eraser.moveTo(7.0, 21.0);
    eraser.lineTo(17.5, 10.5);
    eraser.quadTo(19.0, 9.0, 20.5, 10.5);
    eraser.lineTo(26.5, 16.5);
    eraser.quadTo(28.0, 18.0, 26.5, 19.5);
    eraser.lineTo(19.0, 27.0);
    eraser.lineTo(11.0, 27.0);
    eraser.closeSubpath();
    painter.drawPath(eraser);

    painter.setPen(QPen(QColor(QStringLiteral("#8f5b2d")), 1.5));
    painter.drawLine(QPointF(13.0, 15.0), QPointF(21.0, 23.0));
    painter.setPen(QPen(QColor(QStringLiteral("#d7d7d7")), 2.0));
    painter.drawLine(QPointF(19.0, 27.0), QPointF(28.0, 27.0));
    return QIcon(pixmap);
}

QIcon makeTrimIcon()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(QStringLiteral("#171717")), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(5.0, 25.0), QPointF(27.0, 7.0));
    painter.setPen(QPen(QColor(QStringLiteral("#e6b85c")), 3.0));
    painter.drawLine(QPointF(7.0, 23.0), QPointF(14.0, 17.0));
    painter.setPen(QPen(QColor(QStringLiteral("#d7d7d7")), 2.0, Qt::DashLine));
    painter.drawLine(QPointF(17.0, 15.0), QPointF(26.0, 8.0));
    painter.setPen(QPen(QColor(QStringLiteral("#f0a45a")), 1.5));
    painter.setBrush(QColor(QStringLiteral("#f0a45a")));
    painter.drawEllipse(QPointF(14.0, 17.0), 2.5, 2.5);
    return QIcon(pixmap);
}

} // namespace classiCAD
