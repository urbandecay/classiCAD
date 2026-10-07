#include "nurbs_solid.h"

#include "curve_evaluator.h"
#include "nurbs_surface_factory.h"

#include <cmath>
#include <QLineF>
#include <QSet>

namespace classiCAD {
namespace {
Point3D subtract(const Point3D &a, const Point3D &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
qreal length(const Point3D &p)
{
    return std::hypot(p.x, std::hypot(p.y, p.z));
}
qreal dot(const Point3D &a, const Point3D &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
}

bool nurbsSolidBaseFrame(const NurbsSurface3D &surface, WorkPlaneFrame *frame)
{
    if (frame == nullptr || !validateNurbsSurface(surface) || surface.rational ||
        surface.degreeU != 1 || surface.degreeV != 1 ||
        surface.controlVertexCountU != 2 || surface.controlVertexCountV != 2) {
        return false;
    }
    const auto &cv = surface.controlPoints;
    const Point3D u = subtract(cv[2], cv[0]);
    const Point3D v = subtract(cv[1], cv[0]);
    const qreal scale = std::max(length(u), length(v));
    const Point3D cornerError = subtract(subtract(cv[3], cv[2]), v);
    const Point3D normal{u.y * v.z - u.z * v.y,
                          u.z * v.x - u.x * v.z,
                          u.x * v.y - u.y * v.x};
    if (scale <= 1.0e-12 || length(cornerError) > scale * 1.0e-9 ||
        length(normal) <= length(u) * length(v) * 1.0e-10) {
        return false;
    }
    *frame = makeWorkPlaneFrameFromNormal(cv[0], normal, u);
    return isValidWorkPlaneFrame(*frame);
}

bool validateNurbsSolid(const NurbsExtrusionSolid3D &solid)
{
    if (!solid.boundaryFaces.isEmpty()) {
        if (solid.boundaryFaceReversed.size() != solid.boundaryFaces.size())
            return false;
        struct Edge { Point3D a; Point3D b; int count = 1; };
        QVector<Edge> edges;
        const auto close = [](const Point3D &a, const Point3D &b) {
            return length(subtract(a, b)) < 1.0e-7;
        };
        for (const NurbsSurface3D &face : solid.boundaryFaces) {
            if (!validateNurbsSurface(face) || face.rational ||
                face.degreeU != 1 || face.degreeV != 1 ||
                face.controlVertexCountU != 2 || face.controlVertexCountV != 2 ||
                !face.trimLoops.isEmpty()) return false;
            const int perimeter[] = {0, 2, 3, 1, 0};
            for (int i = 0; i < 4; ++i) {
                const Point3D a = face.controlPoints[perimeter[i]];
                const Point3D b = face.controlPoints[perimeter[i + 1]];
                if (close(a, b)) return false;
                bool found = false;
                for (Edge &edge : edges) {
                    if ((close(a, edge.a) && close(b, edge.b)) ||
                        (close(a, edge.b) && close(b, edge.a))) {
                        ++edge.count;
                        found = true;
                        break;
                    }
                }
                if (!found) edges.append({a, b, 1});
            }
        }
        for (const Edge &edge : edges) if (edge.count != 2) return false;
        return !edges.isEmpty();
    }
    WorkPlaneFrame frame;
    if (!nurbsSolidBaseFrame(solid.baseSurface, &frame) ||
        !std::isfinite(solid.displacement.x) ||
        !std::isfinite(solid.displacement.y) ||
        !std::isfinite(solid.displacement.z)) {
        return false;
    }
    const qreal scale = std::max(length(subtract(solid.baseSurface.controlPoints[2],
                                                frame.origin)),
                                 length(subtract(solid.baseSurface.controlPoints[1],
                                                frame.origin)));
    // An in-plane sweep has zero volume and cannot be a solid.
    return std::abs(dot(solid.displacement, frame.normal)) >
           std::max(scale, length(solid.displacement)) * 1.0e-10;
}

bool makeNurbsExtrusionSolid(const NurbsSurface3D &surface,
                             const Point3D &displacement,
                             NurbsExtrusionSolid3D *solid)
{
    const NurbsExtrusionSolid3D result{surface, displacement, {}, {}};
    if (solid == nullptr || !validateNurbsSolid(result)) return false;
    *solid = result;
    return true;
}

bool nurbsSolidFaceReversed(const NurbsExtrusionSolid3D &solid, int faceIndex)
{
    if (!solid.boundaryFaces.isEmpty()) {
        return faceIndex >= 0 && faceIndex < solid.boundaryFaceReversed.size()
                   ? solid.boundaryFaceReversed[faceIndex] : false;
    }
    const int wallCount = solid.baseSurface.trimLoops.isEmpty()
                              ? 1 : solid.baseSurface.trimLoops.size();
    if (faceIndex < 0 || faceIndex >= 2 + wallCount) return false;
    WorkPlaneFrame frame;
    if (!nurbsSolidBaseFrame(solid.baseSurface, &frame)) return false;
    const bool negative = dot(solid.displacement, frame.normal) < 0;
    if (faceIndex == 0) return !negative;
    if (faceIndex == 1) return negative;
    if (solid.baseSurface.trimLoops.isEmpty()) return negative;
    const auto &loop = solid.baseSurface.trimLoops[faceIndex - 2];
    const auto samples = sampleNurbsSurfaceTrimLoop(loop, 128);
    qreal twiceArea = 0;
    for (int i = 0; i < samples.size(); ++i) {
        const auto &a = samples[i];
        const auto &b = samples[(i + 1) % samples.size()];
        twiceArea += a.x()*b.y() - b.x()*a.y();
    }
    return ((twiceArea > 0) == loop.isHole) != negative;
}

QVector<NurbsSurface3D> nurbsSolidFaces(const NurbsExtrusionSolid3D &solid)
{
    if (!validateNurbsSolid(solid)) return {};
    if (!solid.boundaryFaces.isEmpty()) return solid.boundaryFaces;
    const NurbsSurface3D &base = solid.baseSurface;
    NurbsSurface3D top = base;
    for (Point3D &p : top.controlPoints) {
        p.x += solid.displacement.x;
        p.y += solid.displacement.y;
        p.z += solid.displacement.z;
    }
    QVector<NurbsSurface3D> faces{base, top};
    QVector<NurbsSurfaceTrimLoop> loops = base.trimLoops;
    qreal u0, u1, v0, v1;
    if (!nurbsSurfaceParameterDomains(base, &u0, &u1, &v0, &v1)) return {};
    if (loops.isEmpty()) {
        NurbsSurfaceTrimLoop perimeter;
        perimeter.curve.degree = 1;
        perimeter.curve.order = 2;
        perimeter.curve.controlPoints = {{u0,v0}, {u1,v0}, {u1,v1}, {u0,v1}, {u0,v0}};
        perimeter.curve.weights = {1,1,1,1,1};
        perimeter.curve.knots = {0,1,2,3,4};
        loops.append(perimeter);
    }
    WorkPlaneFrame frame;
    nurbsSolidBaseFrame(base, &frame);
    const Point3D u = subtract(base.controlPoints[2], base.controlPoints[0]);
    const Point3D v = subtract(base.controlPoints[1], base.controlPoints[0]);
    for (const NurbsSurfaceTrimLoop &loop : loops) {
        NurbsCurve2D boundary = loop.curve;
        for (QPointF &uv : boundary.controlPoints) {
            const qreal a = (uv.x() - u0) / (u1 - u0);
            const qreal b = (uv.y() - v0) / (v1 - v0);
            const Point3D world{frame.origin.x + a*u.x + b*v.x,
                                 frame.origin.y + a*u.y + b*v.y,
                                 frame.origin.z + a*u.z + b*v.z};
            uv = worldPointToWorkPlaneFrame(world, frame);
        }
        NurbsSurface3D wall;
        if (!makeNurbsExtrusionSurface(boundary, frame, solid.displacement, &wall)) return {};
        faces.append(wall);
    }
    return faces;
}

bool materializeNurbsSolidBoundary(NurbsExtrusionSolid3D *solid)
{
    if (solid == nullptr || !validateNurbsSolid(*solid)) return false;
    if (!solid->boundaryFaces.isEmpty()) return true;
    const QVector<NurbsSurface3D> derived = nurbsSolidFaces(*solid);
    NurbsExtrusionSolid3D candidate = *solid;
    for (int faceIndex = 0; faceIndex < derived.size(); ++faceIndex) {
        NurbsSurface3D face = derived[faceIndex];
        const bool reversed = nurbsSolidFaceReversed(*solid, faceIndex);
        if (faceIndex < 2) {
            // Only a rectangular parameter-domain trim can be removed
            // exactly. Other trims need a more general boundary topology.
            qreal u0, u1, v0, v1;
            if (!nurbsSurfaceParameterDomains(face, &u0, &u1, &v0, &v1)) return false;
            if (!face.trimLoops.isEmpty()) {
                if (face.trimLoops.size() != 1 || face.trimLoops[0].isHole ||
                    face.trimLoops[0].curve.degree != 1) return false;
                const auto &points = face.trimLoops[0].curve.controlPoints;
                const QVector<QPointF> corners{{u0,v0},{u1,v0},{u1,v1},{u0,v1}};
                QSet<int> seen;
                for (const QPointF &point : points) {
                    int match = -1;
                    for (int i = 0; i < corners.size(); ++i)
                        if (QLineF(point, corners[i]).length() < 1.0e-9) match = i;
                    if (match < 0) return false;
                    seen.insert(match);
                }
                if (seen.size() != 4) return false;
                face.trimLoops.clear();
            }
            candidate.boundaryFaces.append(face);
            candidate.boundaryFaceReversed.append(reversed);
        } else {
            if (face.rational || face.degreeU != 1 || face.degreeV != 1 ||
                face.controlVertexCountV != 2 || !face.trimLoops.isEmpty()) return false;
            for (int u = 0; u + 1 < face.controlVertexCountU; ++u) {
                NurbsSurface3D panel = face;
                panel.controlVertexCountU = 2;
                panel.controlPoints = {face.controlPoints[u * 2],
                    face.controlPoints[u * 2 + 1], face.controlPoints[u * 2 + 2],
                    face.controlPoints[u * 2 + 3]};
                panel.weights = {1,1,1,1};
                panel.knotsU = {face.knotsU[u], face.knotsU[u + 1]};
                candidate.boundaryFaces.append(panel);
                candidate.boundaryFaceReversed.append(reversed);
            }
        }
    }
    if (!validateNurbsSolid(candidate)) return false;
    *solid = std::move(candidate);
    return true;
}

} // namespace classiCAD
