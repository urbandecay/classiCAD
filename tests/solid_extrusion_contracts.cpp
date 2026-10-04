#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/geometry_transform.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/history/document_transaction.h"
#include "core/serialization/shape_json_codec.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tools/point_extrude_tool.h"
#include "tools/tool_context.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_scene_renderer.h"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QPainter>

using namespace classiCAD;
namespace {
bool near(const Point3D &a, const Point3D &b)
{
    return std::hypot(a.x-b.x, std::hypot(a.y-b.y, a.z-b.z)) < 1.0e-7;
}
bool check(bool value, const char *message)
{
    if (!value) qCritical() << message;
    return value;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    bool passed = true;
    const QSize size(640,480);
    const auto frame = makeWorkPlaneFrame(WorkPlane::XY);
    const auto circle = makeCircleNurbs({{0,0}, {3,0}});
    NurbsSurface3D face;
    passed &= check(makeNurbsPlanarFillSurface(circle, frame, &face), "circle fill");
    Shape solid;
    solid.geometryType = GeometryType::NurbsSolid;
    solid.workPlaneFrame = frame;
    passed &= check(makeNurbsExtrusionSolid(face, {1,2,5}, &solid.nurbsSolid), "oblique solid extrusion");
    const auto faces = shapeSurfaceFaces(solid);
    passed &= check(faces.size() == 3 && faces[2].rational &&
                        faces[2].degreeU == 2 && faces[2].knotsU == circle.knots &&
                        faces[0].trimLoops[0].curve.knots == face.trimLoops[0].curve.knots,
                    "solid must retain rational walls and exact cap trims");
    qreal start, end;
    nurbsParameterDomain(circle, &start, &end);
    for (int i = 0; i <= 32; ++i) {
        const qreal t = start + (end-start)*i/32;
        QPointF uv;
        evaluateNurbsPoint(face.trimLoops[0].curve, t, &uv);
        Point3D capBase, capTop, wallBase, wallTop;
        const bool evaluated = evaluateNurbsSurfacePoint(faces[0], uv.x(), uv.y(), &capBase) &&
            evaluateNurbsSurfacePoint(faces[1], uv.x(), uv.y(), &capTop) &&
            evaluateNurbsSurfacePoint(faces[2], t, 0, &wallBase) &&
            evaluateNurbsSurfacePoint(faces[2], t, 1, &wallTop);
        passed &= check(evaluated && near(capBase,wallBase) && near(capTop,wallTop),
                        "caps and walls must meet at exact shared boundary evaluations");
    }
    NurbsExtrusionSolid3D invalid;
    passed &= check(!makeNurbsExtrusionSolid(face, {1,2,0}, &invalid) &&
                        !makeNurbsExtrusionSolid(face, {}, &invalid), "zero-volume sweeps rejected");
    auto curved = face;
    curved.controlPoints[3].z = 1;
    passed &= check(!makeNurbsExtrusionSolid(curved, {0,0,5}, &invalid), "curved base rejected");
    auto rectangleFace = face;
    rectangleFace.trimLoops.clear();
    NurbsExtrusionSolid3D rectangularSolid;
    passed &= check(makeNurbsExtrusionSolid(rectangleFace,{0,0,3},&rectangularSolid) &&
                        nurbsSolidFaces(rectangularSolid).size()==3,
                    "untrimmed rectangular plane must close with a perimeter wall");
    auto holedFace = face;
    NurbsSurfaceTrimLoop hole;
    hole.isHole = true;
    hole.curve = face.trimLoops.first().curve;
    for (auto &p : hole.curve.controlPoints) p = QPointF(0.5,0.5)+(p-QPointF(0.5,0.5))*0.4;
    holedFace.trimLoops.append(hole);
    NurbsExtrusionSolid3D holedSolid;
    passed &= check(makeNurbsExtrusionSolid(holedFace,{0,0,3},&holedSolid) &&
                        nurbsSolidFaces(holedSolid).size()==4 &&
                        nurbsSolidFaceReversed(holedSolid,3),
                    "hole boundaries need matching cap holes and inward-facing ruled walls");
    auto orientedFrame = makeWorkPlaneFrameFromNormal({7,-2,9},{1,2,3},{1,0,0});
    NurbsSurface3D orientedFace;
    NurbsExtrusionSolid3D orientedSolid;
    const auto n = orientedFrame.normal;
    passed &= check(makeNurbsPlanarFillSurface(circle,orientedFrame,&orientedFace) &&
                        makeNurbsExtrusionSolid(orientedFace,{n.x*4,n.y*4,n.z*4},&orientedSolid),
                    "face extrusion supports arbitrary oriented workplanes");
    auto negative = solid.nurbsSolid;
    negative.displacement = {0,0,-5};
    passed &= check(validateNurbsSolid(negative) && nurbsSolidFaces(negative).size() == 3 &&
                        nurbsSolidFaceReversed(solid.nurbsSolid,0) &&
                        !nurbsSolidFaceReversed(solid.nurbsSolid,1) &&
                        !nurbsSolidFaceReversed(negative,0) && nurbsSolidFaceReversed(negative,1),
                    "negative extrusion and outward cap orientation");
    Shape restored;
    passed &= check(shapeFromJson(shapeToJson(solid), &restored) &&
                        restored.geometryType == GeometryType::NurbsSolid &&
                        validateNurbsSolid(restored.nurbsSolid) &&
                        near(restored.nurbsSolid.displacement, {1,2,5}), "solid exact JSON roundtrip");
    QJsonObject broken = shapeToJson(solid);
    auto payload = broken["nurbsSolid"].toObject();
    payload["displacement"] = QJsonArray{0,0,0};
    broken["nurbsSolid"] = payload;
    passed &= check(!shapeFromJson(broken, &restored), "invalid solid JSON rejected");
    Shape transformed = solid;
    rotateShapeGeometry(&transformed, {}, {0,1,0}, 1.5707963267948966);
    passed &= check(validateNurbsSolid(transformed.nurbsSolid) &&
                        near(transformed.nurbsSolid.displacement,{5,2,-1}), "rotate solid and displacement together");
    translateShapeGeometry(&transformed, {4,2}, frame);
    scaleShapeGeometry(&transformed, {}, {1,0}, 2, false, frame);
    passed &= check(validateNurbsSolid(transformed.nurbsSolid) &&
                        near(transformed.nurbsSolid.displacement,{10,4,-2}), "uniform scale must scale all solid dimensions");

    SurfaceTessellationCache cache;
    const ObjectId id = ObjectId::fromValue(20);
    const auto baseMesh = cache.acquire(id,1,faces[0],0);
    const auto topMesh = cache.acquire(id,1,faces[1],1);
    passed &= check(baseMesh && topMesh && baseMesh != topMesh && cache.size() == 2 &&
                        cache.acquire(id,1,faces[0],0) == baseMesh, "solid face caches cannot alias");
    const auto depth = buildViewportDepthGeometry(solid);
    passed &= check(!depth.lineVertices.isEmpty() && !depth.surfaceVertices.isEmpty(), "all solid faces render and occlude");
    ViewportTransform transform;
    transform.setViewPreset(ViewportViewPreset::Top);
    CurveHitTester hitTester(&cache);
    QPointF center;
    transform.worldPointToScreenUnclipped({0,0,0},size,&center);
    passed &= check(hitTester.distanceToShape(center,solid,transform,size,id,1) <= 8,
                    "solid cap interior must be selectable");
    ViewportRenderObject object;
    object.shape = solid;
    ViewportSceneStroke stroke;
    passed &= check(makeViewportSceneStrokes(object,true,&stroke), "solid GPU stroke routing");
    QImage image(size,QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    ViewportRenderer renderer(transform,hitTester,&cache);
    renderer.drawShape(painter,solid,size,false,false,false,QColor(Qt::white),{},0,id,1,&depth);
    painter.end();
    bool drawn = false;
    for (int y=0; y<image.height() && !drawn; ++y)
        for (int x=0; x<image.width(); ++x)
            if (image.pixelColor(x,y) != QColor(Qt::black)) { drawn=true; break; }
    passed &= check(drawn,"solid CPU fallback rendering");

    Document document;
    Shape sourceFace;
    sourceFace.geometryType = GeometryType::NurbsSurface;
    sourceFace.nurbsSurface = face;
    sourceFace.workPlaneFrame = frame;
    const auto faceId = document.append(sourceFace);
    Shape curveShape;
    curveShape.geometryType = GeometryType::Circle;
    curveShape.nurbs = circle;
    curveShape.workPlaneFrame = frame;
    const auto curveId = document.append(curveShape);
    SelectionModel selection;
    selection.setObjectIds({faceId,curveId});
    History history(document);
    CurveSampler sampler;
    SnapEngine snaps;
    SnapSettings settings;
    settings.enabled = false;
    snaps.setSettings(settings);
    ToolContext context(document,selection,history,transform,sampler,hitTester,snaps);
    context.setShapesCommitter([&](ToolId, const QVector<Shape> &shapes) {
        DocumentTransaction transaction(document,history);
        for (const auto &shape : shapes)
            if (!transaction.addShape(shape).isValid()) return false;
        return transaction.commit();
    });
    PointExtrudeTool tool;
    tool.begin(context);
    ToolInput input;
    input.viewportSize = size;
    input.workPlaneFrame = transform.workPlaneFrame();
    transform.worldPointToScreenUnclipped(face.controlPoints.first(),size,&input.screenPosition);
    input.screenPosition.ry() -= 70;
    tool.handleMouseMove(input,context);
    const auto preview = tool.preview();
    passed &= check(preview.shapes.size() == 2 &&
                        preview.shapes[0].geometryType == GeometryType::NurbsSolid &&
                        preview.shapes[1].geometryType == GeometryType::NurbsSurface,
                    "one Extrude tool previews face solids and uncapped curve sheets together");
    passed &= check(tool.status().canCommit && preview.shapes.size() == 2 &&
                        preview.shapes[0].nurbsSolid.displacement.z > 0,
                    "face extrusion defaults to its normal, including a top view drag");
    tool.commit(context);
    passed &= check(document.size() == 4 && history.undoCount() == 1 &&
                        history.undo() && document.size() == 2 &&
                        history.redo() && document.size() == 4,
                    "mixed extrusion commits and undoes atomically");
    return passed ? 0 : 1;
}
