#include "viewport_snap_marker_renderer.h"

#include <QFont>
#include <QPen>

namespace classiCAD {

ViewportSnapMarkerRenderer::ViewportSnapMarkerRenderer(
    const ViewportTransform &transform)
    : transform_(transform)
{
}

void ViewportSnapMarkerRenderer::setLabelsVisible(bool visible)
{
    labelsVisible_ = visible;
}

bool ViewportSnapMarkerRenderer::labelsVisible() const
{
    return labelsVisible_;
}

void ViewportSnapMarkerRenderer::draw(QPainter &painter,
                                      SnapType type,
                                      const QPointF &worldPoint,
                                      const QSize &viewportSize) const
{
    painter.save();
    const QPointF snapScreen = transform_.worldToScreen(worldPoint, viewportSize);
    const QColor snapColor(QStringLiteral("#63b5e8"));
    painter.setPen(QPen(snapColor, 2.0));
    painter.setBrush(Qt::NoBrush);

    if (type == SnapType::Endpoint) {
        painter.drawEllipse(snapScreen, 7.0, 7.0);
    } else if (type == SnapType::Midpoint) {
        painter.drawRect(QRectF(snapScreen - QPointF(6.0, 6.0),
                                snapScreen + QPointF(6.0, 6.0)));
    } else if (type == SnapType::ControlPoint) {
        painter.drawRect(QRectF(snapScreen - QPointF(7.0, 7.0),
                                snapScreen + QPointF(7.0, 7.0)));
    } else if (type == SnapType::Intersection) {
        painter.drawLine(snapScreen - QPointF(7.0, 7.0),
                         snapScreen + QPointF(7.0, 7.0));
        painter.drawLine(snapScreen - QPointF(7.0, -7.0),
                         snapScreen + QPointF(7.0, -7.0));
    } else if (type == SnapType::Center) {
        painter.drawEllipse(snapScreen, 7.0, 7.0);
        painter.drawLine(snapScreen - QPointF(9.0, 0.0),
                         snapScreen + QPointF(9.0, 0.0));
        painter.drawLine(snapScreen - QPointF(0.0, 9.0),
                         snapScreen + QPointF(0.0, 9.0));
    } else if (type == SnapType::Perpendicular) {
        const QPointF corner = snapScreen + QPointF(-2.0, 3.0);
        painter.drawLine(snapScreen + QPointF(-8.0, 3.0), corner);
        painter.drawLine(corner, snapScreen + QPointF(-2.0, -5.0));
    } else if (type == SnapType::Tangent) {
        painter.drawEllipse(snapScreen, 6.0, 6.0);
        painter.drawLine(snapScreen + QPointF(-7.0, 4.0),
                         snapScreen + QPointF(7.0, -4.0));
    } else if (type == SnapType::Near) {
        const QPointF diamond[] = {snapScreen + QPointF(0.0, -7.0),
                                   snapScreen + QPointF(7.0, 0.0),
                                   snapScreen + QPointF(0.0, 7.0),
                                   snapScreen + QPointF(-7.0, 0.0)};
        painter.drawPolygon(diamond, 4);
    }

    if (labelsVisible_ && type != SnapType::None) {
        painter.setPen(snapColor);
        painter.setFont(QFont(QStringLiteral("Sans"), 9, QFont::Bold));
        painter.drawText(snapScreen + QPointF(10.0, -10.0), snapTypeName(type));
    }
    painter.restore();
}

} // namespace classiCAD
