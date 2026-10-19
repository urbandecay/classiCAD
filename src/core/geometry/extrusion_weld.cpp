#include "extrusion_weld.h"

#include "geometry_transform.h"
#include "shape_mapping.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {
Point3D plus(Point3D a, Point3D b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Point3D minus(Point3D a, Point3D b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Point3D times(Point3D a, qreal t) { return {a.x*t,a.y*t,a.z*t}; }
qreal dot(Point3D a, Point3D b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
qreal distance2(Point3D a, Point3D b) { const auto d=minus(a,b); return dot(d,d); }
Point3D curvePoint(const NurbsCurve3D &c, int i, const WorkPlaneFrame &f)
{
    return workPlaneFramePointToWorld(c.controlPoints[i],
        c.dimension == 3 ? c.normalCoordinates[i] : 0.0, f);
}
QVector<Point3D> vertices(const Shape &s, Point3D placement)
{
    QVector<Point3D> out;
    if (s.geometryType == GeometryType::NurbsSurface) out=s.nurbsSurface.controlPoints;
    else if (s.geometryType == GeometryType::NurbsSolid) out=nurbsSolidControlPoints(s.nurbsSolid);
    else if (s.geometryType == GeometryType::Point) {
        for (auto p:s.points) out.append(shapePointToWorld(s,p));
    } else {
        for (const auto &component:nurbsCurveComponentsForShape(s)) {
            if (!validateNurbsCurve(component.curve)) return {};
            for (int i=0;i<component.curve.controlPoints.size();++i)
                out.append(curvePoint(component.curve,i,component.workPlaneFrame));
        }
    }
    for (auto &p:out) p=plus(p,placement);
    return out;
}
bool fractionOnEdge(Point3D p, Point3D a, Point3D b, qreal tolerance, qreal *t)
{
    const auto d=minus(b,a);
    const qreal length2=dot(d,d);
    if (length2<=tolerance*tolerance) return false;
    *t=dot(minus(p,a),d)/length2;
    return *t>tolerance/std::sqrt(length2) &&
           *t<1.0-tolerance/std::sqrt(length2) &&
           distance2(p,plus(a,times(d,*t)))<=tolerance*tolerance;
}
qreal weight(const QVector<double> &weights, bool rational, int i)
{ return rational ? weights[i] : 1.0; }

// In degree one, inserting an interior knot adds precisely one homogeneous
// linear interpolation of its neighboring CVs. Existing CVs do not move.
void insertCurve(NurbsCurve3D *c, int span, qreal fraction)
{
    const qreal a=weight(c->weights,c->rational,span);
    const qreal b=weight(c->weights,c->rational,span+1);
    // Geometric fraction -> rational parameter fraction.
    const qreal t=fraction*a/(b*(1.0-fraction)+fraction*a);
    const qreal w=(1.0-t)*a+t*b;
    c->controlPoints.insert(span+1,
        (c->controlPoints[span]*((1.0-t)*a)+c->controlPoints[span+1]*(t*b))/w);
    if (c->dimension==3)
        c->normalCoordinates.insert(span+1,
            ((1.0-t)*a*c->normalCoordinates[span]+t*b*c->normalCoordinates[span+1])/w);
    if (!c->weights.isEmpty()) c->weights.insert(span+1,w);
    c->knots.insert(span+1,c->knots[span]*(1.0-t)+c->knots[span+1]*t);
}

void insertSurface(NurbsSurface3D *s, QVector<quint64> *groups,
                   bool alongU, int span, qreal fraction, int boundaryIndex)
{
    const int oldU=s->controlVertexCountU, oldV=s->controlVertexCountV;
    const int ia=alongU ? span*oldV+boundaryIndex : boundaryIndex*oldV+span;
    const int ib=alongU ? (span+1)*oldV+boundaryIndex : boundaryIndex*oldV+span+1;
    const qreal a=weight(s->weights,s->rational,ia), b=weight(s->weights,s->rational,ib);
    const qreal t=fraction*a/(b*(1.0-fraction)+fraction*a);
    const int newU=oldU+(alongU?1:0), newV=oldV+(alongU?0:1);
    QVector<Point3D> points;
    QVector<double> weights;
    QVector<quint64> remapped;
    for (int u=0;u<newU;++u) for (int v=0;v<newV;++v) {
        const int coordinate=alongU?u:v;
        if (coordinate==span+1) {
            const int first=alongU ? span*oldV+v : u*oldV+span;
            const int second=alongU ? (span+1)*oldV+v : u*oldV+span+1;
            const qreal wa=weight(s->weights,s->rational,first);
            const qreal wb=weight(s->weights,s->rational,second);
            const qreal w=wa*(1.0-t)+wb*t;
            points.append(times(plus(times(s->controlPoints[first],wa*(1.0-t)),
                                    times(s->controlPoints[second],wb*t)),1.0/w));
            weights.append(w);
            remapped.append(0);
        } else {
            const int ou=alongU && u>span+1 ? u-1:u;
            const int ov=!alongU && v>span+1 ? v-1:v;
            const int old=ou*oldV+ov;
            points.append(s->controlPoints[old]);
            weights.append(weight(s->weights,s->rational,old));
            remapped.append(groups->isEmpty()?0:groups->at(old));
        }
    }
    auto &knots=alongU?s->knotsU:s->knotsV;
    knots.insert(span+1,knots[span]*(1.0-t)+knots[span+1]*t);
    s->controlVertexCountU=newU;
    s->controlVertexCountV=newV;
    s->controlPoints=points;
    if (!s->weights.isEmpty()) s->weights=weights;
    *groups=remapped;
}

void splitAt(Shape *s, Point3D placement, Point3D p, qreal tolerance)
{
    if (s->geometryType==GeometryType::NurbsSolid) {
        // Keep the original seed and weld index order unless a supported
        // boundary edge actually needs splitting. Each resulting patch stays
        // an exact 2x2 degree-one face, as required by solid shell editing.
        auto candidate=s->nurbsSolid;
        if(!materializeNurbsSolidBoundary(&candidate)) return;
        for(const auto &face:candidate.boundaryFaces) {
            if(face.controlPoints.size()!=4) return;
            const auto a=minus(face.controlPoints[2],face.controlPoints[0]);
            const auto b=minus(face.controlPoints[1],face.controlPoints[0]);
            const qreal ab=dot(a,b);
            if(ab*ab>1e-16*dot(a,a)*dot(b,b) ||
               distance2(face.controlPoints[3],plus(face.controlPoints[0],plus(a,b)))>
                   1e-14*std::max(dot(a,a),dot(b,b))) return;
        }
        const auto originalVertices=nurbsSolidControlPoints(s->nurbsSolid);
        QVector<NurbsSurface3D> faces;
        QVector<bool> reversed;
        bool changed=false;
        for(int faceIndex=0;faceIndex<candidate.boundaryFaces.size();++faceIndex) {
            auto face=candidate.boundaryFaces[faceIndex];
            bool split=false;
            for(int direction=0;direction<2 && !split;++direction) {
                const bool alongU=direction==0;
                for(int side=0;side<2 && !split;++side) {
                    const int first=alongU?side:side*2;
                    const int second=alongU?side+2:side*2+1;
                    qreal fraction;
                    if(!fractionOnEdge(p,plus(face.controlPoints[first],placement),
                                       plus(face.controlPoints[second],placement),tolerance,&fraction)) continue;
                    QVector<quint64> unusedGroups;
                    insertSurface(&face,&unusedGroups,alongU,0,fraction,side);
                    for(int part=0;part<2;++part) {
                        NurbsSurface3D panel=face;
                        panel.controlVertexCountU=2;
                        panel.controlVertexCountV=2;
                        panel.controlPoints.clear();
                        panel.weights.clear();
                        for(int u=0;u<2;++u) for(int v=0;v<2;++v) {
                            const int sourceU=u+(alongU?part:0);
                            const int sourceV=v+(alongU?0:part);
                            const int index=sourceU*face.controlVertexCountV+sourceV;
                            panel.controlPoints.append(face.controlPoints[index]);
                            if(!face.weights.isEmpty()) panel.weights.append(face.weights[index]);
                        }
                        if(alongU) panel.knotsU={face.knotsU[part],face.knotsU[part+1]};
                        else panel.knotsV={face.knotsV[part],face.knotsV[part+1]};
                        faces.append(panel);
                        reversed.append(candidate.boundaryFaceReversed[faceIndex]);
                    }
                    split=true;
                    changed=true;
                }
            }
            if(!split) {
                faces.append(face);
                reversed.append(candidate.boundaryFaceReversed[faceIndex]);
            }
        }
        if(!changed) return;
        candidate.boundaryFaces=faces;
        candidate.boundaryFaceReversed=reversed;
        if(!validateNurbsSolid(candidate)) return;
        QVector<quint64> groups;
        if(!s->controlPointWeldGroups.isEmpty()) {
            for(const auto &point:nurbsSolidControlPoints(candidate)) {
                quint64 group=0;
                for(int i=0;i<originalVertices.size();++i)
                    if(distance2(point,originalVertices[i])<=1e-16) {
                        group=s->controlPointWeldGroups[i];
                        break;
                    }
                groups.append(group);
            }
        }
        s->nurbsSolid=std::move(candidate);
        s->controlPointWeldGroups=groups;
        return;
    }
    if (s->geometryType==GeometryType::NurbsSurface) {
        auto &surface=s->nurbsSurface;
        if (!validateNurbsSurface(surface) || surface.degreeU!=1 || surface.degreeV!=1) return;
        // Each boundary can gain at most one knot for this candidate point.
        for (int direction=0;direction<2;++direction) {
            const bool alongU=direction==0;
            for (int side=0;side<2;++side) {
                const int count=alongU?surface.controlVertexCountU:surface.controlVertexCountV;
                const int boundary=side==0?0:(alongU?surface.controlVertexCountV:surface.controlVertexCountU)-1;
                for (int span=0;span<count-1;++span) {
                    const int ia=alongU?span*surface.controlVertexCountV+boundary:boundary*surface.controlVertexCountV+span;
                    const int ib=alongU?(span+1)*surface.controlVertexCountV+boundary:ia+1;
                    qreal t;
                    if (fractionOnEdge(p,plus(surface.controlPoints[ia],placement),
                                       plus(surface.controlPoints[ib],placement),tolerance,&t)) {
                        insertSurface(&surface,&s->controlPointWeldGroups,alongU,span,t,boundary);
                        break;
                    }
                }
            }
        }
        return;
    }
    const bool poly=s->geometryType==GeometryType::PolyCurve;
    int offset=0;
    const int componentCount=poly?s->components.size():1;
    for (int component=0;component<componentCount;++component) {
        auto &curve=poly?s->components[component]:s->nurbs;
        const auto frame=poly?shapeComponentWorkPlaneFrame(*s,component):shapeWorkPlaneFrame(*s);
        if (validateNurbsCurve(curve) && curve.degree==1) {
            for (int i=0;i<curve.controlPoints.size()-1;++i) {
                qreal t;
                if (fractionOnEdge(p,plus(curvePoint(curve,i,frame),placement),
                                   plus(curvePoint(curve,i+1,frame),placement),tolerance,&t)) {
                    insertCurve(&curve,i,t);
                    if(s->geometryType==GeometryType::Rectangle || s->geometryType==GeometryType::Polygon)
                        s->geometryType=GeometryType::Nurbs;
                    if (!s->controlPointWeldGroups.isEmpty()) s->controlPointWeldGroups.insert(offset+i+1,0);
                    ++i;
                }
            }
        }
        offset+=curve.controlPoints.size();
    }
}
} // namespace

QVector<Point3D> extrusionWeldControlPoints(const Shape &shape, Point3D placement)
{
    return vertices(shape,placement);
}

bool weldExtrusionDestination(QVector<Shape> *shapes,
                              const QVector<int> &extrudedShapeIndices,
                              const QVector<Point3D> &placements,
                              qreal tolerance, QString *failure)
{
    const auto fail=[&](const QString &message) { if(failure) *failure=message; return false; };
    if (!shapes || !std::isfinite(tolerance) || tolerance<=0 ||
        (!placements.isEmpty() && placements.size()!=shapes->size()))
        return fail(QStringLiteral("Invalid destination weld inputs"));
    QSet<int> focused;
    for (int i:extrudedShapeIndices) {
        if(i<0 || i>=shapes->size()) return fail(QStringLiteral("Invalid extrusion index"));
        focused.insert(i);
    }
    QVector<Shape> staged=*shapes;
    const auto placement=[&](int i) { return placements.isEmpty()?Point3D{}:placements[i]; };
    QVector<QVector<Point3D>> originalPoints;
    quint64 next=1;
    for (int i=0;i<staged.size();++i) {
        originalPoints.append(vertices(staged[i],placement(i)));
        if (!staged[i].controlPointWeldGroups.isEmpty() && !originalPoints.last().isEmpty() &&
            staged[i].controlPointWeldGroups.size()!=originalPoints.last().size())
            return fail(QStringLiteral("Invalid control point weld groups"));
        for (quint64 group:staged[i].controlPointWeldGroups) {
            if(group==std::numeric_limits<quint64>::max()) return fail(QStringLiteral("Weld group IDs exhausted"));
            next=std::max(next,group+1);
        }
    }
    for (int target=0;target<staged.size();++target)
        for (int source=0;source<staged.size();++source) {
            if(target==source || (!focused.contains(target)&&!focused.contains(source))) continue;
            for (const auto &p:originalPoints[source]) splitAt(&staged[target],placement(target),p,tolerance);
        }
    QVector<QVector<Point3D>> points;
    for (int i=0;i<staged.size();++i) points.append(vertices(staged[i],placement(i)));
    QHash<quint64,quint64> aliases;
    const auto root=[&](quint64 g) { while(aliases.contains(g)) g=aliases.value(g); return g; };
    for (int a=0;a<staged.size();++a) for (int b=a+1;b<staged.size();++b) {
        if(!focused.contains(a)&&!focused.contains(b)) continue;
        for(int ai=0;ai<points[a].size();++ai) for(int bi=0;bi<points[b].size();++bi) {
            if(distance2(points[a][ai],points[b][bi])>tolerance*tolerance) continue;
            auto &ga=staged[a].controlPointWeldGroups;
            auto &gb=staged[b].controlPointWeldGroups;
            if(ga.isEmpty()) ga.fill(0,points[a].size());
            if(gb.isEmpty()) gb.fill(0,points[b].size());
            quint64 first=root(ga[ai]), second=root(gb[bi]);
            if(!first&&!second) {
                if(next==std::numeric_limits<quint64>::max()) return fail(QStringLiteral("Weld group IDs exhausted"));
                first=next++;
            } else if(!first) first=second;
            if(second&&second!=first) aliases.insert(second,first);
            ga[ai]=first;
            gb[bi]=first;
        }
    }
    for(auto &shape:staged) for(auto &g:shape.controlPointWeldGroups) g=root(g);
    *shapes=std::move(staged);
    if(failure) failure->clear();
    return true;
}

bool mergeAdjacentExtrusionSolids(QVector<Shape> *shapes,
                                  const QVector<int> &eligibleIndices,
                                  QVector<int> *owners,
                                  QVector<int> *capIndices,
                                  QString *failure)
{
    const auto fail=[&](const QString &message) { if(failure) *failure=message; return false; };
    if(!shapes || !owners || !capIndices) return fail(QStringLiteral("Invalid connected extrusion inputs"));
    QVector<Shape> staged=*shapes;
    QVector<int> parent(staged.size()), caps(staged.size(),1);
    for(int i=0;i<parent.size();++i) parent[i]=i;
    const auto root=[&](int i) { while(parent[i]!=i) i=parent[i]; return i; };
    QVector<int> eligible=eligibleIndices;
    std::sort(eligible.begin(),eligible.end());
    eligible.erase(std::unique(eligible.begin(),eligible.end()),eligible.end());
    QHash<int,QVector<int>> removed;
    QVector<int> supported;
    for(int i:eligible) {
        if(i<0 || i>=staged.size() || staged[i].geometryType!=GeometryType::NurbsSolid)
            return fail(QStringLiteral("Invalid standalone extrusion solid"));
        if(materializeNurbsSolidBoundary(&staged[i].nurbsSolid)) supported.append(i);
        else staged[i]=shapes->at(i);
    }
    eligible=supported;
    // A shared boundary may cover only part of one source edge. Partition
    // ruled walls at the other regions' exact boundary vertices before face
    // matching. Cap slots stay fixed so each source cap remains selectable.
    QHash<int,QVector<Point3D>> wallVertices;
    for(int i:eligible) {
        const auto &faces=staged[i].nurbsSolid.boundaryFaces;
        for(int face=2;face<faces.size();++face)
            wallVertices[i]+=faces[face].controlPoints;
    }
    for(int target:eligible) {
        auto &solid=staged[target].nurbsSolid;
        for(int source:eligible) {
            if(source==target) continue;
            for(const auto &point:wallVertices[source]) {
                QVector<NurbsSurface3D> partitioned;
                QVector<bool> orientations;
                for(int faceIndex=0;faceIndex<solid.boundaryFaces.size();++faceIndex) {
                    auto face=solid.boundaryFaces[faceIndex];
                    bool split=false;
                    if(faceIndex>=2) for(int direction=0;direction<2 && !split;++direction) {
                        const bool alongU=direction==0;
                        for(int side=0;side<2 && !split;++side) {
                            const int first=alongU?side:side*2;
                            const int second=alongU?side+2:side*2+1;
                            qreal fraction;
                            if(!fractionOnEdge(point,face.controlPoints[first],face.controlPoints[second],1e-7,&fraction)) continue;
                            QVector<quint64> unused;
                            insertSurface(&face,&unused,alongU,0,fraction,side);
                            for(int part=0;part<2;++part) {
                                auto panel=face;
                                panel.controlVertexCountU=2;
                                panel.controlVertexCountV=2;
                                panel.controlPoints.clear();
                                panel.weights.clear();
                                for(int u=0;u<2;++u) for(int v=0;v<2;++v) {
                                    const int sourceU=u+(alongU?part:0);
                                    const int sourceV=v+(alongU?0:part);
                                    const int cv=sourceU*face.controlVertexCountV+sourceV;
                                    panel.controlPoints.append(face.controlPoints[cv]);
                                    if(!face.weights.isEmpty()) panel.weights.append(face.weights[cv]);
                                }
                                if(alongU) panel.knotsU={face.knotsU[part],face.knotsU[part+1]};
                                else panel.knotsV={face.knotsV[part],face.knotsV[part+1]};
                                partitioned.append(panel);
                                orientations.append(solid.boundaryFaceReversed[faceIndex]);
                            }
                            split=true;
                        }
                    }
                    if(!split) {
                        partitioned.append(face);
                        orientations.append(solid.boundaryFaceReversed[faceIndex]);
                    }
                }
                solid.boundaryFaces=std::move(partitioned);
                solid.boundaryFaceReversed=std::move(orientations);
            }
        }
        if(!validateNurbsSolid(solid))
            return fail(QStringLiteral("Partial shared boundary subdivision did not preserve the solid shell"));
    }
    const auto normal=[](const NurbsSurface3D &face,bool reversed) {
        auto a=minus(face.controlPoints[2],face.controlPoints[0]);
        auto b=minus(face.controlPoints[1],face.controlPoints[0]);
        Point3D n{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
        return reversed?times(n,-1):n;
    };
    for(int ai=0;ai<eligible.size();++ai) for(int bi=ai+1;bi<eligible.size();++bi) {
        const int a=eligible[ai], b=eligible[bi];
        const auto &sa=staged[a].nurbsSolid;
        const auto &sb=staged[b].nurbsSolid;
        for(int fa=2;fa<sa.boundaryFaces.size();++fa) for(int fb=2;fb<sb.boundaryFaces.size();++fb) {
            const auto &first=sa.boundaryFaces[fa];
            const auto &second=sb.boundaryFaces[fb];
            if(first.controlPoints.size()!=4 || second.controlPoints.size()!=4) continue;
            QSet<int> matches;
            for(const auto &p:first.controlPoints) for(int k=0;k<4;++k)
                if(distance2(p,second.controlPoints[k])<1e-14) matches.insert(k);
            if(matches.size()!=4) continue;
            const auto na=normal(first,sa.boundaryFaceReversed[fa]);
            const auto nb=normal(second,sb.boundaryFaceReversed[fb]);
            if(dot(na,nb)>=0) continue;
            if(removed[a].contains(fa)||removed[b].contains(fb))
                return fail(QStringLiteral("More than two extrusions share one boundary wall"));
            removed[a].append(fa);
            removed[b].append(fb);
            const int ra=root(a), rb=root(b);
            parent[std::max(ra,rb)]=std::min(ra,rb);
        }
    }
    for(int owner:eligible) {
        if(root(owner)!=owner) continue;
        QVector<int> members;
        for(int i:eligible) if(root(i)==owner) members.append(i);
        if(members.size()<2) {
            staged[owner]=shapes->at(owner);
            continue;
        }
        auto combined=staged[owner].nurbsSolid;
        combined.boundaryFaces.clear();
        combined.boundaryFaceReversed.clear();
        for(int i:members) {
            const auto &solid=staged[i].nurbsSolid;
            for(int face=0;face<solid.boundaryFaces.size();++face) {
                if(removed[i].contains(face)) continue;
                if(face==1) caps[i]=combined.boundaryFaces.size();
                combined.boundaryFaces.append(solid.boundaryFaces[face]);
                combined.boundaryFaceReversed.append(solid.boundaryFaceReversed[face]);
            }
        }
        if(!validateNurbsSolid(combined))
            return fail(QStringLiteral("Adjacent extrusions do not form a closed connected shell"));
        for(int i:members) {
            staged[i].nurbsSolid=combined;
            staged[i].controlPointWeldGroups.clear();
        }
    }
    for(int &i:parent) i=root(i);
    *owners=parent;
    *capIndices=caps;
    *shapes=std::move(staged);
    if(failure) failure->clear();
    return true;
}
} // namespace classiCAD
