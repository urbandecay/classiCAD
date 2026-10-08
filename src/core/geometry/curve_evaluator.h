#pragma once

#include "nurbs_curve.h"
#include "work_plane.h"

namespace classiCAD {

bool nurbsParameterDomain(const NurbsCurve2D &curve,
                          qreal *startParameter,
                          qreal *endParameter);

bool nurbsCurveEndpoints(const NurbsCurve2D &curve,
                        QPointF *start,
                        QPointF *end);

bool evaluateNurbsPoint(const NurbsCurve2D &curve,
                        qreal parameter,
                        QPointF *point);

// Evaluate local (u, v, w) coordinates. For dimension-2 curves, w is zero.
bool evaluateNurbsPoint3D(const NurbsCurve3D &curve,
                          qreal parameter,
                          Point3D *point);

bool evaluateNurbsDerivative(const NurbsCurve2D &curve,
                             qreal parameter,
                             QPointF *derivative);

} // namespace classiCAD
