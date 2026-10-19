#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/history/history.h"
#include "core/serialization/shape_json_codec.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tools/point_extrude_tool.h"
#include "tools/tool_context.h"

#include <QApplication>
#include <QDebug>
#include <QSet>
#include <cmath>

using namespace classiCAD;
namespace {
bool check(bool value,const char *message)
{ if(!value) qCritical()<<message; return value; }
Shape line(QPointF a,QPointF b)
{
    Shape s;
    s.geometryType=GeometryType::Line;
    s.nurbs=makeDegreeOneNurbs({a,b});
    return s;
}
struct Fixture {
    Document document;
    SelectionModel selection;
    History history{document};
    ViewportTransform camera;
    CurveSampler sampler;
    CurveHitTester hits;
    SnapEngine snaps;
    ToolContext context{document,selection,history,camera,sampler,hits,snaps};
    Fixture() {
        camera.setViewPreset(ViewportViewPreset::Top);
        camera.zoom()=20;
        SnapSettings settings;
        settings.enabled=false;
        snaps.setSettings(settings);
    }
    void move(PointExtrudeTool &tool,Point3D target) {
        ToolInput input;
        input.viewportSize={800,600};
        input.workPlaneFrame=makeWorkPlaneFrame(WorkPlane::XY);
        camera.worldPointToScreenUnclipped(target,input.viewportSize,&input.screenPosition);
        tool.handleMouseMove(input,context);
    }
};
}
int main(int argc,char **argv)
{
    QApplication app(argc,argv);
    bool ok=true;
    {
        Fixture f;
        const auto id=f.document.append(line({0,0},{10,0}));
        f.selection.setObjectIds({id});
        PointExtrudeTool tool;
        tool.begin(f.context);
        f.move(tool,{0,5,0});
        ok &= check(!tool.weldEnabled() && tool.status().canCommit,"loose edge can extrude with auto merge off");
        tool.commit(f.context);
        ok &= check(f.document.size()==1 && f.document.shape(id)->geometryType==GeometryType::NurbsSurface,
                    "loose edge is absorbed into its connected surface");
        ok &= check(f.history.undoCount()==1 && f.history.undo() &&
                    f.document.shape(id)->geometryType==GeometryType::Line &&
                    f.history.redo() && f.document.shape(id)->geometryType==GeometryType::NurbsSurface,
                    "edge-to-surface conversion undoes atomically");
    }
    {
        Fixture f;
        const auto id=f.document.append(line({0,0},{10,0}));
        PointExtrudeTool tool;
        tool.setControlPointSources({{id,{10,0,0},makeWorkPlaneFrame(WorkPlane::XY),1}});
        tool.begin(f.context);
        f.move(tool,{10,5,0});
        tool.commit(f.context);
        const Shape *source=f.document.shape(id);
        ok &= check(f.document.size()==2 && !source->controlPointWeldGroups.isEmpty(),
                    "vertex branch attaches to source even with auto merge off");
        if(f.document.size()==2 && source->controlPointWeldGroups.size()==2) {
            const Shape &branch=f.document[1];
            ok &= check(!branch.controlPointWeldGroups.isEmpty() &&
                        branch.controlPointWeldGroups[0]==source->controlPointWeldGroups[1] &&
                        branch.controlPointWeldGroups[0]!=0,"branch source shares persistent CV topology");
        }
        ok &= check(f.history.undoCount()==1 && f.history.undo() && f.document.size()==1 &&
                    f.document.shape(id)->controlPointWeldGroups.isEmpty(),"branch and source weld undo together");
    }
    for(const int corner:{0,4}) {
        Fixture f;
        Shape solid;
        solid.geometryType=GeometryType::NurbsSolid;
        NurbsSurface3D base;
        makeNurbsExtrusionSurface(makeDegreeOneNurbs({{0,0},{10,0}}),
            makeWorkPlaneFrame(WorkPlane::XY),{0,10,0},&base);
        makeNurbsExtrusionSolid(base,{0,0,5},&solid.nurbsSolid);
        const auto id=f.document.append(solid);
        PointExtrudeTool tool;
        const qreal height=corner==0?0.0:5.0;
        tool.setControlPointSources({{id,{0,0,height},makeWorkPlaneFrame(WorkPlane::XY,height),corner}});
        tool.begin(f.context);
        f.move(tool,{0,5,height});
        tool.commit(f.context);
        const Shape *source=f.document.shape(id);
        ok &= check(f.document.size()==2 && source->controlPointWeldGroups.size()==8 &&
                    source->controlPointWeldGroups[corner]!=0,
                    "solid vertex branch stays attached with auto merge off");
        if(f.document.size()==2 && source->controlPointWeldGroups.size()==8)
            ok &= check(!f.document[1].controlPointWeldGroups.isEmpty() &&
                        f.document[1].controlPointWeldGroups[0]==source->controlPointWeldGroups[corner],
                        "solid cage and branch share one persistent weld group");
        Shape restored;
        ok &= check(shapeFromJson(shapeToJson(*source),&restored) &&
                    restored.controlPointWeldGroups==source->controlPointWeldGroups,
                    "solid weld groups survive native JSON round trip");
        ok &= check(f.history.undoCount()==1 && f.history.undo() &&
                    f.document.size()==1 && f.document.shape(id)->controlPointWeldGroups.isEmpty(),
                    "solid vertex attachment undoes with branch geometry");
    }
    {
        Fixture f;
        Shape point;
        point.geometryType=GeometryType::Point;
        point.points={{5,0}};
        const auto source=f.document.append(point);
        const auto target=f.document.append(line({0,5},{10,5}));
        f.selection.setObjectIds({source});
        PointExtrudeTool tool;
        tool.begin(f.context);
        tool.toggleWeld(f.context);
        f.move(tool,{5,5,0});
        ok &= check(tool.status().canCommit,"auto merge target is committable");
        tool.commit(f.context);
        const Shape *edge=f.document.shape(target);
        ok &= check(edge && edge->nurbs.controlPoints.size()==3 &&
                    edge->controlPointWeldGroups.size()==3 && edge->controlPointWeldGroups[1]!=0,
                    "auto merge splits destination edge at new tip");
        ok &= check(f.history.undoCount()==1 && f.history.undo() && f.document.size()==2 &&
                    f.document.shape(target)->nurbs.controlPoints.size()==2,
                    "destination split and extrusion undo together");
    }
    {
        Fixture f;
        Shape face;
        face.geometryType=GeometryType::NurbsSurface;
        auto &surface=face.nurbsSurface;
        surface.controlVertexCountU=3;
        surface.controlVertexCountV=2;
        surface.controlPoints={{0,0,0},{0,10,0},{5,0,0},{5,10,0},{10,0,0},{10,10,0}};
        surface.weights={1,1,1,1,1,1};
        surface.knotsU={0,0.5,1};
        surface.knotsV={0,1};
        face.controlPointWeldGroups={0,0,44,0,0,0};
        const auto faceId=f.document.append(face);
        Shape neighbor=line({5,0},{5,-5});
        neighbor.controlPointWeldGroups={44,0};
        const auto neighborId=f.document.append(neighbor);
        const auto originalNeighbor=shapeToJson(neighbor);
        f.selection.setObjectIds({faceId});
        PointExtrudeTool tool;
        tool.begin(f.context);
        f.move(tool,{0,5,0});
        ok &= check(!tool.weldEnabled() && tool.status().canCommit,
                    "affine split surface extrudes with destination weld disabled");
        tool.commit(f.context);
        const Shape *solid=f.document.shape(faceId);
        ok &= check(f.document.size()==2 && solid && solid->geometryType==GeometryType::NurbsSolid &&
                    validateNurbsSolid(solid->nurbsSolid),"split surface becomes a valid connected solid");
        const auto cage=solid?nurbsSolidControlPoints(solid->nurbsSolid):QVector<Point3D>{};
        int sourceVertex=-1;
        for(int i=0;i<cage.size();++i)
            if(std::abs(cage[i].x-5)<1e-8 && std::abs(cage[i].y)<1e-8 && std::abs(cage[i].z)<1e-8)
                sourceVertex=i;
        ok &= check(sourceVertex>=0 && solid->controlPointWeldGroups.value(sourceVertex)==44,
                    "inserted source boundary vertex retains original weld group in solid cage");
        ok &= check(shapeToJson(*f.document.shape(neighborId))==originalNeighbor,
                    "existing neighboring curve remains unchanged with destination weld disabled");
        const auto completed=tool.preview().completedFaceExtrusions;
        const auto faces=solid?nurbsSolidFaces(solid->nurbsSolid):QVector<NurbsSurface3D>{};
        qreal capArea=0;
        for(const auto &selection:completed) {
            ok &= check(selection.objectId==faceId && selection.faceIndex>=0 && selection.faceIndex<faces.size(),
                        "source attachment split retains valid cap selection");
            if(selection.faceIndex<0 || selection.faceIndex>=faces.size()) continue;
            const auto &panel=faces[selection.faceIndex];
            for(const auto &p:panel.controlPoints)
                ok &= check(std::abs(p.z-5)<1e-8,"source weld restoration selects moving cap only");
            const auto &a=panel.controlPoints[0];
            const auto &b=panel.controlPoints[2];
            const auto &c=panel.controlPoints[1];
            capArea+=std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x));
        }
        ok &= check(!completed.isEmpty() && std::abs(capArea-100)<1e-8,
                    "source attachment restoration preserves full moving cap selection");
        ok &= check(f.history.undoCount()==1 && f.history.undo() && f.document.size()==2 &&
                    shapeToJson(*f.document.shape(faceId))==shapeToJson(face) &&
                    shapeToJson(*f.document.shape(neighborId))==originalNeighbor &&
                    f.history.redo() && f.document.shape(faceId)->geometryType==GeometryType::NurbsSolid,
                    "split source, persistent attachment, and selected face extrusion undo atomically");
    }
    {
        Fixture f;
        Shape sourceFace;
        sourceFace.geometryType=GeometryType::NurbsSurface;
        makeNurbsExtrusionSurface(makeDegreeOneNurbs({{0,0},{10,0}}),
            makeWorkPlaneFrame(WorkPlane::XY),{0,10,0},&sourceFace.nurbsSurface);
        const auto sourceId=f.document.append(sourceFace);
        Shape target;
        target.geometryType=GeometryType::Point;
        target.points={{3,0}};
        target.workPlaneOffset=5;
        const auto targetId=f.document.append(target);
        f.selection.setObjectIds({sourceId});
        PointExtrudeTool tool;
        tool.begin(f.context);
        tool.toggleWeld(f.context);
        f.move(tool,{0,5,0});
        ok &= check(tool.status().canCommit,"face cap overlapping existing edge-interior target can commit");
        tool.commit(f.context);
        const Shape *solid=f.document.shape(sourceId);
        const auto completed=tool.preview().completedFaceExtrusions;
        ok &= check(f.document.size()==2 && solid && solid->geometryType==GeometryType::NurbsSolid &&
                    validateNurbsSolid(solid->nurbsSolid),"destination weld leaves a closed exact face extrusion");
        const auto faces=solid?nurbsSolidFaces(solid->nurbsSolid):QVector<NurbsSurface3D>{};
        ok &= check(faces.size()==8 && completed.size()==2,"edge interior split remaps moving cap into both subsets");
        QSet<int> selectedIndices;
        qreal selectedArea=0;
        for(const auto &selection:completed) {
            ok &= check(selection.objectId==sourceId && selection.faceIndex>=0 && selection.faceIndex<faces.size(),
                        "all remapped cap selections address the surviving solid");
            if(selection.faceIndex<0 || selection.faceIndex>=faces.size()) continue;
            selectedIndices.insert(selection.faceIndex);
            const auto &panel=faces[selection.faceIndex];
            for(const auto &point:panel.controlPoints)
                ok &= check(std::abs(point.z-5)<1e-8 && point.x>=-1e-8 && point.x<=10+1e-8 &&
                            point.y>=-1e-8 && point.y<=10+1e-8,
                            "completion selects translated cap subsets only");
            const auto &a=panel.controlPoints[0];
            const auto &b=panel.controlPoints[2];
            const auto &c=panel.controlPoints[1];
            selectedArea+=std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x));
        }
        ok &= check(selectedIndices.size()==2 && std::abs(selectedArea-100)<1e-8,
                    "all split cap subsets are selected once and retain full cap area");
        const Shape *stationary=f.document.shape(targetId);
        ok &= check(stationary && stationary->controlPointWeldGroups.size()==1 &&
                    stationary->controlPointWeldGroups[0]!=0,"existing target shares inserted solid vertex topology");
        ok &= check(f.history.undoCount()==1 && f.history.undo() && f.document.size()==2 &&
                    f.document.shape(sourceId)->geometryType==GeometryType::NurbsSurface &&
                    f.document.shape(targetId)->controlPointWeldGroups.isEmpty() &&
                    f.history.redo() && f.document.shape(sourceId)->geometryType==GeometryType::NurbsSolid,
                    "split cap, target weld, and source replacement undo atomically");
    }
    {
        Fixture f;
        QVector<ObjectId> ids;
        for(double x:{0.0,10.0}) {
            Shape face;
            face.geometryType=GeometryType::NurbsSurface;
            makeNurbsExtrusionSurface(makeDegreeOneNurbs({{x,0},{x+10,0}}),
                makeWorkPlaneFrame(WorkPlane::XY),{0,10,0},&face.nurbsSurface);
            ids.append(f.document.append(face));
        }
        f.selection.setObjectIds(ids);
        PointExtrudeTool tool;
        tool.begin(f.context);
        // Normal drag in top view resolves depth from screen displacement.
        f.move(tool,{0,5,0});
        ok &= check(tool.status().canCommit,"adjacent face region can extrude");
        tool.commit(f.context);
        const auto completed=tool.preview().completedFaceExtrusions;
        ok &= check(f.document.size()==1 && f.document[0].geometryType==GeometryType::NurbsSolid &&
                    nurbsSolidFaces(f.document[0].nurbsSolid).size()==10,
                    "adjacent selected faces become one solid without internal wall");
        ok &= check(completed.size()==2 && completed[0].objectId==completed[1].objectId &&
                    completed[0].faceIndex!=completed[1].faceIndex,
                    "region completion preserves both moving cap selections");
        ok &= check(f.history.undoCount()==1 && f.history.undo() && f.document.size()==2 &&
                    f.document[0].geometryType==GeometryType::NurbsSurface &&
                    f.document[1].geometryType==GeometryType::NurbsSurface &&
                    f.history.redo() && f.document.size()==1,
                    "connected region replacement is one undo operation");
    }
    return ok?0:1;
}
