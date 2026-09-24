#pragma once

#include "nurbs_curve.h"

namespace classiCAD {

bool nurbsParameterDomain(const NurbsCurve2D &curve,
                          qreal *startParameter,
                          qreal *endParameter);

bool evaluateNurbsPoint(const NurbsCurve2D &curve,
                        qreal parameter,
                        QPointF *point);

} // namespace classiCAD
