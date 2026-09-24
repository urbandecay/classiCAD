#include "nurbs_curve.h"

#include <cmath>

namespace classiCAD {

QVector<double> expandedNurbsKnotVector(const NurbsCurve2D &curve)
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

bool validateNurbsCurve(const NurbsCurve2D &curve, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (curve.dimension != 2) {
        return fail(QStringLiteral("NURBS dimension must be 2"));
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

    for (const QPointF &point : curve.controlPoints) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            return fail(QStringLiteral("NURBS control point is not finite"));
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

} // namespace classiCAD
