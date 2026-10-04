#pragma once

#include "core/document/shape.h"

namespace classiCAD {

bool isSubdividableCurveShape(const Shape &shape);
bool subdivisionCurveForShape(const Shape &shape,
                              Shape::NurbsCurve2D *curve);
QVector<double> equalArcLengthSubdivisionParameters(const Shape &shape,
                                                    int sections);

} // namespace classiCAD
