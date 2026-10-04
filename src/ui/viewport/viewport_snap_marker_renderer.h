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

private:
    const ViewportTransform &transform_;
    bool labelsVisible_ = true;
};

} // namespace classiCAD
