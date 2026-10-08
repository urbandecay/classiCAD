#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

namespace classiCAD {

// Shared Rhino/openNURBS-style curve storage. u/v coordinates are stored in
// the work-plane basis; dimension-3 curves also store the CV's coordinate
// along that frame's normal. Rational CVs remain Euclidean and use separate
// positive weights, matching the openNURBS representation after frame lift.
struct NurbsCurve3D {
    int dimension = 2;
    int degree = 1;
    int order = 2;
    bool rational = false;
    QVector<QPointF> controlPoints;
    // Empty for dimension 2; one local normal coordinate per CV for dimension
    // 3. This keeps existing planar u/v algorithms source-compatible.
    QVector<double> normalCoordinates;
    QVector<double> weights;
    // Reduced Rhino/openNURBS knot array: the two redundant outer entries
    // from the mathematical full vector are not stored.
    QVector<double> knots;
};

// Compatibility alias for planar construction and algorithms being upgraded
// to handle spatial input.
using NurbsCurve2D = NurbsCurve3D;

QVector<double> expandedNurbsKnotVector(const NurbsCurve3D &curve);
bool validateNurbsCurve(const NurbsCurve3D &curve, QString *error = nullptr);
bool isNurbsCurvePlanarInWorkPlane(const NurbsCurve3D &curve);

} // namespace classiCAD
