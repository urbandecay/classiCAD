#include "nurbs_curve.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

QVector<double> expandedNurbsKnotVector(const NurbsCurve3D &curve)
{
    QVector<double> fullKnots;
    if (curve.knots.isEmpty()) {
        return fullKnots;
    }

    fullKnots.reserve(curve.knots.size() + 2);
    fullKnots.append(curve.knots.first());
    fullKnots += curve.knots;
    fullKnots.append(curve.knots.last());
    return fullKnots;
}

bool validateNurbsCurve(const NurbsCurve3D &curve, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (curve.dimension != 2 && curve.dimension != 3) {
        return fail(QStringLiteral("NURBS dimension must be 2 or 3"));
    }
    if (curve.degree < 1 || curve.order != curve.degree + 1) {
        return fail(QStringLiteral("NURBS order must equal degree plus one"));
    }
    if (curve.controlPoints.size() <= curve.degree) {
        return fail(QStringLiteral("NURBS needs more control points than its degree"));
    }
    if (curve.knots.size() != curve.controlPoints.size() + curve.order - 2) {
        return fail(QStringLiteral("NURBS reduced knot count is invalid"));
    }
    if (curve.weights.size() != curve.controlPoints.size()) {
        return fail(QStringLiteral("NURBS weights must match control points"));
    }
    if ((curve.dimension == 2 && !curve.normalCoordinates.isEmpty()) ||
        (curve.dimension == 3 &&
         curve.normalCoordinates.size() != curve.controlPoints.size())) {
        return fail(QStringLiteral("NURBS normal coordinates must match its dimension and CV count"));
    }

    for (const QPointF &point : curve.controlPoints) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            return fail(QStringLiteral("NURBS control point is not finite"));
        }
    }
    for (const double coordinate : curve.normalCoordinates) {
        if (!std::isfinite(coordinate)) {
            return fail(QStringLiteral("NURBS normal coordinate is not finite"));
        }
    }
    for (int index = 0; index < curve.knots.size(); ++index) {
        if (!std::isfinite(curve.knots[index]) ||
            (index > 0 && curve.knots[index] < curve.knots[index - 1])) {
            return fail(QStringLiteral("NURBS knots must be finite and nondecreasing"));
        }
    }
    for (const double weight : curve.weights) {
        if (!std::isfinite(weight) || weight <= 0.0) {
            return fail(QStringLiteral("NURBS weights must be positive and finite"));
        }
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const int endKnotIndex = curve.controlPoints.size();
    if (fullKnots.size() != curve.controlPoints.size() + curve.degree + 1 ||
        curve.degree < 0 || endKnotIndex >= fullKnots.size() ||
        fullKnots[curve.degree] >= fullKnots[endKnotIndex]) {
        return fail(QStringLiteral("NURBS parameter domain is invalid"));
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool isNurbsCurvePlanarInWorkPlane(const NurbsCurve3D &curve)
{
    if (!validateNurbsCurve(curve)) {
        return false;
    }
    if (curve.dimension == 2 || curve.normalCoordinates.isEmpty()) {
        return true;
    }
    const double planeCoordinate = curve.normalCoordinates.first();
    for (const double coordinate : curve.normalCoordinates) {
        const double scale = std::max({1.0, std::abs(planeCoordinate),
                                      std::abs(coordinate)});
        if (std::abs(coordinate - planeCoordinate) > 1.0e-9 * scale) {
            return false;
        }
    }
    return true;
}

} // namespace classiCAD
