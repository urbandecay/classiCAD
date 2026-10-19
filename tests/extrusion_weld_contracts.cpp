#include "core/geometry/extrusion_weld.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_surface_factory.h"

#include <QDebug>
#include <cmath>

using namespace classiCAD;
namespace {
Shape line(QPointF a,QPointF b)
{
    Shape s;
    s.geometryType=GeometryType::Line;
    s.nurbs=makeDegreeOneNurbs({a,b});
    return s;
}
bool check(bool value,const char *message)
{ if(!value) qCritical()<<message; return value; }
bool near(Point3D a,Point3D b)
{ return std::hypot(a.x-b.x,std::hypot(a.y-b.y,a.z-b.z))<1e-9; }
}
int main()
{
    bool ok=true;
    // Grab must not attach an untouched endpoint just because its object moved.
    QVector<Shape> grabbed{line({0,0},{5,0}),line({0,0},{0,3}),line({5,0},{5,3})};
    QHash<int,QVector<Point3D>> eligible{{0,{{5,0,0}}}};
    ok &= check(weldExtrusionDestination(&grabbed,{0},{},1e-6,nullptr,&eligible),
                "Grab destination weld accepts selected vertex mask");
    ok &= check(grabbed[1].controlPointWeldGroups.isEmpty() &&
                grabbed[0].controlPointWeldGroups[0]==0 &&
                grabbed[0].controlPointWeldGroups[1]!=0 &&
                grabbed[0].controlPointWeldGroups[1]==grabbed[2].controlPointWeldGroups[0],
                "Grab weld excludes untouched vertices on moved object");
    grabbed={line({0,0},{10,0}),line({3,0},{3,3})};
    eligible={{0,{{0,0,0},{10,0,0}}}};
    ok &= check(weldExtrusionDestination(&grabbed,{0},{},1e-6,nullptr,&eligible) &&
                grabbed[0].nurbs.controlPoints.size()==3 &&
                grabbed[0].controlPointWeldGroups[1]==grabbed[1].controlPointWeldGroups[0],
                "Grab selected edge splits at stationary target vertex");
    grabbed={line({0,0},{10,0}),line({3,0},{3,3})};
    eligible={{0,{{0,0,0}}}};
    ok &= check(weldExtrusionDestination(&grabbed,{0},{},1e-6,nullptr,&eligible) &&
                grabbed[0].nurbs.controlPoints.size()==2 &&
                grabbed[1].controlPointWeldGroups.isEmpty(),
                "Grab unselected adjoining edge does not weld its interior");
    // A translated incoming line overlaps only the middle of the target.
    QVector<Shape> curves{line({0,0},{10,0}),line({0,0},{4,0})};
    curves[0].controlPointWeldGroups={31,32};
    ok &= check(weldExtrusionDestination(&curves,{1},{{0,0,0},{3,0,0}}),"curve overlap weld");
    ok &= check(curves[0].nurbs.controlPoints.size()==4,"both overlap limits split");
    ok &= check(curves[0].controlPointWeldGroups[0]==31 && curves[0].controlPointWeldGroups[3]==32,"existing groups retained");
    ok &= check(curves[0].controlPointWeldGroups[1]==curves[1].controlPointWeldGroups[0] &&
                curves[0].controlPointWeldGroups[2]==curves[1].controlPointWeldGroups[1],"overlap endpoint topology shared");
    ok &= check(curves[0].nurbs.knots.first()==0 && curves[0].nurbs.knots.last()==1,"curve domain retained");
    ok &= check(curves[1].nurbs.controlPoints.first()==QPointF(0,0),"placement not baked");
    Shape rational=line({0,0},{10,0});
    rational.nurbs.rational=true;
    rational.nurbs.weights={2,5};
    rational.nurbs.knots={4,9};
    QVector<Shape> weighted{rational,line({3,0},{7,0})};
    ok &= check(weldExtrusionDestination(&weighted,{1}) && validateNurbsCurve(weighted[0].nurbs),
                "rational degree-one knot insertion valid");
    for(double parameter:{4.0,4.5,6.0,8.5,9.0}) {
        QPointF before,after;
        evaluateNurbsPoint(rational.nurbs,parameter,&before);
        evaluateNurbsPoint(weighted[0].nurbs,parameter,&after);
        ok &= check(std::hypot(before.x()-after.x(),before.y()-after.y())<1e-9,
                    "rational curve positions and original domain preserved");
    }

    Shape surface;
    surface.geometryType=GeometryType::NurbsSurface;
    makeNurbsExtrusionSurface(makeDegreeOneNurbs({{0,0},{10,0}}),
        makeWorkPlaneFrame(WorkPlane::XY),{0,10,0},&surface.nurbsSurface);
    QVector<Shape> patches{surface,line({3,0},{7,0})};
    const auto original=surface.nurbsSurface;
    ok &= check(weldExtrusionDestination(&patches,{1}),"surface boundary overlap");
    const auto &split=patches[0].nurbsSurface;
    ok &= check(split.controlVertexCountU==4 && split.controlVertexCountV==2,"surface full rows inserted");
    ok &= check(validateNurbsSurface(split),"split surface valid");
    ok &= check(patches[0].controlPointWeldGroups[2]==patches[1].controlPointWeldGroups[0] &&
                patches[0].controlPointWeldGroups[4]==patches[1].controlPointWeldGroups[1],"surface new boundary nodes welded");
    for(double u:{0.0,0.1,0.3,0.5,0.7,0.9,1.0}) for(double v:{0.0,0.4,1.0}) {
        Point3D before,after;
        evaluateNurbsSurfacePoint(original,u,v,&before);
        evaluateNurbsSurfacePoint(split,u,v,&after);
        ok &= check(near(before,after),"surface exact geometry preserved");
    }
    // Existing weld groups on other objects must follow union operations.
    QVector<Shape> groups{line({0,0},{1,0}),line({1,0},{2,0}),line({2,0},{3,0})};
    groups[0].controlPointWeldGroups={0,101};
    groups[1].controlPointWeldGroups={102,102};
    groups[2].controlPointWeldGroups={102,0};
    ok &= check(weldExtrusionDestination(&groups,{1}),"group union");
    ok &= check(groups[0].controlPointWeldGroups[1]==groups[2].controlPointWeldGroups[0],"group union propagates beyond selected pair");
    // Projected intersections at different depths must never weld.
    QVector<Shape> spatial{line({0,0},{10,0}),line({3,0},{7,0})};
    ok &= check(weldExtrusionDestination(&spatial,{1},{{0,0,0},{0,0,1}}),"spatial disjoint operation");
    ok &= check(spatial[0].nurbs.controlPoints.size()==2 && spatial[0].controlPointWeldGroups.isEmpty(),"no projection weld");
    const auto box=[](double x) {
        Shape shape;
        shape.geometryType=GeometryType::NurbsSolid;
        NurbsSurface3D base;
        makeNurbsExtrusionSurface(makeDegreeOneNurbs({{x,0},{x+10,0}}),
            makeWorkPlaneFrame(WorkPlane::XY),{0,10,0},&base);
        makeNurbsExtrusionSolid(base,{0,0,5},&shape.nurbsSolid);
        return shape;
    };
    QVector<Shape> solids{box(0),box(10),box(40)};
    Shape branch=line({3,4},{4,3});
    branch.workPlaneOffset=5;
    QVector<Shape> solidTarget{box(0),branch};
    ok &= check(weldExtrusionDestination(&solidTarget,{1},{{3,4,0},{0,0,0}}),
                "incoming edge can weld to a translated solid vertex");
    const auto cage=nurbsSolidControlPoints(solidTarget[0].nurbsSolid);
    ok &= check(cage.size()==8 && near(cage[4],{0,0,5}),"solid cage follows viewport dedup order");
    ok &= check(solidTarget[0].controlPointWeldGroups.size()==8 &&
                solidTarget[0].controlPointWeldGroups[4]!=0 &&
                solidTarget[0].controlPointWeldGroups[4]==solidTarget[1].controlPointWeldGroups[0],
                "incoming endpoint shares solid unique cage group");
    ok &= check(solidTarget[0].nurbsSolid.boundaryFaces.isEmpty(),
                "vertex weld does not materialize or alter exact solid geometry");
    Shape tip=line({3,0},{7,0});
    tip.workPlaneOffset=5;
    QVector<Shape> solidOverlap{box(0),tip};
    solidOverlap[0].controlPointWeldGroups.fill(0,8);
    solidOverlap[0].controlPointWeldGroups[0]=901;
    ok &= check(weldExtrusionDestination(&solidOverlap,{1}),"partial overlap splits solid top edge");
    const auto splitCage=nurbsSolidControlPoints(solidOverlap[0].nurbsSolid);
    ok &= check(validateNurbsSolid(solidOverlap[0].nurbsSolid) &&
                solidOverlap[0].nurbsSolid.boundaryFaces.size()>6,
                "solid edge split retains a valid exact shell");
    for(int end=0;end<2;++end) {
        const Point3D expected{end==0?3.0:7.0,0,5};
        int match=-1;
        for(int i=0;i<splitCage.size();++i) if(near(splitCage[i],expected)) match=i;
        ok &= check(match>=0 && solidOverlap[0].controlPointWeldGroups[match]!=0 &&
                    solidOverlap[0].controlPointWeldGroups[match]==solidOverlap[1].controlPointWeldGroups[end],
                    "overlap limits share inserted solid cage vertices");
    }
    ok &= check(solidOverlap[0].controlPointWeldGroups[0]==901,"solid split preserves existing vertex groups");
    for(const auto &panel:solidOverlap[0].nurbsSolid.boundaryFaces)
        ok &= check(panel.controlVertexCountU==2 && panel.controlVertexCountV==2 &&
                    validateNurbsSurface(panel),"solid split emits exact 2x2 panels");
    QVector<int> owners,caps;
    QString failure;
    ok &= check(mergeAdjacentExtrusionSolids(&solids,{0,1,2},&owners,&caps,&failure),qPrintable(failure));
    ok &= check(owners==QVector<int>({0,0,2}),"only adjacent solids share owner");
    ok &= check(solids[0].nurbsSolid.boundaryFaces.size()==10 &&
                solids[1].nurbsSolid.boundaryFaces.size()==10,"shared interior walls removed");
    ok &= check(caps[0]==1 && caps[1]==6 && caps[2]==1,"original caps mapped in merged shell");
    ok &= check(validateNurbsSolid(solids[0].nurbsSolid),"merged shell closed");
    // A three-face chain must merge transitively into a single region.
    solids={box(0),box(10),box(20)};
    ok &= check(mergeAdjacentExtrusionSolids(&solids,{2,0,1},&owners,&caps),"three adjacent sweeps");
    ok &= check(owners==QVector<int>({0,0,0}) &&
                solids[0].nurbsSolid.boundaryFaces.size()==14,"transitive region union");
    Shape half=box(10);
    for(auto &cv:half.nurbsSolid.baseSurface.controlPoints) cv.y*=0.5;
    solids={box(0),half};
    ok &= check(mergeAdjacentExtrusionSolids(&solids,{0,1},&owners,&caps),
                "adjacent unequal rectangles merge along partial shared edge");
    ok &= check(owners==QVector<int>({0,0}) &&
                solids[0].nurbsSolid.boundaryFaces.size()==11 &&
                validateNurbsSolid(solids[0].nurbsSolid),
                "partial shared wall is removed from a closed connected shell");
    bool hasInteriorWall=false;
    for(const auto &panel:solids[0].nurbsSolid.boundaryFaces) {
        bool interior=true;
        for(const auto &cv:panel.controlPoints)
            interior=interior && std::abs(cv.x-10)<1e-8 && cv.y<=5+1e-8;
        hasInteriorWall=hasInteriorWall || interior;
    }
    ok &= check(!hasInteriorWall && caps[0]==1 && caps[1]>=2,
                "partial region has no retained interior wall and both caps remain mapped");
    Shape curved;
    curved.geometryType=GeometryType::NurbsSolid;
    NurbsSurface3D disk;
    makeNurbsPlanarFillSurface(makeCircleNurbs({{0,0},{5,0}}),
        makeWorkPlaneFrame(WorkPlane::XY),&disk);
    ok &= check(makeNurbsExtrusionSolid(disk,{0,0,5},&curved.nurbsSolid),"curved singleton seed valid");
    solids={curved};
    ok &= check(mergeAdjacentExtrusionSolids(&solids,{0},&owners,&caps) &&
                solids[0].nurbsSolid.boundaryFaces.isEmpty() &&
                solids[0].nurbsSolid.baseSurface.trimLoops.size()==1,
                "curved singleton remains unchanged");
    return ok?0:1;
}
