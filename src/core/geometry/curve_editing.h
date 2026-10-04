#pragma once

#include "core/geometry/curve_geometry_data.h"
#include "core/geometry/nurbs_curve.h"

namespace classiCAD {

// Decomposes a valid NURBS curve into exact homogeneous Bezier spans without
// changing the source curve or its parameter domain.
bool rationalBezierSpansForCurve(const NurbsCurve2D &curve,
                                 QVector<RationalBezierSpan2D> *spans);

// Splits one homogeneous Bezier span at a normalized parameter in (0, 1).
bool splitRationalBezierSpan(
    const QVector<HomogeneousControlPoint2D> &source,
    qreal fraction,
    QVector<HomogeneousControlPoint2D> *left,
    QVector<HomogeneousControlPoint2D> *right);

// Extracts a parameter interval as a NURBS curve while preserving the source
// degree, rational state, and original parameter values.
bool trimNurbsCurve(const NurbsCurve2D &source,
                    qreal startParameter,
                    qreal endParameter,
                    NurbsCurve2D *trimmed);

bool reverseNurbsCurve(const NurbsCurve2D &source,
                       NurbsCurve2D *reversed);

} // namespace classiCAD
