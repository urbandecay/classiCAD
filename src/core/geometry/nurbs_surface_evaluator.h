#pragma once

#include "nurbs_surface.h"

#include <QVector>

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

// Blender's legacy surface evaluator precomputes the one-dimensional U and V
// basis functions for its regular sample grid before combining them with the
// control net. This keeps the same tensor-product approach available when a
// fixed set of UV samples is reevaluated after control-point edits.
class PreparedNurbsSurfaceBasisGrid final {
public:
    bool prepare(const NurbsSurface3D &surface,
                 const QVector<QPointF> &parameters);
    bool isValid() const;
    int sampleCount() const;
    bool evaluate(const NurbsSurface3D &surface,
                  int sampleIndex,
                  Point3D *point) const;

private:
    struct AxisBasisRow {
        int firstControlVertex = 0;
        int valuesOffset = 0;
        int valueCount = 0;
    };
    struct SampleBasisIndices {
        int u = -1;
        int v = -1;
    };

    int degreeU_ = 0;
    int degreeV_ = 0;
    int controlVertexCountU_ = 0;
    int controlVertexCountV_ = 0;
    bool rational_ = false;
    QVector<double> knotsU_;
    QVector<double> knotsV_;
    QVector<double> weights_;
    QVector<qreal> uParameters_;
    QVector<qreal> vParameters_;
    QVector<AxisBasisRow> uRows_;
    QVector<AxisBasisRow> vRows_;
    QVector<qreal> uBasisValues_;
    QVector<qreal> vBasisValues_;
    QVector<SampleBasisIndices> sampleIndices_;
    bool valid_ = false;
};

} // namespace classiCAD
