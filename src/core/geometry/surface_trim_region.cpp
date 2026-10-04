#include "surface_trim_region.h"

#include <cmath>
#include <utility>

namespace classiCAD {
namespace {

bool pointInsidePolygon(const QPointF &point, const QVector<QPointF> &polygon)
{
    bool inside = false;
    for (int current = 0, previous = polygon.size() - 1;
         current < polygon.size();
         previous = current++) {
        const QPointF &a = polygon[current];
        const QPointF &b = polygon[previous];
        const bool crosses = (a.y() > point.y()) != (b.y() > point.y());
        if (crosses && point.x() < (b.x() - a.x()) *
                                         (point.y() - a.y()) /
                                         (b.y() - a.y()) + a.x()) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace

bool PreparedNurbsSurfaceTrimRegion::prepare(const NurbsSurface3D &surface,
                                             int sampleCount)
{
    loops_.clear();
    valid_ = false;
    if (sampleCount < 3 ||
        (!surface.trimLoops.isEmpty() && surface.trimLoops.first().isHole)) {
        return false;
    }

    loops_.reserve(surface.trimLoops.size());
    for (const NurbsSurfaceTrimLoop &sourceLoop : surface.trimLoops) {
        Loop loop;
        loop.isHole = sourceLoop.isHole;
        loop.points = sampleNurbsSurfaceTrimLoop(sourceLoop, sampleCount);
        if (loop.points.size() < 3) {
            loops_.clear();
            return false;
        }
        for (const QPointF &point : loop.points) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                loops_.clear();
                return false;
            }
        }
        loops_.append(std::move(loop));
    }
    valid_ = true;
    return true;
}

bool PreparedNurbsSurfaceTrimRegion::contains(const QPointF &parameter) const
{
    return contains(parameter.x(), parameter.y());
}

bool PreparedNurbsSurfaceTrimRegion::contains(qreal u, qreal v) const
{
    if (!valid_ || !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }
    if (loops_.isEmpty()) {
        return true;
    }

    bool insideOuterLoop = false;
    const QPointF parameter(u, v);
    for (const Loop &loop : loops_) {
        const bool inside = pointInsidePolygon(parameter, loop.points);
        if (!loop.isHole && inside) {
            insideOuterLoop = true;
        } else if (loop.isHole && inside) {
            return false;
        }
    }
    return insideOuterLoop;
}

bool PreparedNurbsSurfaceTrimRegion::isValid() const
{
    return valid_;
}

bool PreparedNurbsSurfaceTrimRegion::isTrimmed() const
{
    return !loops_.isEmpty();
}

const QVector<PreparedNurbsSurfaceTrimRegion::Loop> &
PreparedNurbsSurfaceTrimRegion::loops() const
{
    return loops_;
}

} // namespace classiCAD
