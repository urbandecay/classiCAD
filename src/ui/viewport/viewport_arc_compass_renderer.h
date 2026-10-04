#pragma once

#include "core/geometry/work_plane.h"
#include "services/viewport/viewport_transform.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

class ViewportArcCompassRenderer final {
public:
    static void draw(QPainter &painter,
                     const ViewportTransform &transform,
                     const WorkPlaneFrame &frame,
                     const QPointF &localCenter,
                     const QSize &viewportSize,
                     qreal rotation,
                     qreal angleIncrementDegrees = 15.0);
};

} // namespace classiCAD
