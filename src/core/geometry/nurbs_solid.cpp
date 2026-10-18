#include "nurbs_solid.h"

#include "curve_evaluator.h"
#include "nurbs_surface_factory.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QLineF>
#include <QSet>

namespace classiCAD {
namespace {
Point3D subtract(const Point3D &a, const Point3D &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Point3D add(const Point3D &a, const Point3D &b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Point3D multiply(const Point3D &p, qreal scalar)
{
    return {p.x * scalar, p.y * scalar, p.z * scalar};
}
Point3D cross(const Point3D &a, const Point3D &b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
qreal length(const Point3D &p)
{
    return std::hypot(p.x, std::hypot(p.y, p.z));
}
Point3D normalized(const Point3D &p)
{
    const qreal magnitude = length(p);
    return magnitude > 1.0e-12 ? multiply(p, 1.0 / magnitude) : Point3D{};
}
qreal dot(const Point3D &a, const Point3D &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

int wallPanelCount(const NurbsCurve2D &curve)
{
    if (curve.degree != 1) return 1;
    int count = 0;
    for (int i = 0; i + 1 < curve.knots.size(); ++i)
        count += curve.knots[i + 1] > curve.knots[i];
    return count;
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
        QVector<Edge> rawEdges;
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
                rawEdges.append({a, b, 1});
            }
        }
        // Adjacent rectangular patches may meet along part of a longer edge.
        // Compare their exact geometric subsegments, including T junctions.
        for (const Edge &raw : rawEdges) {
            const Point3D direction = subtract(raw.b, raw.a);
            const qreal squaredLength = dot(direction, direction);
            QVector<qreal> parameters{0.0, 1.0};
            for (const Edge &other : rawEdges) {
                for (const Point3D &point : {other.a, other.b}) {
                    const qreal t = dot(subtract(point, raw.a), direction) / squaredLength;
                    if (t > 0.0 && t < 1.0 &&
                        close(point, add(raw.a, multiply(direction, t)))) {
                        parameters.append(t);
                    }
                }
            }
            std::sort(parameters.begin(), parameters.end());
            for (int part = 0; part + 1 < parameters.size(); ++part) {
                const Point3D a = add(raw.a, multiply(direction, parameters[part]));
                const Point3D b = add(raw.a, multiply(direction, parameters[part + 1]));
                if (close(a, b)) continue;
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

bool extrudeNurbsSolidFace(const NurbsExtrusionSolid3D &solid,
                           int faceIndex,
                           const Point3D &displacement,
                           NurbsExtrusionSolid3D *result,
                           bool manifold,
                           int *resultFaceIndex)
{
    if (resultFaceIndex != nullptr) *resultFaceIndex = faceIndex;
    if (result == nullptr || !validateNurbsSolid(solid)) return false;
    // Fill-created boxes retain exact rectangular UV trims on their caps.
    // Convert that construction to its equivalent connected patches before
    // editing any face, just as shared-corner editing does.
    NurbsExtrusionSolid3D boundary = solid;
    if (!materializeNurbsSolidBoundary(&boundary)) return false;
    const QVector<NurbsSurface3D> faces = nurbsSolidFaces(boundary);
    if (faceIndex < 0 || faceIndex >= faces.size()) return false;

    struct FacePlane {
        Point3D origin;
        Point3D normal;
    };
    QVector<FacePlane> facePlanes;
    facePlanes.reserve(faces.size());
    qreal geometryScale = 1.0e-12;
    const Point3D reference = faces.first().controlPoints.first();
    for (const NurbsSurface3D &face : faces) {
        if (!face.trimLoops.isEmpty() || face.rational ||
            face.degreeU != 1 || face.degreeV != 1 ||
            face.orderU != 2 || face.orderV != 2 ||
            face.controlVertexCountU != 2 ||
            face.controlVertexCountV != 2 ||
            face.controlPoints.size() != 4 || face.weights.size() != 4) {
            return false;
        }
        for (const Point3D &point : face.controlPoints) {
            geometryScale = std::max(geometryScale,
                                     length(subtract(point, reference)));
        }
        const Point3D u = subtract(face.controlPoints[2],
                                   face.controlPoints[0]);
        const Point3D v = subtract(face.controlPoints[1],
                                   face.controlPoints[0]);
        const Point3D naturalNormal = normalized(cross(u, v));
        if (length(naturalNormal) <= 1.0e-12) return false;
        for (const Point3D &point : face.controlPoints) {
            if (std::abs(dot(subtract(point, face.controlPoints[0]),
                             naturalNormal)) >
                std::max<qreal>(1.0e-12,
                                std::max(length(u), length(v)) * 1.0e-9)) {
                return false;
            }
        }
        if (nurbsSolidFaceReversed(boundary, facePlanes.size())) {
            facePlanes.append({face.controlPoints[0],
                               multiply(naturalNormal, -1.0)});
        } else {
            facePlanes.append({face.controlPoints[0], naturalNormal});
        }
    }

    const qreal shellTolerance = std::max<qreal>(1.0e-12,
                                                  geometryScale * 1.0e-9);

    const NurbsSurface3D &selectedFace = faces[faceIndex];
    const Point3D outwardNormal = facePlanes[faceIndex].normal;
    const qreal tolerance = shellTolerance;
    const bool selectedFaceReversed =
        nurbsSolidFaceReversed(boundary, faceIndex);
    const qreal displacementLength = length(displacement);
    const qreal signedDistance = dot(displacement, outwardNormal);
    const Point3D tangential = subtract(
        displacement, multiply(outwardNormal, signedDistance));
    if (displacementLength <= tolerance ||
        length(tangential) > std::max<qreal>(
            tolerance, displacementLength * 1.0e-8) ||
        std::abs(signedDistance) <= tolerance) {
        return false;
    }

    // Test the swept face against opposing patches in its footprint. A recess
    // is nonconvex; unrelated face half-spaces cannot bound its movement.
    const Point3D uAxis = normalized(subtract(selectedFace.controlPoints[2],
                                             selectedFace.controlPoints[0]));
    const Point3D vAxis = normalized(cross(outwardNormal, uAxis));
    const auto interval = [&](const NurbsSurface3D &face, const Point3D &axis) {
        QPair<qreal,qreal> bounds{std::numeric_limits<qreal>::max(),
                                 std::numeric_limits<qreal>::lowest()};
        for (const Point3D &point : face.controlPoints) {
            const qreal coordinate = dot(subtract(point, reference), axis);
            bounds.first = std::min(bounds.first, coordinate);
            bounds.second = std::max(bounds.second, coordinate);
        }
        return bounds;
    };
    const auto selectedU = interval(selectedFace, uAxis);
    const auto selectedV = interval(selectedFace, vAxis);
    for (int index = 0; index < faces.size(); ++index) {
        if (index == faceIndex || dot(facePlanes[index].normal, outwardNormal) > -1.0 + 1.0e-8)
            continue;
        const auto otherU = interval(faces[index], uAxis);
        const auto otherV = interval(faces[index], vAxis);
        if (std::min(selectedU.second, otherU.second) - std::max(selectedU.first, otherU.first) <= tolerance ||
            std::min(selectedV.second, otherV.second) - std::max(selectedV.first, otherV.first) <= tolerance)
            continue;
        const qreal separation = dot(subtract(facePlanes[index].origin,
                                              selectedFace.controlPoints[0]), outwardNormal);
        if ((signedDistance < 0.0 && separation < -tolerance && signedDistance <= separation + tolerance) ||
            (signedDistance > 0.0 && separation > tolerance && signedDistance >= separation - tolerance))
            return false;
    }

    NurbsExtrusionSolid3D candidate = boundary;
    candidate.boundaryFaces = faces;
    candidate.boundaryFaceReversed.clear();
    candidate.boundaryFaceReversed.reserve(faces.size() + 4);
    for (int index = 0; index < faces.size(); ++index) {
        candidate.boundaryFaceReversed.append(
            nurbsSolidFaceReversed(boundary, index));
    }

    bool fillsRecess = false;
    const int boundaryOrder[] = {0,2,3,1};
    if (signedDistance > 0.0) {
        for (int index=0; index<faces.size() && !fillsRecess; ++index) {
            if (index==faceIndex || std::abs(dot(facePlanes[index].normal,outwardNormal))>1.0e-4)
                continue;
            bool extendsAhead=false;
            for (const Point3D &point : faces[index].controlPoints)
                extendsAhead |= dot(subtract(point,selectedFace.controlPoints[0]),outwardNormal)>tolerance;
            if (!extendsAhead) continue;
            for (int edge=0; edge<4 && !fillsRecess; ++edge) {
                const Point3D a=selectedFace.controlPoints[boundaryOrder[edge]];
                const Point3D b=selectedFace.controlPoints[boundaryOrder[(edge+1)%4]];
                const Point3D axis=normalized(subtract(b,a));
                const qreal extent=length(subtract(b,a));
                for (int side=0; side<4; ++side) {
                    const Point3D p=faces[index].controlPoints[boundaryOrder[side]];
                    const Point3D q=faces[index].controlPoints[boundaryOrder[(side+1)%4]];
                    const qreal first=dot(subtract(p,a),axis), last=dot(subtract(q,a),axis);
                    if (length(subtract(subtract(p,a),multiply(axis,first)))<=tolerance &&
                        length(subtract(subtract(q,a),multiply(axis,last)))<=tolerance &&
                        std::min(extent,std::max(first,last))-std::max<qreal>(0,std::min(first,last))>tolerance)
                        fillsRecess=true;
                }
            }
        }
    }
    if (manifold || signedDistance < 0.0 || fillsRecess) {
        // Edit edge neighbors only. Coplanar neighbors stay in place and gain
        // a step wall; orthogonal neighbors shorten/extend to the moving face.
        // Faces touching just one corner must keep their original geometry.
        for (Point3D &point : candidate.boundaryFaces[faceIndex].controlPoints)
            point = add(point, displacement);
        const int forward[] = {0,2,3,1};
        const int reverse[] = {0,1,3,2};
        const int *perimeter = selectedFaceReversed ? reverse : forward;
        QVector<Point3D> expectedNormals;
        QVector<bool> resizedNeighbor(faces.size(),false);
        for (const FacePlane &plane : facePlanes) expectedNormals.append(plane.normal);
        QSet<int> editedNeighbors;
        for (int edge = 0; edge < 4; ++edge) {
            const Point3D &a = selectedFace.controlPoints[perimeter[edge]];
            const Point3D &b = selectedFace.controlPoints[perimeter[(edge+1)%4]];
            const Point3D direction = subtract(b,a);
            const qreal edgeLength = length(direction);
            const Point3D edgeAxis = multiply(direction,1.0/edgeLength);
            QVector<QPair<qreal,qreal>> coverage;
            for (int index = 0; index < faces.size(); ++index) {
                if (index == faceIndex) continue;
                const auto &neighbor = faces[index];
                for (int side = 0; side < 4; ++side) {
                    const int i = forward[side], j = forward[(side+1)%4];
                    const Point3D p = neighbor.controlPoints[i], q = neighbor.controlPoints[j];
                    const qreal pDistance = dot(subtract(p,a),edgeAxis);
                    const qreal qDistance = dot(subtract(q,a),edgeAxis);
                    if (length(subtract(subtract(p,a),multiply(edgeAxis,pDistance))) > tolerance ||
                        length(subtract(subtract(q,a),multiply(edgeAxis,qDistance))) > tolerance) continue;
                    const qreal low = std::max<qreal>(0,std::min(pDistance,qDistance));
                    const qreal high = std::min(edgeLength,std::max(pDistance,qDistance));
                    if (high-low <= tolerance) continue;
                    coverage.append({low,high});
                    const Point3D start = add(a,multiply(edgeAxis,low));
                    const Point3D end = add(a,multiply(edgeAxis,high));
                    const qreal alignment = dot(facePlanes[index].normal,outwardNormal);
                    if (std::abs(alignment) < 1.0e-4) {
                        if (editedNeighbors.contains(index)) return false;
                        editedNeighbors.insert(index);
                        // Split a longer neighboring edge at the selected
                        // region's limits before moving only its shared part.
                        const bool variesU = (i/2 != j/2);
                        const Point3D edgeOrigin=neighbor.controlPoints[std::min(i,j)];
                        const Point3D axis = subtract(neighbor.controlPoints[std::max(i,j)],edgeOrigin);
                        const qreal squared = dot(axis,axis);
                        const qreal first = dot(subtract(start,edgeOrigin),axis)/squared;
                        const qreal last = dot(subtract(end,edgeOrigin),axis)/squared;
                        const qreal t0 = std::clamp(std::min(first,last),qreal(0),qreal(1));
                        const qreal t1 = std::clamp(std::max(first,last),qreal(0),qreal(1));
                        QVector<qreal> cuts{0,t0,t1,1};
                        std::sort(cuts.begin(),cuts.end());
                        bool replaced = false;
                        const auto pointAt = [&](qreal u,qreal v) {
                            return add(add(multiply(neighbor.controlPoints[0],(1-u)*(1-v)),
                                           multiply(neighbor.controlPoints[1],(1-u)*v)),
                                       add(multiply(neighbor.controlPoints[2],u*(1-v)),
                                           multiply(neighbor.controlPoints[3],u*v)));
                        };
                        for (int part=0; part+1<cuts.size(); ++part) {
                            const qreal lo=cuts[part], hi=cuts[part+1];
                            if ((hi-lo)*length(axis) <= tolerance) continue;
                            NurbsSurface3D patch=neighbor;
                            patch.controlPoints = variesU
                                ? QVector<Point3D>{pointAt(lo,0),pointAt(lo,1),pointAt(hi,0),pointAt(hi,1)}
                                : QVector<Point3D>{pointAt(0,lo),pointAt(0,hi),pointAt(1,lo),pointAt(1,hi)};
                            auto &knots=variesU?patch.knotsU:patch.knotsV;
                            const qreal domainStart=knots[0], span=knots[1]-knots[0];
                            knots={domainStart+span*lo,domainStart+span*hi};
                            if ((lo+hi)*0.5 > t0 && (lo+hi)*0.5 < t1) {
                                for (Point3D &point : patch.controlPoints) {
                                    const qreal along=dot(subtract(point,a),edgeAxis);
                                    if (length(subtract(subtract(point,a),multiply(edgeAxis,along))) <= tolerance)
                                        point=add(point,displacement);
                                }
                            }
                            if (!replaced) {
                                candidate.boundaryFaces[index]=std::move(patch);
                                resizedNeighbor[index]=true;
                                replaced=true;
                            } else {
                                candidate.boundaryFaces.append(std::move(patch));
                                candidate.boundaryFaceReversed.append(nurbsSolidFaceReversed(boundary,index));
                                expectedNormals.append(facePlanes[index].normal);
                                resizedNeighbor.append(true);
                            }
                        }
                    } else if (alignment > 1.0-1.0e-8) {
                        NurbsSurface3D wall;
                        wall.controlVertexCountU=wall.controlVertexCountV=2;
                        wall.controlPoints={start,add(start,displacement),end,add(end,displacement)};
                        wall.weights={1,1,1,1};
                        wall.knotsU=wall.knotsV={0,1};
                        candidate.boundaryFaces.append(std::move(wall));
                        candidate.boundaryFaceReversed.append(false);
                        expectedNormals.append(normalized(cross(subtract(end,start),displacement)));
                        resizedNeighbor.append(false);
                    } else return false;
                }
            }
            std::sort(coverage.begin(),coverage.end());
            qreal covered=0;
            for (const auto &part : coverage) {
                if (std::abs(part.first-covered)>tolerance) return false;
                covered=part.second;
            }
            if (std::abs(covered-edgeLength)>tolerance) return false;
        }
        QVector<NurbsSurface3D> retainedFaces;
        QVector<bool> retainedReversed;
        int movedFace = -1;
        for (int index = 0; index < candidate.boundaryFaces.size(); ++index) {
            const auto &face = candidate.boundaryFaces[index];
            const Point3D u=subtract(face.controlPoints[2],face.controlPoints[0]);
            const Point3D v=subtract(face.controlPoints[1],face.controlPoints[0]);
            const Point3D area=cross(u,v);
            if (length(area) <= tolerance*std::max(length(u),length(v))) {
                // At a neighboring cap's height, the connecting step wall
                // disappears. Past that height its natural winding reverses.
                if (!resizedNeighbor[index] || index == faceIndex) return false;
                Point3D axis=length(u)>length(v)?u:v;
                const Point3D diagonal=subtract(face.controlPoints[3],face.controlPoints[0]);
                if (length(diagonal)>length(axis)) axis=diagonal;
                axis=normalized(axis);
                for (const Point3D &point : face.controlPoints) {
                    const Point3D delta=subtract(point,face.controlPoints[0]);
                    if (length(subtract(delta,multiply(axis,dot(delta,axis))))>tolerance)
                        return false;
                }
                continue;
            }
            Point3D normal = normalized(area);
            if (candidate.boundaryFaceReversed[index]) normal = multiply(normal, -1);
            const qreal orientation=dot(normal,expectedNormals[index]);
            if (orientation <= 0.0 &&
                (!resizedNeighbor[index] || orientation > -1.0+1.0e-8)) return false;
            for (const Point3D &point : face.controlPoints) {
                if (std::abs(dot(subtract(point, face.controlPoints[0]), normal)) >
                    tolerance) return false;
            }
            if (index == faceIndex) movedFace=retainedFaces.size();
            retainedFaces.append(face);
            retainedReversed.append(candidate.boundaryFaceReversed[index]);
        }
        if (movedFace < 0) return false;
        candidate.boundaryFaces=std::move(retainedFaces);
        candidate.boundaryFaceReversed=std::move(retainedReversed);
        if (!validateNurbsSolid(candidate)) return false;
        if (resultFaceIndex != nullptr) *resultFaceIndex=movedFace;
        *result = std::move(candidate);
        return true;
    }

    NurbsSurface3D movedFace = selectedFace;
    for (Point3D &point : movedFace.controlPoints) {
        point = add(point, displacement);
    }
    candidate.boundaryFaces[faceIndex] = movedFace;
    candidate.boundaryFaceReversed[faceIndex] = selectedFaceReversed;

    const int forwardPerimeter[] = {0, 2, 3, 1};
    const int reversedPerimeter[] = {0, 1, 3, 2};
    const int *perimeter = selectedFaceReversed
                               ? reversedPerimeter : forwardPerimeter;
    for (int edgeIndex = 0; edgeIndex < 4; ++edgeIndex) {
        const Point3D &start = selectedFace.controlPoints[
            perimeter[edgeIndex]];
        const Point3D &end = selectedFace.controlPoints[
            perimeter[(edgeIndex + 1) % 4]];
        NurbsSurface3D wall;
        wall.degreeU = 1;
        wall.degreeV = 1;
        wall.orderU = 2;
        wall.orderV = 2;
        wall.controlVertexCountU = 2;
        wall.controlVertexCountV = 2;
        wall.controlPoints = {start, add(start, displacement),
                              end, add(end, displacement)};
        wall.weights = {1.0, 1.0, 1.0, 1.0};
        wall.knotsU = {0.0, 1.0};
        wall.knotsV = {0.0, 1.0};
        candidate.boundaryFaces.append(std::move(wall));
        // The oriented perimeter and displacement already determine winding,
        // including inward movement; do not reverse these faces a second time.
        candidate.boundaryFaceReversed.append(false);
    }

    if (!validateNurbsSolid(candidate)) return false;
    *result = std::move(candidate);
    return true;
}

bool nurbsSolidFaceReversed(const NurbsExtrusionSolid3D &solid, int faceIndex)
{
    if (!solid.boundaryFaces.isEmpty()) {
        return faceIndex >= 0 && faceIndex < solid.boundaryFaceReversed.size()
                   ? solid.boundaryFaceReversed[faceIndex] : false;
    }
    int wallCount = solid.baseSurface.trimLoops.isEmpty() ? 4 : 0;
    for (const auto &loop : solid.baseSurface.trimLoops)
        wallCount += wallPanelCount(loop.curve);
    if (faceIndex < 0 || faceIndex >= 2 + wallCount) return false;
    WorkPlaneFrame frame;
    if (!nurbsSolidBaseFrame(solid.baseSurface, &frame)) return false;
    const bool negative = dot(solid.displacement, frame.normal) < 0;
    if (faceIndex == 0) return !negative;
    if (faceIndex == 1) return negative;
    if (solid.baseSurface.trimLoops.isEmpty()) return negative;
    int wallIndex = faceIndex - 2;
    int loopIndex = 0;
    while (loopIndex < solid.baseSurface.trimLoops.size() &&
           wallIndex >= wallPanelCount(solid.baseSurface.trimLoops[loopIndex].curve)) {
        wallIndex -= wallPanelCount(solid.baseSurface.trimLoops[loopIndex].curve);
        ++loopIndex;
    }
    if (loopIndex >= solid.baseSurface.trimLoops.size()) return false;
    const auto &loop = solid.baseSurface.trimLoops[loopIndex];
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
        if (wall.degreeU == 1) {
            // Each linear boundary span is a distinct exact wall face. Keep
            // the stored base/vector construction and the span's domain and
            // weights; picking and editing must not select the entire wrap.
            for (int span = 0; span + 1 < wall.controlVertexCountU; ++span) {
                if (wall.knotsU[span + 1] <= wall.knotsU[span]) continue;
                NurbsSurface3D panel = wall;
                panel.controlVertexCountU = 2;
                panel.controlPoints = {wall.controlPoints[span * 2],
                                       wall.controlPoints[span * 2 + 1],
                                       wall.controlPoints[span * 2 + 2],
                                       wall.controlPoints[span * 2 + 3]};
                panel.weights = {wall.weights[span * 2],
                                 wall.weights[span * 2 + 1],
                                 wall.weights[span * 2 + 2],
                                 wall.weights[span * 2 + 3]};
                panel.knotsU = {wall.knotsU[span], wall.knotsU[span + 1]};
                faces.append(std::move(panel));
            }
        } else {
            faces.append(std::move(wall));
        }
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
