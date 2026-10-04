#include "point_text.h"

namespace classiCAD {

QString pointText(const QPointF &point)
{
    return QStringLiteral("(%1, %2)")
        .arg(point.x(), 0, 'f', 3)
        .arg(point.y(), 0, 'f', 3);
}

} // namespace classiCAD
