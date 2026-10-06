#include "nurbs_surface.h"

#include "curve_evaluator.h"
#include "nurbs_surface_evaluator.h"
#include "surface_trim_region.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
QVector<double> expandedNurbsSurfaceKnotVector(const QVector<double> &knots)
{
    QVector<double> expanded;
    if (!knots.isEmpty()) {
        expanded.reserve(knots.size() + 2);
        expanded.append(knots.first());
        expanded += knots;
        expanded.append(knots.last());
    }
    return expanded;
}

bool validateNurbsSurface(const NurbsSurface3D &surface, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (surface.dimension != 3) {
        return fail(QStringLiteral("NURBS surface dimension must be 3"));
    }
    if (surface.degreeU < 1 || surface.orderU != surface.degreeU + 1 ||
        surface.degreeV < 1 || surface.orderV != surface.degreeV + 1) {
        return fail(QStringLiteral("NURBS surface orders must equal degrees plus one"));
    }
    if (surface.controlVertexCountU <= surface.degreeU ||
        surface.controlVertexCountV <= surface.degreeV) {
        return fail(QStringLiteral("NURBS surface control net is too small for its degrees"));
    }
    const qint64 controlPointCount = qint64(surface.controlVertexCountU) *
                                     surface.controlVertexCountV;
    if (controlPointCount <= 0 || controlPointCount > std::numeric_limits<int>::max() ||
        surface.controlPoints.size() != controlPointCount ||
        surface.weights.size() != controlPointCount) {
        return fail(QStringLiteral("NURBS surface control net and weights are inconsistent"));
    }
    if (surface.knotsU.size() != surface.controlVertexCountU + surface.orderU - 2 ||
        surface.knotsV.size() != surface.controlVertexCountV + surface.orderV - 2) {
        return fail(QStringLiteral("NURBS surface reduced knot counts are invalid"));
    }

    for (const Point3D &point : surface.controlPoints) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z)) {
            return fail(QStringLiteral("NURBS surface control vertex is not finite"));
        }
    }
    for (const double weight : surface.weights) {
        if (!std::isfinite(weight) || weight <= 0.0) {
            return fail(QStringLiteral("NURBS surface weights must be positive and finite"));
        }
    }
    const auto validKnots = [](const QVector<double> &knots) {
        for (int index = 0; index < knots.size(); ++index) {
            if (!std::isfinite(knots[index]) ||
                (index > 0 && knots[index] < knots[index - 1])) {
                return false;
            }
        }
        return true;
    };
    if (!validKnots(surface.knotsU) || !validKnots(surface.knotsV)) {
        return fail(QStringLiteral("NURBS surface knots must be finite and nondecreasing"));
    }

    const QVector<double> uKnots = expandedNurbsSurfaceKnotVector(surface.knotsU);
    const QVector<double> vKnots = expandedNurbsSurfaceKnotVector(surface.knotsV);
    if (uKnots.size() != surface.controlVertexCountU + surface.orderU ||
        vKnots.size() != surface.controlVertexCountV + surface.orderV ||
        uKnots[surface.degreeU] >= uKnots[surface.controlVertexCountU] ||
        vKnots[surface.degreeV] >= vKnots[surface.controlVertexCountV]) {
        return fail(QStringLiteral("NURBS surface parameter domain is invalid"));
    }

    if (!surface.trimLoops.isEmpty()) {
        if (surface.trimLoops.first().isHole) {
            return fail(QStringLiteral("the first NURBS surface trim loop must be an outer boundary"));
        }
        const qreal uStart = uKnots[surface.degreeU];
        const qreal uEnd = uKnots[surface.controlVertexCountU];
        const qreal vStart = vKnots[surface.degreeV];
        const qreal vEnd = vKnots[surface.controlVertexCountV];
        const qreal parameterScale = std::max<qreal>(
            {1.0, std::abs(uStart), std::abs(uEnd), std::abs(vStart), std::abs(vEnd)});
        const qreal tolerance = parameterScale * 1.0e-8;
        for (const NurbsSurfaceTrimLoop &loop : surface.trimLoops) {
            if (loop.curve.dimension != 2 || !validateNurbsCurve(loop.curve)) {
                return fail(QStringLiteral("NURBS surface trim loop curve is invalid"));
            }
            qreal curveStart = 0.0;
            qreal curveEnd = 0.0;
            QPointF firstPoint;
            QPointF lastPoint;
            if (!nurbsParameterDomain(loop.curve, &curveStart, &curveEnd) ||
                !evaluateNurbsPoint(loop.curve, curveStart, &firstPoint) ||
                !evaluateNurbsPoint(loop.curve, curveEnd, &lastPoint) ||
                std::hypot(firstPoint.x() - lastPoint.x(),
                           firstPoint.y() - lastPoint.y()) > tolerance) {
                return fail(QStringLiteral("NURBS surface trim loops must be closed"));
            }
            for (const QPointF &point : loop.curve.controlPoints) {
                if (point.x() < uStart - tolerance || point.x() > uEnd + tolerance ||
                    point.y() < vStart - tolerance || point.y() > vEnd + tolerance) {
                    return fail(QStringLiteral("NURBS surface trim loop lies outside its parameter domain"));
                }
            }
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool nurbsSurfaceParameterDomains(const NurbsSurface3D &surface,
                                  qreal *uStart,
                                  qreal *uEnd,
                                  qreal *vStart,
                                  qreal *vEnd)
{
    PreparedNurbsSurfaceEvaluator evaluator;
    return evaluator.prepare(surface) &&
           evaluator.parameterDomains(uStart, uEnd, vStart, vEnd);
}

bool evaluateNurbsSurfacePoint(const NurbsSurface3D &surface,
                               qreal u,
                               qreal v,
                               Point3D *point)
{
    PreparedNurbsSurfaceEvaluator evaluator;
    return evaluator.prepare(surface) && evaluator.evaluate(u, v, point);
}

QVector<QPointF> sampleNurbsSurfaceTrimLoop(const NurbsSurfaceTrimLoop &loop,
                                            int sampleCount)
{
    QVector<QPointF> points;
    if (!validateNurbsCurve(loop.curve) || sampleCount < 3) {
        return points;
    }
    qreal start = 0.0;
    qreal end = 0.0;
    if (!nurbsParameterDomain(loop.curve, &start, &end)) {
        return points;
    }
    const QVector<double> fullKnots = expandedNurbsKnotVector(loop.curve);
    struct KnotSpan {
        qreal start = 0.0;
        qreal end = 0.0;
        int subdivisions = 1;
    };
    QVector<KnotSpan> spans;
    spans.reserve(loop.curve.controlPoints.size() - loop.curve.degree);
    for (int spanIndex = loop.curve.degree;
         spanIndex < loop.curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }
        const int subdivisions = std::max(
            1, static_cast<int>(std::ceil(
                   sampleCount * (spanEnd - spanStart) / (end - start))));
        spans.append({spanStart, spanEnd, subdivisions});
    }
    if (spans.isEmpty()) {
        return points;
    }
    int totalSamples = 0;
    for (const KnotSpan &span : spans) {
        totalSamples += span.subdivisions;
    }
    points.reserve(totalSamples);
    for (const KnotSpan &span : spans) {
        for (int index = 0; index < span.subdivisions; ++index) {
            const qreal parameter = span.start +
                (span.end - span.start) * index / span.subdivisions;
            QPointF point;
            if (!evaluateNurbsPoint(loop.curve, parameter, &point)) {
                return {};
            }
            points.append(point);
        }
    }
    return points;
}

bool nurbsSurfaceParameterInsideTrim(const NurbsSurface3D &surface,
                                     qreal u,
                                     qreal v)
{
    if (!validateNurbsSurface(surface) || !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }
    PreparedNurbsSurfaceTrimRegion region;
    return region.prepare(surface, 128) && region.contains(u, v);
}

} // namespace classiCAD
