#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

namespace classiCAD {

// Shared Rhino/openNURBS-style 2D curve storage. Rational control vertices
// remain Euclidean internally and are paired with positive weights.
struct NurbsCurve2D {
    int dimension = 2;
    int degree = 1;
    int order = 2;
    bool rational = false;
    QVector<QPointF> controlPoints;
    QVector<double> weights;
    // Reduced Rhino/openNURBS knot array: the two redundant outer entries
    // from the mathematical full vector are not stored.
    QVector<double> knots;
};

QVector<double> expandedNurbsKnotVector(const NurbsCurve2D &curve);
bool validateNurbsCurve(const NurbsCurve2D &curve, QString *error = nullptr);

} // namespace classiCAD
