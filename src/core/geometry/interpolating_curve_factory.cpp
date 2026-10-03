#include "interpolating_curve_factory.h"

#include <cmath>
#include <QVector>

namespace classiCAD {
namespace {

QPointF reflectedEndpoint(const QPointF &endpoint, const QPointF &neighbor)
{
    return endpoint * 2.0 - neighbor;
}

double centripetalStep(const QPointF &first, const QPointF &second)
{
    return std::sqrt(std::hypot(second.x() - first.x(), second.y() - first.y()));
}

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

} // namespace

bool makeCentripetalCatmullRomNurbsCurve(const QVector<QPointF> &points,
                                         NurbsCurve2D *curve)
{
    if (curve == nullptr || points.size() < 2) {
        return false;
    }
    for (const QPointF &point : points) {
        if (!finitePoint(point)) {
            return false;
        }
    }

    NurbsCurve2D result;
    if (points.size() == 2) {
        result.degree = 1;
        result.order = 2;
        result.controlPoints = points;
        result.weights = {1.0, 1.0};
        result.knots = {0.0, 1.0};
        result.rational = false;
        if (!validateNurbsCurve(result)) {
            return false;
        }
        *curve = std::move(result);
        return true;
    }

    const int segmentCount = static_cast<int>(points.size()) - 1;
    QVector<double> segmentParameters(segmentCount, 0.0);
    double totalParameterLength = 0.0;
    for (int i = 1; i < points.size(); ++i) {
        const double step = centripetalStep(points[i - 1], points[i]);
        if (!std::isfinite(step) || step <= 1.0e-12) {
            return false;
        }
        segmentParameters[i - 1] = step;
        totalParameterLength += step;
    }
    if (!std::isfinite(totalParameterLength) || totalParameterLength <= 1.0e-12) {
        return false;
    }

    result.degree = 3;
    result.order = 4;
    result.rational = false;
    result.controlPoints.reserve(3 * segmentCount + 1);
    result.weights.fill(1.0, 3 * segmentCount + 1);

    QVector<QPointF> padded;
    padded.reserve(points.size() + 2);
    padded.append(reflectedEndpoint(points.first(), points[1]));
    padded += points;
    padded.append(reflectedEndpoint(points.last(), points[points.size() - 2]));

    double parameterStart = 0.0;
    QVector<double> fullKnots;
    fullKnots.reserve(3 * segmentCount + 5);
    for (int i = 0; i < 4; ++i) {
        fullKnots.append(0.0);
    }
    for (int segment = 0; segment < segmentCount; ++segment) {
        const QPointF &p0 = padded[segment];
        const QPointF &p1 = padded[segment + 1];
        const QPointF &p2 = padded[segment + 2];
        const QPointF &p3 = padded[segment + 3];
        const double t1 = centripetalStep(p0, p1);
        const double t2 = t1 + centripetalStep(p1, p2);
        const double t3 = t2 + centripetalStep(p2, p3);
        if (!std::isfinite(t1) || !std::isfinite(t2) || !std::isfinite(t3) ||
            t1 <= 1.0e-12 || t2 - t1 <= 1.0e-12 || t3 - t1 <= 1.0e-12) {
            return false;
        }

        const QPointF firstTangent = (p2 - p0) * ((t2 - t1) / (t2 - 0.0));
        const QPointF secondTangent = (p3 - p1) * ((t2 - t1) / (t3 - t1));
        const QPointF bezierControls[] = {
            p1,
            p1 + firstTangent / 3.0,
            p2 - secondTangent / 3.0,
            p2};
        if (segment == 0) {
            for (const QPointF &control : bezierControls) {
                result.controlPoints.append(control);
            }
        } else {
            result.controlPoints.append(bezierControls[1]);
            result.controlPoints.append(bezierControls[2]);
            result.controlPoints.append(bezierControls[3]);
        }
        if (segment + 1 < segmentCount) {
            parameterStart += segmentParameters[segment] / totalParameterLength;
            fullKnots.append(parameterStart);
            fullKnots.append(parameterStart);
            fullKnots.append(parameterStart);
        }
    }
    for (int i = 0; i < 4; ++i) {
        fullKnots.append(1.0);
    }
    if (fullKnots.size() < 3) {
        return false;
    }
    result.knots.reserve(fullKnots.size() - 2);
    for (int i = 1; i + 1 < fullKnots.size(); ++i) {
        result.knots.append(fullKnots[i]);
    }
    if (!validateNurbsCurve(result)) {
        return false;
    }
    *curve = std::move(result);
    return true;
}

} // namespace classiCAD
