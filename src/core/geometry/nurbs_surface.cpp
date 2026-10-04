#include "nurbs_surface.h"

#include "curve_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

qreal basisValue(const QVector<double> &knots,
                 int index,
                 int degree,
                 qreal parameter)
{
    if (degree == 0) {
        return knots[index] <= parameter && parameter < knots[index + 1]
                   ? 1.0
                   : 0.0;
    }

    qreal value = 0.0;
    const qreal leftDenominator = knots[index + degree] - knots[index];
    if (std::abs(leftDenominator) > 1.0e-12) {
        value += (parameter - knots[index]) / leftDenominator *
                 basisValue(knots, index, degree - 1, parameter);
    }

    const qreal rightDenominator = knots[index + degree + 1] - knots[index + 1];
    if (std::abs(rightDenominator) > 1.0e-12) {
        value += (knots[index + degree + 1] - parameter) /
                 rightDenominator *
                 basisValue(knots, index + 1, degree - 1, parameter);
    }
    return value;
}

} // namespace

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
    if (!validateNurbsSurface(surface) ||
        (uStart == nullptr && uEnd == nullptr && vStart == nullptr && vEnd == nullptr)) {
        return false;
    }
    const QVector<double> uKnots = expandedNurbsSurfaceKnotVector(surface.knotsU);
    const QVector<double> vKnots = expandedNurbsSurfaceKnotVector(surface.knotsV);
    if (uStart != nullptr) {
        *uStart = uKnots[surface.degreeU];
    }
    if (uEnd != nullptr) {
        *uEnd = uKnots[surface.controlVertexCountU];
    }
    if (vStart != nullptr) {
        *vStart = vKnots[surface.degreeV];
    }
    if (vEnd != nullptr) {
        *vEnd = vKnots[surface.controlVertexCountV];
    }
    return true;
}

bool evaluateNurbsSurfacePoint(const NurbsSurface3D &surface,
                               qreal u,
                               qreal v,
                               Point3D *point)
{
    if (!validateNurbsSurface(surface) || point == nullptr ||
        !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    const QVector<double> uKnots = expandedNurbsSurfaceKnotVector(surface.knotsU);
    const QVector<double> vKnots = expandedNurbsSurfaceKnotVector(surface.knotsV);
    const qreal domainUStart = uKnots[surface.degreeU];
    const qreal domainUEnd = uKnots[surface.controlVertexCountU];
    const qreal domainVStart = vKnots[surface.degreeV];
    const qreal domainVEnd = vKnots[surface.controlVertexCountV];
    u = std::clamp(u, domainUStart, domainUEnd);
    v = std::clamp(v, domainVStart, domainVEnd);
    if (u >= domainUEnd) {
        u = std::nextafter(domainUEnd, domainUStart);
    }
    if (v >= domainVEnd) {
        v = std::nextafter(domainVEnd, domainVStart);
    }

    Point3D numerator{};
    qreal denominator = 0.0;
    for (int uIndex = 0; uIndex < surface.controlVertexCountU; ++uIndex) {
        const qreal uBasis = basisValue(uKnots, uIndex, surface.degreeU, u);
        if (uBasis == 0.0) {
            continue;
        }
        for (int vIndex = 0; vIndex < surface.controlVertexCountV; ++vIndex) {
            const int controlIndex = uIndex * surface.controlVertexCountV + vIndex;
            const qreal basis = uBasis *
                basisValue(vKnots, vIndex, surface.degreeV, v) *
                (surface.rational ? surface.weights[controlIndex] : 1.0);
            const Point3D &controlPoint = surface.controlPoints[controlIndex];
            numerator.x += controlPoint.x * basis;
            numerator.y += controlPoint.y * basis;
            numerator.z += controlPoint.z * basis;
            denominator += basis;
        }
    }
    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }
    point->x = numerator.x / denominator;
    point->y = numerator.y / denominator;
    point->z = numerator.z / denominator;
    return true;
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
    points.reserve(sampleCount);
    for (int index = 0; index < sampleCount; ++index) {
        const qreal parameter = start + (end - start) * index / sampleCount;
        QPointF point;
        if (!evaluateNurbsPoint(loop.curve, parameter, &point)) {
            return {};
        }
        points.append(point);
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
    if (surface.trimLoops.isEmpty()) {
        return true;
    }

    bool insideOuterLoop = false;
    for (const NurbsSurfaceTrimLoop &loop : surface.trimLoops) {
        const QVector<QPointF> polygon = sampleNurbsSurfaceTrimLoop(loop);
        if (polygon.size() < 3) {
            return false;
        }
        const bool inside = pointInsidePolygon(QPointF(u, v), polygon);
        if (!loop.isHole && inside) {
            insideOuterLoop = true;
        } else if (loop.isHole && inside) {
            return false;
        }
    }
    return insideOuterLoop;
}

} // namespace classiCAD
