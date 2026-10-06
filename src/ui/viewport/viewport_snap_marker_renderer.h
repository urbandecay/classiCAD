#pragma once

#include "services/snapping/snap_types.h"
#include "services/viewport/viewport_transform.h"

#include <QPainter>
#include <QSize>

namespace classiCAD {

class ViewportSnapMarkerRenderer final {
public:
    explicit ViewportSnapMarkerRenderer(const ViewportTransform &transform);

    void setLabelsVisible(bool visible);
    bool labelsVisible() const;
    void draw(QPainter &painter,
              SnapType type,
              const QPointF &worldPoint,
              const QSize &viewportSize) const;
    void drawWorld(QPainter &painter,
                   SnapType type,
                   const Point3D &worldPoint,
                   const QSize &viewportSize) const;

private:
    void drawAtScreen(QPainter &painter,
                      SnapType type,
                      const QPointF &screenPoint) const;

    const ViewportTransform &transform_;
    bool labelsVisible_ = true;
};

} // namespace classiCAD
