#pragma once

#include "core/geometry/nurbs_surface.h"

namespace classiCAD {

// Immutable sampled UV boundaries for one surface query or render pass.
// Sampling is derived data; the source trim curves remain on NurbsSurface3D.
class PreparedNurbsSurfaceTrimRegion final {
public:
    struct Loop {
        bool isHole = false;
        QVector<QPointF> points;
    };

    bool prepare(const NurbsSurface3D &surface, int sampleCount = 256);
    bool contains(const QPointF &parameter) const;
    bool contains(qreal u, qreal v) const;
    bool isValid() const;
    bool isTrimmed() const;
    const QVector<Loop> &loops() const;

private:
    QVector<Loop> loops_;
    bool valid_ = false;
};

} // namespace classiCAD
