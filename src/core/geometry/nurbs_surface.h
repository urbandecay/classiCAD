#pragma once

#include "nurbs_curve.h"
#include "work_plane.h"

#include <QVector>

namespace classiCAD {

// A Brep-style trim loop is a closed NURBS curve in the surface's UV
// parameter space. The first loop on a face is the outside boundary; further
// loops marked as holes remove regions from that face.
struct NurbsSurfaceTrimLoop {
    NurbsCurve2D curve;
    bool isHole = false;
};

// Rhino/openNURBS-compatible tensor-product NURBS surface data. Control
// vertices are Euclidean world-space XYZ positions; a rational surface stores
// one positive weight per vertex. The CV grid is flattened with V varying
// fastest: index = uIndex * controlVertexCountV + vIndex.
struct NurbsSurface3D {
    int dimension = 3;
    int degreeU = 1;
    int degreeV = 1;
    int orderU = 2;
    int orderV = 2;
    int controlVertexCountU = 0;
    int controlVertexCountV = 0;
    bool rational = false;
    QVector<Point3D> controlPoints;
    QVector<double> weights;
    // Rhino reduced knot convention, stored independently in each direction.
    QVector<double> knotsU;
    QVector<double> knotsV;
    // Empty means the full rectangular parameter domain is visible. Otherwise
    // these UV curves trim the surface like loops on an openNURBS Brep face.
    QVector<NurbsSurfaceTrimLoop> trimLoops;
};

// Exact untrimmed isoparametric boundary in the identity world XY frame:
// curve CV xy stores world xy and normalCoordinates stores world z.
struct NurbsSurfaceBoundaryCurve {
    NurbsCurve3D curve;
    QVector<int> sourceControlPointIndices;
    Point3D startPoint;
    Point3D endPoint;
};

// V minimum, U maximum, V maximum, U minimum; each curve has an increasing
// parameter domain. Trimmed or invalid surfaces return no boundaries.
QVector<NurbsSurfaceBoundaryCurve> nurbsSurfaceBoundaryCurves(
    const NurbsSurface3D &surface);

QVector<double> expandedNurbsSurfaceKnotVector(const QVector<double> &knots);
bool validateNurbsSurface(const NurbsSurface3D &surface,
                          QString *error = nullptr);
bool nurbsSurfaceParameterDomains(const NurbsSurface3D &surface,
                                  qreal *uStart,
                                  qreal *uEnd,
                                  qreal *vStart,
                                  qreal *vEnd);
bool evaluateNurbsSurfacePoint(const NurbsSurface3D &surface,
                               qreal u,
                               qreal v,
                               Point3D *point);
QVector<QPointF> sampleNurbsSurfaceTrimLoop(
    const NurbsSurfaceTrimLoop &loop,
    int sampleCount = 128);
bool nurbsSurfaceParameterInsideTrim(const NurbsSurface3D &surface,
                                     qreal u,
                                     qreal v);

} // namespace classiCAD
