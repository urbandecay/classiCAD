#include "nurbs_surface_tessellator.h"

#include "nurbs_surface_evaluator.h"
#include "core/geometry/surface_trim_region.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

struct ParameterBounds {
    qreal u0 = 0.0;
    qreal u1 = 0.0;
    qreal v0 = 0.0;
    qreal v1 = 0.0;
};

bool segmentIntersectsBounds(const QPointF &start,
                             const QPointF &end,
                             const ParameterBounds &bounds)
{
    const qreal dx = end.x() - start.x();
    const qreal dy = end.y() - start.y();
    qreal first = 0.0;
    qreal last = 1.0;
    const auto clip = [&first, &last](qreal p, qreal q) {
        if (qFuzzyIsNull(p)) {
            return q >= 0.0;
        }
        const qreal ratio = q / p;
        if (p < 0.0) {
            if (ratio > last) {
                return false;
            }
            first = std::max(first, ratio);
        } else {
            if (ratio < first) {
                return false;
            }
            last = std::min(last, ratio);
        }
        return true;
    };
    return clip(-dx, start.x() - bounds.u0) &&
           clip(dx, bounds.u1 - start.x()) &&
           clip(-dy, start.y() - bounds.v0) &&
           clip(dy, bounds.v1 - start.y()) && first <= last;
}

bool trimBoundaryIntersects(const PreparedNurbsSurfaceTrimRegion &region,
                            const ParameterBounds &bounds)
{
    for (const PreparedNurbsSurfaceTrimRegion::Loop &loop : region.loops()) {
        for (int index = 0; index < loop.points.size(); ++index) {
            if (segmentIntersectsBounds(loop.points[index],
                                        loop.points[(index + 1) % loop.points.size()],
                                        bounds)) {
                return true;
            }
        }
    }
    return false;
}

quint64 parameterVertexKey(int uIndex, int vIndex)
{
    return (quint64(static_cast<quint32>(uIndex)) << 32) |
           static_cast<quint32>(vIndex);
}

QPointF parameterAt(qreal uIndex,
                    qreal vIndex,
                    int gridSize,
                    qreal uStart,
                    qreal uEnd,
                    qreal vStart,
                    qreal vEnd)
{
    return {uStart + (uEnd - uStart) * uIndex / gridSize,
            vStart + (vEnd - vStart) * vIndex / gridSize};
}

QVector<PreparedNurbsSurfaceTessellation::Polyline> clippedIsocurves(
    const PreparedNurbsSurfaceEvaluator &evaluator,
    const PreparedNurbsSurfaceTrimRegion &trimRegion,
    const PreparedNurbsSurfaceTessellation::Options &options,
    qreal uStart,
    qreal uEnd,
    qreal vStart,
    qreal vEnd)
{
    using Polyline = PreparedNurbsSurfaceTessellation::Polyline;
    QVector<Polyline> result;
    const auto boundaryInsidePoint = [&](const QPointF &inside,
                                         const QPointF &outside) {
        qreal lower = 0.0;
        qreal upper = 1.0;
        for (int iteration = 0; iteration < 28; ++iteration) {
            const qreal middle = (lower + upper) * 0.5;
            const QPointF parameter = inside + (outside - inside) * middle;
            if (trimRegion.contains(parameter)) {
                lower = middle;
            } else {
                upper = middle;
            }
        }
        const qreal safeFraction = std::max<qreal>(0.0, lower - 1.0e-10);
        const QPointF parameter = inside + (outside - inside) * safeFraction;
        Point3D point;
            evaluator.evaluate(parameter.x(), parameter.y(), &point);
        return point;
    };

    const auto appendFamily = [&](bool varyU) {
        for (int curveIndex = 0; curveIndex <= options.isocurveCount; ++curveIndex) {
            const qreal fixedFraction = static_cast<qreal>(curveIndex) /
                                        options.isocurveCount;
            const qreal fixedParameter = varyU
                ? vStart + (vEnd - vStart) * fixedFraction
                : uStart + (uEnd - uStart) * fixedFraction;
            Polyline current;
            QPointF previousParameter;
            bool previousInside = false;
            bool previousEvaluated = false;
            for (int sampleIndex = 0;
                 sampleIndex <= options.isocurveSamples;
                 ++sampleIndex) {
                const qreal fraction = static_cast<qreal>(sampleIndex) /
                                       options.isocurveSamples;
                const qreal variableParameter = varyU
                    ? uStart + (uEnd - uStart) * fraction
                    : vStart + (vEnd - vStart) * fraction;
                const QPointF parameter = varyU
                    ? QPointF(variableParameter, fixedParameter)
                    : QPointF(fixedParameter, variableParameter);
                Point3D point;
                const bool evaluated = evaluator.evaluate(parameter.x(),
                                                           parameter.y(),
                                                           &point);
                const bool inside = evaluated && trimRegion.contains(parameter);
                if (evaluated && previousEvaluated && inside != previousInside) {
                    if (previousInside) {
                        current.points.append(boundaryInsidePoint(previousParameter,
                                                                  parameter));
                        if (current.points.size() > 1) {
                            result.append(std::move(current));
                        }
                        current = Polyline{};
                    } else if (inside) {
                        current.points.append(boundaryInsidePoint(parameter,
                                                                  previousParameter));
                    }
                }
                if (inside) {
                    current.points.append(point);
                } else if (current.points.size() > 1) {
                    result.append(std::move(current));
                    current = Polyline{};
                } else {
                    current = Polyline{};
                }
                previousParameter = parameter;
                previousInside = inside;
                previousEvaluated = evaluated;
            }
            if (current.points.size() > 1) {
                result.append(std::move(current));
            }
        }
    };
    appendFamily(true);
    appendFamily(false);
    return result;
}

QVector<PreparedNurbsSurfaceTessellation::Polyline> sampledTrimBoundaries(
    const PreparedNurbsSurfaceEvaluator &evaluator,
    const PreparedNurbsSurfaceTrimRegion &trimRegion)
{
    QVector<PreparedNurbsSurfaceTessellation::Polyline> result;
    result.reserve(trimRegion.loops().size());
    for (const PreparedNurbsSurfaceTrimRegion::Loop &loop : trimRegion.loops()) {
        PreparedNurbsSurfaceTessellation::Polyline polyline;
        polyline.points.reserve(loop.points.size() + 1);
        for (const QPointF &parameter : loop.points) {
            Point3D point;
            if (!evaluator.evaluate(parameter.x(), parameter.y(), &point)) {
                polyline.points.clear();
                break;
            }
            polyline.points.append(point);
        }
        if (polyline.points.size() >= 3) {
            polyline.points.append(polyline.points.first());
            result.append(std::move(polyline));
        }
    }
    return result;
}

} // namespace

bool PreparedNurbsSurfaceTessellation::prepare(const NurbsSurface3D &surface)
{
    return prepare(surface, Options{});
}

bool PreparedNurbsSurfaceTessellation::prepare(
    const NurbsSurface3D &surface,
    const Options &options)
{
    vertices_.clear();
    triangles_.clear();
    wireframe_.clear();
    valid_ = false;
    if (options.gridCount < 1 ||
        options.gridCount > 256 || options.trimBoundaryDepth < 0 ||
        options.trimBoundaryDepth > 8 || options.isocurveCount < 1 ||
        options.isocurveSamples < 1 || options.trimSamples < 3) {
        return false;
    }

    qreal uStart = 0.0;
    qreal uEnd = 0.0;
    qreal vStart = 0.0;
    qreal vEnd = 0.0;
    PreparedNurbsSurfaceEvaluator surfaceEvaluator;
    if (!surfaceEvaluator.prepare(surface) ||
        !surfaceEvaluator.parameterDomains(&uStart,
                                          &uEnd,
                                          &vStart,
                                          &vEnd)) {
        return false;
    }
    PreparedNurbsSurfaceTrimRegion trimRegion;
    if (!trimRegion.prepare(surface, options.trimSamples)) {
        return false;
    }

    wireframe_ = clippedIsocurves(surfaceEvaluator,
                                  trimRegion,
                                  options,
                                  uStart,
                                  uEnd,
                                  vStart,
                                  vEnd);
    wireframe_ += sampledTrimBoundaries(surfaceEvaluator, trimRegion);

    // An affine plane with one convex trim needs only boundary triangles.
    // Validate convexity against every edge so self-intersecting or concave
    // loops cannot accidentally fill regions outside their visible boundary.
    bool affinePlane = !surface.rational && surface.degreeU == 1 &&
        surface.degreeV == 1 && surface.controlVertexCountU == 2 &&
        surface.controlVertexCountV == 2 && trimRegion.loops().size() == 1 &&
        !trimRegion.loops().first().isHole;
    if (affinePlane) {
        const auto &p = surface.controlPoints;
        const double tolerance = 256 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(p[0].x), std::abs(p[0].y), std::abs(p[0].z),
                      std::abs(p[3].x), std::abs(p[3].y), std::abs(p[3].z)});
        affinePlane = std::abs((p[1].x - p[0].x) - (p[3].x - p[2].x)) <= tolerance &&
                      std::abs((p[1].y - p[0].y) - (p[3].y - p[2].y)) <= tolerance &&
                      std::abs((p[1].z - p[0].z) - (p[3].z - p[2].z)) <= tolerance;
    }
    if (affinePlane) {
        const double scale = std::max({uEnd - uStart, vEnd - vStart, 1.0e-12});
        const double tolerance = 256 * std::numeric_limits<double>::epsilon() * scale;
        QVector<QPointF> polygon;
        for (const auto &point : trimRegion.loops().first().points) {
            if (point.x() < uStart - tolerance || point.x() > uEnd + tolerance ||
                point.y() < vStart - tolerance || point.y() > vEnd + tolerance) {
                affinePlane = false;
                break;
            }
            if (polygon.isEmpty() || std::hypot(point.x() - polygon.last().x(),
                                                point.y() - polygon.last().y()) > tolerance) {
                polygon.append(point);
            }
        }
        if (polygon.size() > 1 && std::hypot(polygon.first().x() - polygon.last().x(),
                                            polygon.first().y() - polygon.last().y()) <= tolerance) {
            polygon.removeLast();
        }
        const auto cross = [](const QPointF &a, const QPointF &b) {
            return a.x() * b.y() - a.y() * b.x();
        };
        double area = 0;
        QPointF center;
        if (polygon.size() < 3) affinePlane = false;
        for (int i = 0; i < polygon.size(); ++i) {
            area += cross(polygon[i] - polygon.first(),
                          polygon[(i + 1) % polygon.size()] - polygon.first());
            center += polygon[i];
        }
        if (std::abs(area) <= tolerance * scale) affinePlane = false;
        const double sign = area < 0 ? -1 : 1;
        for (int i = 0; affinePlane && i < polygon.size(); ++i) {
            const QPointF edge = polygon[(i + 1) % polygon.size()] - polygon[i];
            for (const auto &point : polygon) {
                if (sign * cross(edge, point - polygon[i]) < -tolerance * scale) {
                    affinePlane = false;
                    break;
                }
            }
        }
        if (affinePlane) {
            center /= polygon.size();
            affinePlane = trimRegion.contains(center);
        }
        if (affinePlane) {
            if (area < 0) std::reverse(polygon.begin(), polygon.end());
            polygon.prepend(center);
            for (const auto &parameter : polygon) {
                Point3D point;
                if (!surfaceEvaluator.evaluate(std::clamp(parameter.x(), uStart, uEnd),
                                               std::clamp(parameter.y(), vStart, vEnd), &point)) return false;
                vertices_.append(point);
            }
            for (int i = 1; i < polygon.size(); ++i) {
                triangles_.append({0, i, i + 1 < polygon.size() ? i + 1 : 1});
            }
            valid_ = true;
            return true;
        }
    }

    // A translational extrusion is linear in V. Extra V rows repeat the
    // same planar strips, adding vertices and triangles without improving
    // the approximation of the curved U direction.
    bool linearExtrusion = !trimRegion.isTrimmed() && surface.degreeV == 1 &&
                           surface.controlVertexCountV == 2;
    if (linearExtrusion) {
        const auto &a = surface.controlPoints[0];
        const auto &b = surface.controlPoints[1];
        const Point3D vector{b.x - a.x, b.y - a.y, b.z - a.z};
        for (int u = 0; u < surface.controlVertexCountU; ++u) {
            const auto &p = surface.controlPoints[u * 2];
            const auto &q = surface.controlPoints[u * 2 + 1];
            const double tolerance = 256 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, std::abs(p.x), std::abs(p.y), std::abs(p.z),
                          std::abs(q.x), std::abs(q.y), std::abs(q.z)});
            if (std::abs((q.x - p.x) - vector.x) > tolerance ||
                std::abs((q.y - p.y) - vector.y) > tolerance ||
                std::abs((q.z - p.z) - vector.z) > tolerance ||
                (surface.rational && surface.weights[u * 2] != surface.weights[u * 2 + 1])) {
                linearExtrusion = false;
                break;
            }
        }
    }
    if (linearExtrusion) {
        for (int u = 0; u <= options.gridCount; ++u) {
            const qreal parameter = uStart + (uEnd - uStart) * u / options.gridCount;
            for (const qreal v : {vStart, vEnd}) {
                Point3D point;
                if (!surfaceEvaluator.evaluate(parameter, v, &point)) return false;
                vertices_.append(point);
            }
            if (u > 0) {
                const int first = (u - 1) * 2;
                triangles_.append({first, first + 2, first + 3});
                triangles_.append({first, first + 3, first + 1});
            }
        }
        valid_ = !triangles_.isEmpty();
        return valid_;
    }

    const int subdivisions = 1 << options.trimBoundaryDepth;
    const int meshGridSize = options.gridCount * subdivisions;
    QHash<quint64, int> vertexIndices;
    const auto vertexAt = [&](int uIndex, int vIndex) {
        const quint64 key = parameterVertexKey(uIndex, vIndex);
        const auto found = vertexIndices.constFind(key);
        if (found != vertexIndices.cend()) {
            return found.value();
        }
        const QPointF parameter = parameterAt(uIndex,
                                              vIndex,
                                              meshGridSize,
                                              uStart,
                                              uEnd,
                                              vStart,
                                              vEnd);
        Point3D point;
        if (!surfaceEvaluator.evaluate(parameter.x(), parameter.y(), &point)) {
            return -1;
        }
        const int vertexIndex = vertices_.size();
        vertices_.append(point);
        vertexIndices.insert(key, vertexIndex);
        return vertexIndex;
    };
    const auto appendTriangle = [&](int u0,
                                    int v0,
                                    int u1,
                                    int v1,
                                    int u2,
                                    int v2) {
        const int first = vertexAt(u0, v0);
        const int second = vertexAt(u1, v1);
        const int third = vertexAt(u2, v2);
        if (first < 0 || second < 0 || third < 0 || first == second ||
            second == third || first == third) {
            return false;
        }
        triangles_.append({first, second, third});
        return true;
    };

    const auto tessellateCell = [&](auto &&self,
                                    int u0,
                                    int v0,
                                    int step,
                                    int depth) -> void {
        const int u1 = u0 + step;
        const int v1 = v0 + step;
        const QPointF p00 = parameterAt(u0, v0, meshGridSize,
                                        uStart, uEnd, vStart, vEnd);
        const QPointF p10 = parameterAt(u1, v0, meshGridSize,
                                        uStart, uEnd, vStart, vEnd);
        const QPointF p11 = parameterAt(u1, v1, meshGridSize,
                                        uStart, uEnd, vStart, vEnd);
        const QPointF p01 = parameterAt(u0, v1, meshGridSize,
                                        uStart, uEnd, vStart, vEnd);
        const QPointF center = parameterAt(u0 + step * 0.5,
                                           v0 + step * 0.5,
                                           meshGridSize,
                                           uStart, uEnd, vStart, vEnd);
        const bool inside00 = trimRegion.contains(p00);
        const bool inside10 = trimRegion.contains(p10);
        const bool inside11 = trimRegion.contains(p11);
        const bool inside01 = trimRegion.contains(p01);
        const bool insideCenter = trimRegion.contains(center);
        const ParameterBounds bounds{p00.x(), p11.x(), p00.y(), p11.y()};
        const bool crossesBoundary = trimRegion.isTrimmed() &&
                                     trimBoundaryIntersects(trimRegion, bounds);
        const bool allInside = inside00 && inside10 && inside11 && inside01 &&
                               insideCenter;
        const bool allOutside = !inside00 && !inside10 && !inside11 &&
                                !inside01 && !insideCenter;
        if (allInside && !crossesBoundary) {
            appendTriangle(u0, v0, u1, v0, u1, v1);
            appendTriangle(u0, v0, u1, v1, u0, v1);
            return;
        }
        if (allOutside && !crossesBoundary) {
            return;
        }
        if (depth < options.trimBoundaryDepth && step > 1) {
            const int halfStep = step / 2;
            self(self, u0, v0, halfStep, depth + 1);
            self(self, u0 + halfStep, v0, halfStep, depth + 1);
            self(self, u0 + halfStep, v0 + halfStep, halfStep, depth + 1);
            self(self, u0, v0 + halfStep, halfStep, depth + 1);
            return;
        }

        const qreal stepAsReal = step;
        const QPointF firstTriangleCenter = parameterAt(
            u0 + stepAsReal * (2.0 / 3.0),
            v0 + stepAsReal * (1.0 / 3.0),
            meshGridSize, uStart, uEnd, vStart, vEnd);
        if (trimRegion.contains(firstTriangleCenter)) {
            appendTriangle(u0, v0, u1, v0, u1, v1);
        }
        const QPointF secondTriangleCenter = parameterAt(
            u0 + stepAsReal * (1.0 / 3.0),
            v0 + stepAsReal * (2.0 / 3.0),
            meshGridSize, uStart, uEnd, vStart, vEnd);
        if (trimRegion.contains(secondTriangleCenter)) {
            appendTriangle(u0, v0, u1, v1, u0, v1);
        }
    };

    const int initialStep = subdivisions;
    for (int uIndex = 0; uIndex < options.gridCount; ++uIndex) {
        for (int vIndex = 0; vIndex < options.gridCount; ++vIndex) {
            tessellateCell(tessellateCell,
                           uIndex * initialStep,
                           vIndex * initialStep,
                           initialStep,
                           0);
        }
    }
    valid_ = true;
    return true;
}

bool PreparedNurbsSurfaceTessellation::isValid() const
{
    return valid_;
}

PreparedNurbsSurfaceTessellation PreparedNurbsSurfaceTessellation::translated(
    const Point3D &offset) const
{
    auto result = *this;
    const auto move = [&offset](Point3D &point) {
        point.x += offset.x;
        point.y += offset.y;
        point.z += offset.z;
    };
    for (Point3D &point : result.vertices_) {
        move(point);
    }
    for (Polyline &line : result.wireframe_) {
        for (Point3D &point : line.points) {
            move(point);
        }
    }
    return result;
}

const QVector<Point3D> &PreparedNurbsSurfaceTessellation::vertices() const
{
    return vertices_;
}

const QVector<PreparedNurbsSurfaceTessellation::Triangle> &
PreparedNurbsSurfaceTessellation::triangles() const
{
    return triangles_;
}

const QVector<PreparedNurbsSurfaceTessellation::Polyline> &
PreparedNurbsSurfaceTessellation::wireframe() const
{
    return wireframe_;
}

} // namespace classiCAD
