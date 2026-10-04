#pragma once

#include "nurbs_surface.h"

namespace classiCAD {

// Validated, immutable surface snapshot with expanded knots and parameter
// domains prepared for repeated point evaluation. QVector's implicit sharing
// keeps the snapshot independent of later document edits without copying each
// control point up front.
class PreparedNurbsSurfaceEvaluator final {
public:
    bool prepare(const NurbsSurface3D &surface);
    bool isValid() const;
    bool parameterDomains(qreal *uStart,
                          qreal *uEnd,
                          qreal *vStart,
                          qreal *vEnd) const;
    bool evaluate(qreal u, qreal v, Point3D *point) const;

private:
    NurbsSurface3D surface_;
    QVector<double> fullKnotsU_;
    QVector<double> fullKnotsV_;
    qreal uStart_ = 0.0;
    qreal uEnd_ = 0.0;
    qreal vStart_ = 0.0;
    qreal vEnd_ = 0.0;
    bool valid_ = false;
};

} // namespace classiCAD
