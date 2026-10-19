#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/geometry_transform.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/geometry/nurbs_solid_face_extrusion.h"
#include "core/history/document_transaction.h"
#include "core/serialization/shape_json_codec.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tools/point_extrude_tool.h"
#include "tools/tool_context.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_geometry_cache.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_scene_renderer.h"
#include "ui/viewport/blender_grid_renderer.h"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>

#include <algorithm>
#include <iostream>

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

bool probeFaceExtrusion(const NurbsExtrusionSolid3D &solid, int faceIndex,
                        const Point3D &displacement, NurbsExtrusionSolid3D *result,
                        bool manifold=false, int *resultFaceIndex=nullptr)
{
    auto candidate=planNurbsSolidFaceExtrusion(solid,faceIndex,displacement,manifold);
    if (!candidate.succeeded()) {
        std::cerr << "extrusion rejected: face=" << faceIndex
                  << " manifold=" << manifold << " displacement=("
                  << displacement.x << ',' << displacement.y << ',' << displacement.z
                  << ") reason=" << candidate.error.toStdString() << '\n';
        return false;
    }
    if (!result) return false;
    if (resultFaceIndex) *resultFaceIndex=candidate.faceIndex;
    *result=std::move(candidate.solid);
    return true;
}

bool consistentSolidWinding(const NurbsExtrusionSolid3D &solid)
{
    struct Edge { Point3D a, b; };
    QVector<Edge> edges;
    const auto faces = nurbsSolidFaces(solid);
    for (int index = 0; index < faces.size(); ++index) {
        const auto &cv = faces[index].controlPoints;
        if (cv.size() != 4) return false;
        const int order[] = {0, 2, 3, 1};
        for (int side = 0; side < 4; ++side) {
            Edge edge{cv[order[side]], cv[order[(side+1)%4]]};
            if (nurbsSolidFaceReversed(solid,index)) std::swap(edge.a,edge.b);
            edges.append(edge);
        }
    }
    QVector<Edge> segments;
    for (const auto &edge : edges) {
        const Point3D d{edge.b.x-edge.a.x,edge.b.y-edge.a.y,edge.b.z-edge.a.z};
        const double length2=d.x*d.x+d.y*d.y+d.z*d.z;
        if (length2 < 1.0e-16) return false;
        QVector<double> cuts{0,1};
        for (const auto &other : edges) {
            for (const auto &p : {other.a,other.b}) {
                const double t=((p.x-edge.a.x)*d.x+(p.y-edge.a.y)*d.y+
                                (p.z-edge.a.z)*d.z)/length2;
                if (t>1.0e-8 && t<1.0-1.0e-8 &&
                    near(p,{edge.a.x+t*d.x,edge.a.y+t*d.y,edge.a.z+t*d.z})) cuts.append(t);
            }
        }
        std::sort(cuts.begin(),cuts.end());
        for (int i=1;i<cuts.size();++i) {
            if (cuts[i]-cuts[i-1]<1.0e-8) continue;
            const auto at = [&](double t) {
                return Point3D{edge.a.x+t*d.x,edge.a.y+t*d.y,edge.a.z+t*d.z};
            };
            segments.append({at(cuts[i-1]),at(cuts[i])});
        }
    }
    for (int i=0;i<segments.size();++i) {
        int opposing=0;
        for (int j=0;j<segments.size();++j) {
            if (i==j) continue;
            if (near(segments[i].a,segments[j].a) && near(segments[i].b,segments[j].b)) return false;
            if (near(segments[i].a,segments[j].b) && near(segments[i].b,segments[j].a)) ++opposing;
        }
        if (opposing!=1) return false;
    }
    return true;
}

bool nativePlacement(const ViewportRenderObject &object,
                     const ViewportTransform &camera, const QSize &size)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        return QGuiApplication::platformName() == QStringLiteral("offscreen") ||
               check(false, "native placement context creation");
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!context.makeCurrent(&surface)) return check(false, "native placement surface");
    bool passed = true;
    {
        QOpenGLFunctions_3_3_Core functions;
        passed &= check(functions.initializeOpenGLFunctions(), "native placement functions");
        QOpenGLFramebufferObjectFormat framebufferFormat;
        framebufferFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        QOpenGLFramebufferObject framebuffer(size, framebufferFormat);
        ViewportSceneRenderer renderer;
        auto fresh = object;
        fresh.preparedDepthGeometry = QSharedPointer<ViewportDepthGeometry>::create(
            buildViewportDepthGeometry(object.shape));
        fresh.preparedGeometryRevision = fresh.geometryRevision;
        fresh.preparedGeometryOffset = {};
        const auto draw = [&](const ViewportRenderObject &entry) {
            framebuffer.bind();
            functions.glClearColor(0, 0, 0, 1);
            functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ViewportSceneStroke stroke;
            passed &= check(makeViewportSceneStrokes(entry, true, &stroke) &&
                            renderer.draw({stroke}, camera, size, 1),
                            "native translated solid stroke draw");
            functions.glFinish();
            auto image = framebuffer.toImage();
            framebuffer.release();
            return image;
        };
        const auto cachedImage = draw(object);
        const auto freshImage = draw(fresh);
        int differences = 0;
        int painted = 0;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                differences += cachedImage.pixel(x, y) != freshImage.pixel(x, y);
                painted += cachedImage.pixelColor(x, y) != QColor(Qt::black);
            }
        }
        passed &= check(painted > 0 && differences < 200,
                        "GPU translation must match fresh geometry within float raster roundoff");
        BlenderGridRenderer depthRenderer;
        framebuffer.bind();
        passed &= check(depthRenderer.renderToCurrentFramebuffer(
            camera, size, 1, {object}, 10, {}), "native depth renderer initialization");
        Point3D picked;
        const Point3D topCenter{object.preparedGeometryOffset.x + 1,
                              object.preparedGeometryOffset.y + 2, 5};
        QPointF screen;
        camera.worldPointToScreen(topCenter, size, &screen);
        passed &= check(depthRenderer.pickScenePoint(screen, camera, size, 1, {object}, &picked) &&
                        std::abs(picked.z - 5) < 0.05,
                        "GPU depth must follow the translated cap");
        // Reuse the same buffers with a new placement and check the depth again.
        auto shifted = object;
        shifted.preparedGeometryOffset.x += 2;
        camera.worldPointToScreen({topCenter.x + 2, topCenter.y, topCenter.z}, size, &screen);
        passed &= check(depthRenderer.pickScenePoint(screen, camera, size, 1, {shifted}, &picked) &&
                        std::abs(picked.z - 5) < 0.05,
                        "GPU cached depth must update placement without reuploading vertices");
        framebuffer.release();
    }
    context.doneCurrent();
    return passed;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    bool passed = true;
    const QSize size(640,480);
    const auto frame = makeWorkPlaneFrame(WorkPlane::XY);
    const auto circle = makeCircleNurbs({{0,0}, {3,0}});
    NurbsSurface3D ruled;
    passed &= check(makeNurbsExtrusionSurface(circle, frame, {1,2,5}, &ruled),
                    "rational boundary fixture");
    const auto ruledBoundaries = nurbsSurfaceBoundaryCurves(ruled);
    passed &= check(ruledBoundaries.size() == 4, "four exact ruled surface boundaries");
    if (ruledBoundaries.size() == 4) {
        const auto &top = ruledBoundaries[2];
        passed &= check(top.curve.degree == circle.degree &&
                            top.curve.knots == circle.knots &&
                            top.curve.weights == circle.weights &&
                            top.curve.controlPoints.size() == circle.controlPoints.size(),
                        "copied curved boundary preserves NURBS degree knots weights");
        for (int i = 0; i < circle.controlPoints.size(); ++i) {
            passed &= check(top.curve.controlPoints[i] == circle.controlPoints[i] + QPointF(1,2) &&
                                top.curve.normalCoordinates[i] == 5 &&
                                top.sourceControlPointIndices[i] == i * 2 + 1,
                            "boundary copies translated rational CV row rather than CV polygon edges");
        }
    }
    NurbsSurface3D nonclamped;
    nonclamped.degreeU = nonclamped.degreeV = 2;
    nonclamped.orderU = nonclamped.orderV = 3;
    nonclamped.controlVertexCountU = nonclamped.controlVertexCountV = 3;
    nonclamped.rational = true;
    nonclamped.knotsU = nonclamped.knotsV = {0,1,2,3};
    for (int u = 0; u < 3; ++u) {
        for (int v = 0; v < 3; ++v) {
            nonclamped.controlPoints.append({double(u), double(v), double(u*v)});
            nonclamped.weights.append(1.0 + 0.2*u + 0.1*v);
        }
    }
    const auto nonclampedBoundaries = nurbsSurfaceBoundaryCurves(nonclamped);
    passed &= check(nonclampedBoundaries.size() == 4, "nonclamped exact boundary extraction");
    if (nonclampedBoundaries.size() == 4) {
        for (int side = 0; side < 4; ++side) {
            for (int sample = 0; sample <= 8; ++sample) {
                const double parameter = 1.0 + sample / 8.0;
                const double u = side == 0 || side == 2 ? parameter : side == 1 ? 2.0 : 1.0;
                const double v = side == 1 || side == 3 ? parameter : side == 2 ? 2.0 : 1.0;
                Point3D expected, actual;
                passed &= check(evaluateNurbsSurfacePoint(nonclamped, u, v, &expected) &&
                                    evaluateNurbsPoint3D(nonclampedBoundaries[side].curve, parameter, &actual) &&
                                    near(expected, actual),
                                "nonclamped boundary contracts homogeneous basis exactly");
            }
        }
    }
    if (qEnvironmentVariableIsSet("CLASSICAD_SURFACE_BOUNDARY_GEOMETRY_ONLY"))
        return passed ? 0 : 1;
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
                        nurbsSolidFaces(rectangularSolid).size()==6,
                    "rectangular extrusion must expose two caps and four exact wall faces");
    {
        auto splitFace=rectangleFace;
        splitFace.controlVertexCountU=3;
        splitFace.knotsU={2,3,6};
        splitFace.knotsV={-4,8};
        const auto &p=rectangleFace.controlPoints;
        const auto interpolate=[](Point3D a,Point3D b) {
            return Point3D{a.x*0.75+b.x*0.25,a.y*0.75+b.y*0.25,a.z*0.75+b.z*0.25};
        };
        splitFace.controlPoints={p[0],p[1],interpolate(p[0],p[2]),
                                 interpolate(p[1],p[3]),p[2],p[3]};
        splitFace.weights={1,1,1,1,1,1};
        NurbsSurfaceTrimLoop loop;
        loop.curve.degree=1;loop.curve.order=2;
        loop.curve.controlPoints={{2,-4},{6,-4},{6,8},{2,8},{2,-4}};
        loop.curve.weights={1,1,1,1,1};loop.curve.knots={0,1,2,3,4};
        splitFace.trimLoops={loop};
        WorkPlaneFrame splitFrame;
        NurbsExtrusionSolid3D splitSolid;
        passed &= check(nurbsSolidBaseFrame(splitFace,&splitFrame) &&
                            makeNurbsExtrusionSolid(splitFace,{0,0,3},&splitSolid) &&
                            splitSolid.baseSurface.controlVertexCountU==2 &&
                            splitSolid.baseSurface.controlVertexCountV==2 &&
                            splitSolid.baseSurface.knotsU==QVector<qreal>{2,6} &&
                            splitSolid.baseSurface.knotsV==QVector<qreal>{-4,8} &&
                            splitSolid.baseSurface.trimLoops.size()==1 &&
                            splitSolid.baseSurface.trimLoops[0].curve.controlPoints==loop.curve.controlPoints,
                        "a knot-split affine face must extrude into a canonical seed preserving UV domains and trims");
        for (qreal u:{2.0,2.5,3.0,4.0,6.0}) for (qreal v:{-4.0,0.0,8.0}) {
            Point3D original,canonical;
            passed &= check(evaluateNurbsSurfacePoint(splitFace,u,v,&original) &&
                                evaluateNurbsSurfacePoint(splitSolid.baseSurface,u,v,&canonical) &&
                                near(original,canonical),
                            "canonical affine extrusion must retain the exact source UV evaluation");
        }
        splitFace.controlPoints[2].x+=0.1;
        passed &= check(!nurbsSolidBaseFrame(splitFace,&splitFrame) &&
                            !makeNurbsExtrusionSolid(splitFace,{0,0,3},&splitSolid),
                        "a planar piecewise affine net with a changed UV map must not be flattened");
    }

    // Exercise all six faces, both movement directions, and both connected modes.
    const auto sourceFaces = nurbsSolidFaces(rectangularSolid);
    // Match a box made by Fill -> Extrude, whose caps retain a rectangular
    // parameter-domain trim. These used to accept cap motion but reject sides.
    auto trimmedRectangle = rectangleFace;
    qreal trimU0, trimU1, trimV0, trimV1;
    passed &= check(nurbsSurfaceParameterDomains(trimmedRectangle,
                         &trimU0,&trimU1,&trimV0,&trimV1), "rectangle trim domain");
    NurbsSurfaceTrimLoop rectangleTrim;
    rectangleTrim.curve.degree = 1;
    rectangleTrim.curve.order = 2;
    rectangleTrim.curve.controlPoints = {{trimU0,trimV0},{trimU1,trimV0},
        {trimU1,trimV1},{trimU0,trimV1},{trimU0,trimV0}};
    rectangleTrim.curve.weights = {1,1,1,1,1};
    rectangleTrim.curve.knots = {0,1,2,3,4};
    trimmedRectangle.trimLoops = {rectangleTrim};
    NurbsExtrusionSolid3D filledBox;
    passed &= check(makeNurbsExtrusionSolid(trimmedRectangle,{0,0,3},&filledBox),
                    "filled rectangle must produce a valid trimmed box");
    const auto filledFaces = nurbsSolidFaces(filledBox);
    for (int faceIndex = 0; faceIndex < filledFaces.size(); ++faceIndex) {
        const auto &patch = filledFaces[faceIndex];
        const auto a = patch.controlPoints[0];
        const auto b = patch.controlPoints[2];
        const auto c = patch.controlPoints[1];
        Point3D normal{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
                       (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
                       (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
        const double scale = (nurbsSolidFaceReversed(filledBox,faceIndex)?-1.0:1.0) /
            std::hypot(normal.x,std::hypot(normal.y,normal.z));
        for (double distance : {-0.5,0.5}) {
            for (bool manifold : {false,true}) {
                NurbsExtrusionSolid3D edited;
                passed &= check(probeFaceExtrusion(filledBox,faceIndex,
                                    {normal.x*scale*distance,normal.y*scale*distance,
                                     normal.z*scale*distance},&edited,manifold) &&
                                    validateNurbsSolid(edited) &&
                                    nurbsSolidFaces(edited).size() == (manifold || distance < 0 ? 6 : 10),
                                "all Fill-created box faces must extrude inward and outward with Weld or Manifold");
            }
        }
    }
    for (int faceIndex = 0; faceIndex < sourceFaces.size(); ++faceIndex) {
        const auto &patch = sourceFaces[faceIndex];
        const Point3D a = patch.controlPoints[0];
        const Point3D b = patch.controlPoints[2];
        const Point3D c = patch.controlPoints[1];
        Point3D normal{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
                       (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
                       (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
        const double magnitude = std::hypot(normal.x, std::hypot(normal.y, normal.z));
        const double orientation = nurbsSolidFaceReversed(rectangularSolid, faceIndex) ? -1 : 1;
        normal = {normal.x*orientation/magnitude, normal.y*orientation/magnitude,
                  normal.z*orientation/magnitude};
        for (double distance : {-0.5, 0.5}) {
            for (int mode = 0; mode < 2; ++mode) {
                Shape original;
                original.geometryType = GeometryType::NurbsSolid;
                original.nurbsSolid = rectangularSolid;
                Document fixture;
                const ObjectId sourceId = fixture.append(original);
                SelectionModel fixtureSelection;
                History fixtureHistory(fixture);
                ViewportTransform camera;
                camera.setViewPreset(ViewportViewPreset::Isometric);
                camera.zoom() = 24;
                CurveSampler fixtureSampler;
                CurveHitTester fixtureHits;
                SnapEngine fixtureSnaps;
                SnapSettings disabledSnaps;
                disabledSnaps.enabled = false;
                fixtureSnaps.setSettings(disabledSnaps);
                ToolContext fixtureContext(fixture, fixtureSelection, fixtureHistory,
                                           camera, fixtureSampler, fixtureHits, fixtureSnaps);
                PointExtrudeTool fixtureTool;
                PointExtrudeTool::SolidFaceSource source;
                source.objectId = sourceId;
                source.solid = rectangularSolid;
                source.faceIndex = faceIndex;
                source.worldPoint = a;
                source.workPlaneFrame = makeWorkPlaneFrameFromNormal(a, normal, {b.x-a.x,b.y-a.y,b.z-a.z});
                fixtureTool.setSolidFaceSources({source});
                fixtureTool.begin(fixtureContext);
                if (mode == 1) fixtureTool.toggleManifold(fixtureContext);
                ToolInput move;
                move.viewportSize = size;
                move.workPlaneFrame = source.workPlaneFrame;
                camera.worldPointToScreenUnclipped(
                    {a.x+normal.x*distance, a.y+normal.y*distance, a.z+normal.z*distance},
                    size, &move.screenPosition);
                fixtureTool.handleMouseMove(move, fixtureContext);
                const auto modePreview = fixtureTool.preview();
                passed &= check(fixtureTool.status().canCommit && modePreview.shapes.size() == 1,
                                "every supported face must extrude in both directions in every mode");
                if (modePreview.shapes.size() != 1) continue;
                ToolInput zeroMove=move;
                camera.worldPointToScreenUnclipped(a,size,&zeroMove.screenPosition);
                fixtureTool.handleMouseMove(zeroMove,fixtureContext);
                const auto rejectedPreview=fixtureTool.preview();
                passed &= check(!fixtureTool.status().canCommit &&
                                    rejectedPreview.shapes.size()==1 &&
                                    shapeToJson(rejectedPreview.shapes.first())==shapeToJson(modePreview.shapes.first()),
                                "a rejected movement must retain the last accepted display candidate");
                fixtureTool.commit(fixtureContext);
                passed &= check(fixtureHistory.undoCount()==0 && fixture.size()==1,
                                "a rejected endpoint must never commit the retained display candidate");
                fixtureTool.handleMouseMove(move,fixtureContext);
                passed &= check(fixtureTool.status().canCommit &&
                                    fixtureTool.preview().shapes.size()==1 &&
                                    shapeToJson(fixtureTool.preview().shapes.first())==shapeToJson(modePreview.shapes.first()),
                                "returning to a valid endpoint must restore the same exact candidate");
                passed &= check(modePreview.hiddenObjectIds.contains(sourceId) &&
                                    modePreview.extrusionPreviewFaceIndices.first() >= 0,
                                "connected preview must hide its source and highlight the moving face");
                passed &= check(nurbsSolidFaces(modePreview.shapes.first().nurbsSolid).size() ==
                                    (mode == 1 && distance > 0 ? 10 : 6),
                                "divided extrusion retains the seam, while normal extrusion preserves six faces");
                if (mode == 1) {
                    passed &= check(nurbsSolidFaceReversed(
                                        modePreview.shapes.first().nurbsSolid, faceIndex) ==
                                        nurbsSolidFaceReversed(rectangularSolid, faceIndex),
                                    "normal extrusion must preserve the moving face winding in either direction");
                }
                fixtureTool.commit(fixtureContext);
                const auto completion = fixtureTool.preview();
                passed &= check(fixture.size() == 1 &&
                                    completion.completedFaceExtrusions.size() == 1,
                                "both extrusion modes modify the connected source");
                if (completion.completedFaceExtrusions.size() != 1) continue;
                passed &= check(completion.completedFaceExtrusions.first().objectId == sourceId,
                                "extrusion must retain source object identity");
                passed &= check(fixtureHistory.undoCount() == 1 && fixtureHistory.undo() &&
                                    fixture.size() == 1 &&
                                    shapeToJson(*fixture.shape(sourceId)) == shapeToJson(original) &&
                                    fixtureHistory.redo() && fixture.size() == 1,
                                "face extrusion modes must undo and redo atomically");
            }
        }
    }
    NurbsExtrusionSolid3D collapsed;
    // A normal connected extension splits its four side planes into adjacent
    // patches. Recess one such patch without pulling its coplanar neighbor or
    // retaining the old perimeter as an overlapping rim.
    {
        const auto &patch = sourceFaces[2];
        const auto a = patch.controlPoints[0];
        const auto u = Point3D{patch.controlPoints[2].x-a.x,patch.controlPoints[2].y-a.y,patch.controlPoints[2].z-a.z};
        const auto v = Point3D{patch.controlPoints[1].x-a.x,patch.controlPoints[1].y-a.y,patch.controlPoints[1].z-a.z};
        Point3D normal{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
        const double factor = (nurbsSolidFaceReversed(rectangularSolid,2)?-1.0:1.0) /
            std::hypot(normal.x,std::hypot(normal.y,normal.z));
        NurbsExtrusionSolid3D split;
        passed &= check(probeFaceExtrusion(rectangularSolid,2,
                            {normal.x*factor,normal.y*factor,normal.z*factor},&split,false),
                        "normal extension must create the split-face regression shell");
        const auto splitFaces = nurbsSolidFaces(split);
        for (int target = 6; target < splitFaces.size(); ++target) {
            const auto &selected = splitFaces[target];
            const auto p = selected.controlPoints[0];
            const Point3D su{selected.controlPoints[2].x-p.x,selected.controlPoints[2].y-p.y,selected.controlPoints[2].z-p.z};
            const Point3D sv{selected.controlPoints[1].x-p.x,selected.controlPoints[1].y-p.y,selected.controlPoints[1].z-p.z};
            Point3D n{su.y*sv.z-su.z*sv.y,su.z*sv.x-su.x*sv.z,su.x*sv.y-su.y*sv.x};
            const double scale = (nurbsSolidFaceReversed(split,target)?-0.25:0.25) /
                std::hypot(n.x,std::hypot(n.y,n.z));
            for (bool manifold : {false,true}) {
                NurbsExtrusionSolid3D recessed;
                const Point3D delta{-n.x*scale,-n.y*scale,-n.z*scale};
                const bool success = probeFaceExtrusion(split,target,delta,&recessed,manifold);
                passed &= check(success && validateNurbsSolid(recessed),
                                "a split box face must recess with Manifold on or off");
                if (!success) continue;
                const auto resultFaces = nurbsSolidFaces(recessed);
                passed &= check(resultFaces.size() == splitFaces.size()+1,
                                "a recess must leave one step wall and shrink existing neighbors without four overlapping rim walls");
                for (int cv=0; cv<4; ++cv) {
                    const auto before = selected.controlPoints[cv];
                    passed &= check(near(resultFaces[target].controlPoints[cv],
                                         {before.x+delta.x,before.y+delta.y,before.z+delta.z}),
                                    "the selected face must end at the requested recess plane");
                }
                for (int next=0; next<resultFaces.size(); ++next) {
                    const auto &nextFace=resultFaces[next];
                    const auto a=nextFace.controlPoints[0];
                    const Point3D u{nextFace.controlPoints[2].x-a.x,nextFace.controlPoints[2].y-a.y,nextFace.controlPoints[2].z-a.z};
                    const Point3D v{nextFace.controlPoints[1].x-a.x,nextFace.controlPoints[1].y-a.y,nextFace.controlPoints[1].z-a.z};
                    const Point3D normal{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
                    const double factor=(nurbsSolidFaceReversed(recessed,next)?0.01:-0.01) /
                        std::hypot(normal.x,std::hypot(normal.y,normal.z));
                    NurbsExtrusionSolid3D again;
                    passed &= check(probeFaceExtrusion(recessed,next,
                                        {normal.x*factor,normal.y*factor,normal.z*factor},&again,true) &&
                                        validateNurbsSolid(again) &&
                                        probeFaceExtrusion(recessed,next,
                                        {-normal.x*factor,-normal.y*factor,-normal.z*factor},&again,true) &&
                                        validateNurbsSolid(again),
                                    "every face of the stepped result must remain extrudable in both directions, including partial edge neighbors");
                }
                // Raise the recessed face to, and then past, its neighbor.
                // Preview must remain valid on both sides of the zero-height
                // step wall rather than reverting to the original solid.
                for (double distance : {0.125,0.25,0.5,1.0}) {
                  for (bool raiseManifold : {false,true}) {
                    const Point3D rise{-delta.x*distance/0.25,
                                       -delta.y*distance/0.25,
                                       -delta.z*distance/0.25};
                    NurbsExtrusionSolid3D raised;
                    int moved=-1;
                    const bool success=probeFaceExtrusion(recessed,target,rise,&raised,raiseManifold,&moved);
                    passed &= check(success && validateNurbsSolid(raised),
                                    "Manifold must raise a recessed face through and beyond its neighbor's height");
                    if (!success) continue;
                    const auto raisedFaces=nurbsSolidFaces(raised);
                    passed &= check(moved>=0 && moved<raisedFaces.size() &&
                                        near(raisedFaces[moved].controlPoints[0],
                                            {resultFaces[target].controlPoints[0].x+rise.x,
                                             resultFaces[target].controlPoints[0].y+rise.y,
                                             resultFaces[target].controlPoints[0].z+rise.z}),
                                    "crossing a neighbor must report the actual moving face index");
                  }
                }
                auto reordered=recessed;
                const int lateFace=reordered.boundaryFaces.size()-1;
                std::swap(reordered.boundaryFaces[target],reordered.boundaryFaces[lateFace]);
                const bool oldFlag=reordered.boundaryFaceReversed[target];
                reordered.boundaryFaceReversed[target]=reordered.boundaryFaceReversed[lateFace];
                reordered.boundaryFaceReversed[lateFace]=oldFlag;
                NurbsExtrusionSolid3D flush;
                int flushFace=-1;
                passed &= check(probeFaceExtrusion(reordered,lateFace,
                                    {-delta.x,-delta.y,-delta.z},&flush,true,&flushFace) &&
                                    validateNurbsSolid(flush) && flushFace==lateFace-1,
                                "removing an earlier collapsed wall must remap a later selected face");

                // Carry each committed result forward: isolated probes miss
                // stale winding and topology left by an earlier edit.
                for (bool chainManifold : {false,true}) {
                    auto current=recessed;
                    int moving=target;
                    double previousHeight=0;
                    int step=0;
                    for (double height : {0.125,0.25,0.5,0.25,0.125,0.0}) {
                        const double change=(height-previousHeight)/0.25;
                        const Point3D movement{-delta.x*change,-delta.y*change,-delta.z*change};
                        const auto before=nurbsSolidFaces(current)[moving];
                        NurbsExtrusionSolid3D next;
                        int nextMoving=-1;
                        const bool moved=probeFaceExtrusion(current,moving,movement,
                                                              &next,chainManifold,&nextMoving);
                        passed &= check(moved && validateNurbsSolid(next) && consistentSolidWinding(next),
                                        "chained extrusion must remain closed and consistently oriented below, at, and above neighboring faces");
                        if (!moved) break;
                        const auto after=nurbsSolidFaces(next);
                        const bool mapped=nextMoving>=0 && nextMoving<after.size();
                        passed &= check(mapped,"each chained edit must return the surviving moving face");
                        if (!mapped) break;
                        for (int cv=0;cv<4;++cv) {
                            const auto p=before.controlPoints[cv];
                            passed &= check(near(after[nextMoving].controlPoints[cv],
                                                 {p.x+movement.x,p.y+movement.y,p.z+movement.z}),
                                            "chained extrusion must translate every moving control vertex exactly");
                        }
                        current=std::move(next);
                        moving=nextMoving;
                        previousHeight=height;
                        if (++step==3) {
                            Shape stored;
                            stored.geometryType=GeometryType::NurbsSolid;
                            stored.nurbsSolid=current;
                            Shape restored;
                            const bool loaded=shapeFromJson(shapeToJson(stored),&restored);
                            passed &= check(loaded && validateNurbsSolid(restored.nurbsSolid) &&
                                                consistentSolidWinding(restored.nurbsSolid),
                                            "serialized midpoint must preserve the editable oriented shell");
                            if (!loaded) break;
                            current=restored.nurbsSolid;
                        }
                    }
                    const auto finalFaces=nurbsSolidFaces(current);
                    for (int index=0;index<finalFaces.size();++index) {
                        const auto &cv=finalFaces[index].controlPoints;
                        const Point3D u{cv[2].x-cv[0].x,cv[2].y-cv[0].y,cv[2].z-cv[0].z};
                        const Point3D v{cv[1].x-cv[0].x,cv[1].y-cv[0].y,cv[1].z-cv[0].z};
                        Point3D direction{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
                        const double factor=(nurbsSolidFaceReversed(current,index)?-0.01:0.01)/
                            std::hypot(direction.x,std::hypot(direction.y,direction.z));
                        direction={direction.x*factor,direction.y*factor,direction.z*factor};
                        NurbsExtrusionSolid3D outward,returned;
                        int outwardFace=-1,returnedFace=-1;
                        const bool out=probeFaceExtrusion(current,index,direction,&outward,true,&outwardFace);
                        const bool back=out && probeFaceExtrusion(outward,outwardFace,
                            {-direction.x,-direction.y,-direction.z},&returned,true,&returnedFace);
                        passed &= check(out && back && validateNurbsSolid(outward) &&
                                            validateNurbsSolid(returned) && consistentSolidWinding(outward) &&
                                            consistentSolidWinding(returned),
                                        "every face after a committed chain must extrude outward and return with coherent winding");
                    }
                    // Twenty committed steps, each outward move followed by
                    // its exact inverse, avoid crossing an unrelated wall or
                    // choosing tiny slivers below the shell tolerance.
                    auto stress=current;
                    for (int pair=0;pair<10;++pair) {
                        const auto stressFaces=nurbsSolidFaces(stress);
                        QVector<int> eligible;
                        for (int face=0;face<stressFaces.size();++face) {
                            const auto &p=stressFaces[face].controlPoints;
                            if (std::hypot(p[2].x-p[0].x,std::hypot(p[2].y-p[0].y,p[2].z-p[0].z))>=0.1 &&
                                std::hypot(p[1].x-p[0].x,std::hypot(p[1].y-p[0].y,p[1].z-p[0].z))>=0.1)
                                eligible.append(face);
                        }
                        passed &= check(!eligible.isEmpty(),"stress fixture must retain a face above modeling tolerance");
                        if (eligible.isEmpty()) break;
                        int selected=eligible[(pair*7+3)%eligible.size()];
                        const auto &cv=stressFaces[selected].controlPoints;
                        const Point3D u{cv[2].x-cv[0].x,cv[2].y-cv[0].y,cv[2].z-cv[0].z};
                        const Point3D v{cv[1].x-cv[0].x,cv[1].y-cv[0].y,cv[1].z-cv[0].z};
                        Point3D normal{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
                        const double span=std::min(std::hypot(u.x,std::hypot(u.y,u.z)),
                                                   std::hypot(v.x,std::hypot(v.y,v.z)));
                        const double orientation=nurbsSolidFaceReversed(stress,selected)?-1:1;
                        const double factor=orientation*span*0.001/
                            std::hypot(normal.x,std::hypot(normal.y,normal.z));
                        const Point3D outward{normal.x*factor,normal.y*factor,normal.z*factor};
                        bool pairSucceeded=true;
                        for (int direction=0;direction<2;++direction) {
                            const Point3D movement=direction==0?outward:Point3D{-outward.x,-outward.y,-outward.z};
                            const auto before=nurbsSolidFaces(stress)[selected];
                            NurbsExtrusionSolid3D next;
                            int movedFace=-1;
                            const bool success=probeFaceExtrusion(stress,selected,movement,&next,
                                                                  chainManifold,&movedFace);
                            passed &= check(success && validateNurbsSolid(next) && consistentSolidWinding(next),
                                            "twenty committed edits on alternating faces and directions must preserve a closed oriented shell");
                            if (!success) { pairSucceeded=false; break; }
                            const auto after=nurbsSolidFaces(next);
                            const bool mapped=movedFace>=0 && movedFace<after.size();
                            passed &= check(mapped,"stress edit must preserve its moving face identity");
                            if (!mapped) { pairSucceeded=false; break; }
                            for (int index=0;index<4;++index) {
                                const auto p=before.controlPoints[index];
                                passed &= check(near(after[movedFace].controlPoints[index],
                                                     {p.x+movement.x,p.y+movement.y,p.z+movement.z}),
                                                "stress edit must apply the requested displacement exactly");
                            }
                            stress=std::move(next);
                            selected=movedFace;
                        }
                        if (!pairSucceeded) break;
                    }
                }
            }
        }
    }
    // Cross the coplanar subdivision seam left by a previous normal extension.
    // The consumed side patches must disappear rather than overlap the slab.
    for (bool extendTop : {false,true}) {
        NurbsExtrusionSolid3D stacked;
        const int extendedFace=extendTop?1:0;
        const Point3D extension{0,0,extendTop?1.0:-1.0};
        const bool extended=probeFaceExtrusion(rectangularSolid,extendedFace,extension,&stacked,false);
        passed &= check(extended && nurbsSolidFaces(stacked).size()==10,
                        "ordinary outward cap extension must retain the original wall seam");
        if (!extended) continue;
        for (bool manifold : {false,true}) {
            const int selected=extendTop?0:1;
            const Point3D movement{0,0,extendTop?3.5:-3.5};
            const auto before=nurbsSolidFaces(stacked)[selected];
            NurbsExtrusionSolid3D slab;
            int movedFace=-1;
            const bool success=probeFaceExtrusion(stacked,selected,movement,&slab,manifold,&movedFace);
            passed &= check(success && validateNurbsSolid(slab) && consistentSolidWinding(slab),
                            "inward cap extrusion must cross a prior wall subdivision seam and retain a closed oriented slab");
            if (!success) continue;
            const auto faces=nurbsSolidFaces(slab);
            const bool mapped=movedFace>=0 && movedFace<faces.size();
            passed &= check(mapped,"crossing a prior wall seam must preserve the moving cap identity");
            if (!mapped) continue;
            for (int cv=0;cv<4;++cv) {
                const auto p=before.controlPoints[cv];
                passed &= check(near(faces[movedFace].controlPoints[cv],
                                     {p.x,p.y,p.z+movement.z}),
                                "cap crossing a subdivision seam must reach its exact requested plane");
            }
            double volume=0;
            for (int face=0;face<faces.size();++face) {
                const auto &cv=faces[face].controlPoints;
                const auto tetra = [](const Point3D &a,const Point3D &b,const Point3D &c) {
                    return (a.x*(b.y*c.z-b.z*c.y)+a.y*(b.z*c.x-b.x*c.z)+
                            a.z*(b.x*c.y-b.y*c.x))/6.0;
                };
                const double orientation=nurbsSolidFaceReversed(slab,face)?-1:1;
                volume+=orientation*(tetra(cv[0],cv[2],cv[3])+tetra(cv[0],cv[3],cv[1]));
                for (const auto &point : cv) {
                    const double lower=extendTop?3.5:-1.0;
                    const double upper=extendTop?4.0:-0.5;
                    passed &= check(point.z>=lower-1.0e-7 && point.z<=upper+1.0e-7,
                                    "consumed wall patches must not protrude beyond the remaining slab");
                }
            }
            passed &= check(std::abs(volume-18.0)<1.0e-6,
                            "crossing a prior wall seam must leave exactly the outward-oriented 6 by 6 by 0.5 slab");
            Shape placed;
            placed.geometryType=GeometryType::NurbsSolid;
            placed.nurbsSolid=stacked;
            const Point3D rotationAxis{1,2,3};
            passed &= check(rotateShapeGeometry(&placed,{},rotationAxis,0.731) &&
                                bakeShapePlacementTranslation(&placed,{10000,-20000,30000}),
                            "the seam regression must support arbitrary world orientation and placement");
            const Point3D rotatedMovement=rotatePointAboutAxis(movement,{},rotationAxis,0.731);
            const auto placedBefore=nurbsSolidFaces(placed.nurbsSolid)[selected];
            NurbsExtrusionSolid3D placedSlab;
            int placedFace=-1;
            const bool placedSuccess=probeFaceExtrusion(placed.nurbsSolid,selected,rotatedMovement,
                                                         &placedSlab,manifold,&placedFace);
            passed &= check(placedSuccess && validateNurbsSolid(placedSlab) &&
                                consistentSolidWinding(placedSlab),
                            "crossing an old wall seam must be independent of world axes and placement");
            if (placedSuccess && placedFace>=0) {
                const auto moved=nurbsSolidFaces(placedSlab)[placedFace];
                for (int cv=0;cv<4;++cv) {
                    const auto p=placedBefore.controlPoints[cv];
                    passed &= check(near(moved.controlPoints[cv],
                        {p.x+rotatedMovement.x,p.y+rotatedMovement.y,p.z+rotatedMovement.z}),
                        "a rotated moving cap must retain exact world displacement");
                }
            }
        }
    }
    // The opposite cap may be subdivided without changing the solid. A ray
    // through its shared seam still crosses the boundary only once.
    {
        auto subdivided=rectangularSolid;
        passed &= check(materializeNurbsSolidBoundary(&subdivided),"subdivided cap fixture");
        auto left=subdivided.boundaryFaces[1],right=left;
        const auto midpoint=[](Point3D a,Point3D b) {
            return Point3D{(a.x+b.x)*0.5,(a.y+b.y)*0.5,(a.z+b.z)*0.5};
        };
        left.controlPoints[2]=right.controlPoints[0]=midpoint(left.controlPoints[0],left.controlPoints[2]);
        left.controlPoints[3]=right.controlPoints[1]=midpoint(left.controlPoints[1],left.controlPoints[3]);
        const double middle=(left.knotsU[0]+left.knotsU[1])*0.5;
        left.knotsU[1]=middle;right.knotsU[0]=middle;
        subdivided.boundaryFaces[1]=left;
        subdivided.boundaryFaces.append(right);
        subdivided.boundaryFaceReversed.append(subdivided.boundaryFaceReversed[1]);
        const QVector<int> region{1,static_cast<int>(subdivided.boundaryFaces.size()-1)};
        for (bool manifold:{false,true}) {
            const auto extrusion=planNurbsSolidRegionExtrusion(
                subdivided,region,{0,0,0.5},manifold);
            passed &= check(extrusion.succeeded() && extrusion.faceIndices.size()==2 &&
                            validateNurbsSolid(extrusion.solid) &&
                            consistentSolidWinding(extrusion.solid),
                            "adjacent coplanar selected faces must extrude as one closed region");
            if (extrusion.succeeded()) {
                for (int slot=0;slot<region.size();++slot) {
                    const auto &moved=extrusion.solid.boundaryFaces[extrusion.faceIndices[slot]];
                    const auto &original=subdivided.boundaryFaces[region[slot]];
                    for (int cv=0;cv<4;++cv)
                        passed &= check(near(moved.controlPoints[cv],
                            {original.controlPoints[cv].x,original.controlPoints[cv].y,
                             original.controlPoints[cv].z+0.5}),
                            "region face mappings must preserve selected face order and exact displacement");
                }
            }
        }
        passed &= check(!planNurbsSolidRegionExtrusion(
                            subdivided,{0,1},{0,0,0.5},true).succeeded(),
                        "opposite cap selection must reject a noncoplanar shared region");
        for (bool manifold:{false,true}) {
            NurbsExtrusionSolid3D extended;
            int moving=-1;
            const bool success=probeFaceExtrusion(subdivided,0,{0,0,-0.5},&extended,manifold,&moving);
            passed &= check(success && validateNurbsSolid(extended) && consistentSolidWinding(extended),
                            "opposite-face subdivisions must not reverse material classification");
            if (success) {
                const auto extendedFaces=nurbsSolidFaces(extended);
                for (Point3D p:extendedFaces[moving].controlPoints)
                    passed &= check(std::abs(p.z+0.5)<1e-8,"subdivided cap must allow the opposite face to move outward");
            }
        }
    }
    passed &= check(!probeFaceExtrusion(rectangularSolid, 1, {0,0,-3}, &collapsed, true) &&
                        !probeFaceExtrusion(rectangularSolid, 1, {0,0,-4}, &collapsed, true),
                    "Manifold must reject collapse and crossing the opposite cap");
    auto slanted = rectangularSolid;
    slanted.displacement = {1,0,3};
    NurbsExtrusionSolid3D slantedExtension;
    passed &= check(probeFaceExtrusion(slanted,1,{0,0,1},&slantedExtension,false) &&
                        validateNurbsSolid(slantedExtension) && consistentSolidWinding(slantedExtension) &&
                        nurbsSolidFaces(slantedExtension).size()==10,
                    "ordinary outward extrusion of a convex oblique seed must retain its exact perimeter seam");
    passed &= check(!probeFaceExtrusion(slanted, 1, {0,0,1}, &collapsed, true),
                    "Manifold must reject resizing nonorthogonal neighboring walls");

    // Moving a top cap back toward the base must shorten the existing solid.
    {
        Shape sourceSolid;
        sourceSolid.geometryType = GeometryType::NurbsSolid;
        sourceSolid.nurbsSolid = rectangularSolid;
        Document capDocument;
        const ObjectId capObject = capDocument.append(sourceSolid);
        SelectionModel capSelection;
        History capHistory(capDocument);
        ViewportTransform capTransform;
        capTransform.zoom() = 24.0;
        capTransform.setViewPreset(ViewportViewPreset::Top);
        CurveSampler capSampler;
        CurveHitTester capHitTester;
        SnapEngine capSnaps;
        SnapSettings capSnapSettings;
        capSnapSettings.enabled = false;
        capSnaps.setSettings(capSnapSettings);
        ToolContext capContext(capDocument, capSelection, capHistory,
                               capTransform, capSampler, capHitTester,
                               capSnaps);
        PointExtrudeTool capTool;
        PointExtrudeTool::SolidFaceSource capSource;
        capSource.objectId = capObject;
        capSource.solid = rectangularSolid;
        capSource.faceIndex = 1;
        capSource.worldPoint = rectangleFace.controlPoints.first();
        capSource.worldPoint.z += rectangularSolid.displacement.z;
        capSource.workPlaneFrame = makeWorkPlaneFrameFromNormal(
            capSource.worldPoint, {0,0,1}, {1,0,0});
        capTool.setSolidFaceSources({capSource});
        capTool.begin(capContext);
        ToolInput downward;
        downward.viewportSize = size;
        downward.workPlaneFrame = capSource.workPlaneFrame;
        capTransform.worldPointToScreenUnclipped(
            capSource.worldPoint, size, &downward.screenPosition);
        downward.screenPosition.ry() += 12.0;
        capTool.handleMouseMove(downward, capContext);
        const auto shortenedPreview = capTool.preview();
        passed &= check(capTool.status().canCommit &&
                            shortenedPreview.shapes.size() == 1 &&
                            shortenedPreview.shapes.first().nurbsSolid.displacement.z > 0.0 &&
                            shortenedPreview.shapes.first().nurbsSolid.displacement.z <
                                rectangularSolid.displacement.z,
                        "dragging a top cap inward must preview a shorter valid solid");
        capTool.commit(capContext);
        const Shape *shortened = capDocument.shape(capObject);
        passed &= check(shortened != nullptr &&
                            shortened->geometryType == GeometryType::NurbsSolid &&
                            shortened->nurbsSolid.displacement.z > 0.0 &&
                            shortened->nurbsSolid.displacement.z <
                                rectangularSolid.displacement.z,
                        "inward cap extrusion must commit by moving the cap toward the base");
    }
    const auto rectangleFaces = nurbsSolidFaces(rectangularSolid);
    Point3D solidCenter;
    for (const auto &point : rectangleFace.controlPoints) {
        solidCenter.x += point.x / 4.0;
        solidCenter.y += point.y / 4.0;
        solidCenter.z += point.z / 4.0;
    }
    solidCenter.z += 1.5;
    for (int index = 0; index < rectangleFaces.size(); ++index) {
        const auto &panel = rectangleFaces[index];
        const auto &a = panel.controlPoints[0];
        const auto &b = panel.controlPoints[2];
        const auto &c = panel.controlPoints[1];
        Point3D normal{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
                       (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
                       (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
        const double sign = nurbsSolidFaceReversed(rectangularSolid,index) ? -1.0 : 1.0;
        passed &= check(sign * (normal.x*(a.x-solidCenter.x) +
                                normal.y*(a.y-solidCenter.y) +
                                normal.z*(a.z-solidCenter.z)) > 0,
                        "each derived rectangular face must retain outward orientation");
        if (index >= 2) {
            passed &= check(panel.controlVertexCountU==2 && panel.controlVertexCountV==2 &&
                                panel.knotsU==QVector<double>{double(index-2),double(index-1)},
                            "linear wall panels must preserve their exact NURBS span domains");
        }
    }
    auto holedFace = face;
    {
        Shape junction;
        junction.geometryType = GeometryType::NurbsSolid;
        junction.nurbsSolid = rectangularSolid;
        passed &= check(materializeNurbsSolidBoundary(&junction.nurbsSolid),
                        "junction fixture materializes");
        const auto original = junction.nurbsSolid.boundaryFaces[1];
        const auto midpoint = [](const Point3D &a, const Point3D &b) {
            return Point3D{(a.x+b.x)/2, (a.y+b.y)/2, (a.z+b.z)/2};
        };
        auto first = original, second = original;
        first.controlPoints[2] = second.controlPoints[0] =
            midpoint(original.controlPoints[0], original.controlPoints[2]);
        first.controlPoints[3] = second.controlPoints[1] =
            midpoint(original.controlPoints[1], original.controlPoints[3]);
        const qreal middleU = (original.knotsU.first()+original.knotsU.last())/2;
        first.knotsU.last() = second.knotsU.first() = middleU;
        junction.nurbsSolid.boundaryFaces[1] = first;
        junction.nurbsSolid.boundaryFaces.append(second);
        junction.nurbsSolid.boundaryFaceReversed.append(
            junction.nurbsSolid.boundaryFaceReversed[1]);
        const auto before = nurbsSolidControlPoints(junction.nurbsSolid);
        int selected = -1;
        for (int i=0; i<before.size(); ++i)
            if (near(before[i], first.controlPoints[2])) selected = i;
        junction.controlPointWeldGroups.fill(0, before.size());
        if (selected >= 0) junction.controlPointWeldGroups[selected] = 741;
        passed &= check(selected >= 0 && transformShapeControlPoints(&junction, {selected},
                            [](const Point3D &p) { return Point3D{p.x+0.2,p.y-0.1,p.z+0.4}; }) &&
                            validateNurbsSolid(junction.nurbsSolid),
                        "moving a T junction subdivides incident faces and keeps a closed smooth solid");
        const auto after = nurbsSolidControlPoints(junction.nurbsSolid);
        bool retainedGroup = false;
        for (int i=0; i<after.size(); ++i) {
            const auto &old = first.controlPoints[2];
            if (near(after[i], {old.x+0.2,old.y-0.1,old.z+0.4}))
                retainedGroup = junction.controlPointWeldGroups.value(i) == 741;
        }
        passed &= check(retainedGroup,
                        "junction subdivision remaps existing weld identity by world position");
    }
    {
        Shape edited;
        edited.geometryType = GeometryType::NurbsSolid;
        edited.nurbsSolid = rectangularSolid;
        passed &= check(materializeNurbsSolidBoundary(&edited.nurbsSolid) &&
                            edited.nurbsSolid.boundaryFaces.size() == 6,
                        "rectangular extrusion must preserve six exact editable faces");
        const auto before = edited.nurbsSolid.boundaryFaces;
        const Point3D corner = before[1].controlPoints[0];
        const Point3D moved{corner.x + 0.3, corner.y - 0.2, corner.z + 0.8};
        int affected = 0;
        for (auto &patch : edited.nurbsSolid.boundaryFaces) {
            for (Point3D &point : patch.controlPoints) {
                if (near(point, corner)) { point = moved; ++affected; }
            }
        }
        passed &= check(affected == 3 && validateNurbsSolid(edited.nurbsSolid),
                        "one cube corner must deform all three adjoining faces and stay closed");
        passed &= check(near(edited.nurbsSolid.boundaryFaces[0].controlPoints[0],
                             before[0].controlPoints[0]),
                        "opposite cap corner must stay fixed during a vertex edit");
        ViewportRenderObject object;
        object.shape = edited;
        passed &= check(!buildViewportDepthGeometry(object).surfaceVertices.isEmpty(),
                        "deformed nonplanar NURBS solid must remain visible");
        Shape roundtrip;
        passed &= check(shapeFromJson(shapeToJson(edited), &roundtrip) &&
                            roundtrip.nurbsSolid.boundaryFaces.size() == 6 &&
                            near(roundtrip.nurbsSolid.boundaryFaces[1].controlPoints[0], moved),
                        "native save/reload must preserve the edited NURBS faces");
        rotateShapeGeometry(&roundtrip, {}, {0,1,0}, 0.4);
        passed &= check(validateNurbsSolid(roundtrip.nurbsSolid) &&
                            !near(roundtrip.nurbsSolid.boundaryFaces[1].controlPoints[0], moved),
                        "transforms must update authoritative edited faces");
    }
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
    PreparedNurbsSurfaceTessellation wallMesh;
    PreparedNurbsSurfaceTessellation capMesh;
    passed &= check(capMesh.prepare(faces[0]) && capMesh.triangles().size() <= 256,
                    "convex flat caps must use their boundary instead of a dense interior grid");
    double capArea = 0;
    for (const auto &triangle : capMesh.triangles()) {
        const auto &a = capMesh.vertices()[triangle[0]];
        const auto &b = capMesh.vertices()[triangle[1]];
        const auto &c = capMesh.vertices()[triangle[2]];
        const double area = ((b.x - a.x) * (c.y - a.y) -
                             (b.y - a.y) * (c.x - a.x)) * 0.5;
        passed &= check(area >= 0, "convex cap triangles must retain UV orientation");
        capArea += area;
    }
    passed &= check(std::abs(capArea - 9 * 3.141592653589793) < 0.01,
                    "convex cap triangles must cover the circular boundary accurately");
    PreparedNurbsSurfaceTessellation holeMesh;
    passed &= check(holeMesh.prepare(holedFace) && holeMesh.triangles().size() > 256,
                    "holed caps must retain trim-aware triangulation");
    passed &= check(wallMesh.prepare(faces[2]) && wallMesh.triangles().size() == 96 &&
                        wallMesh.vertices().size() == 98,
                    "linear extrusion walls must retain curved samples without redundant V rows");
    for (int u = 0; u < 49; ++u) {
        const auto &base = wallMesh.vertices()[u * 2];
        const auto &top = wallMesh.vertices()[u * 2 + 1];
        passed &= check(near({top.x - base.x, top.y - base.y, top.z - base.z},
                            solid.nurbsSolid.displacement),
                        "reduced wall mesh must preserve its exact extrusion vector");
    }
    auto varyingWall = faces[2];
    varyingWall.controlPoints[3].x += 0.5;
    PreparedNurbsSurfaceTessellation varyingMesh;
    passed &= check(varyingMesh.prepare(varyingWall) && varyingMesh.triangles().size() > 96,
                    "a varying ruled wall must keep the full surface tessellation path");
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

    // A group move must retain immutable meshes and carry placement separately.
    // Exceed the face-cache capacity to catch regeneration hidden by small scenes.
    Document group;
    for (int i = 0; i < 64; ++i) {
        Shape placed = solid;
        translateShapeGeometry(&placed, {double(i % 8) * 12, double(i / 8) * 12}, frame);
        group.append(placed);
    }
    const auto stationaryId = group.append(solid);
    SurfaceTessellationCache groupSurfaceCache;
    ViewportGeometryCache groupGeometryCache;
    auto initialFrame = buildViewportRenderFrame(group, transform, size, {});
    groupGeometryCache.prepareFrame(&initialFrame, &groupSurfaceCache);
    const auto initialBufferKey = viewportDepthGeometryCacheKey(initialFrame.objects, false);
    const auto initialPlacementKey = viewportDepthGeometryCacheKey(initialFrame.objects);
    for (int step = 1; step <= 3; ++step) {
        for (int i = 0; i < 64; ++i) {
            group.mutateGeometry(group.objectIdAt(i), [&](Shape &shape) {
                translateShapeGeometry(&shape, {2, -1}, frame);
                return true;
            });
        }
        auto movedFrame = buildViewportRenderFrame(group, transform, size, {});
        groupGeometryCache.prepareFrame(&movedFrame, &groupSurfaceCache);
        for (int i = 0; i < 64; ++i) {
            const auto &moved = movedFrame.objects[i];
            ViewportSceneStroke movingStroke;
            passed &= check(moved.preparedDepthGeometry == initialFrame.objects[i].preparedDepthGeometry &&
                near(moved.preparedGeometryOffset, {step * 2.0, step * -1.0, 0}) &&
                makeViewportSceneStrokes(moved, true, &movingStroke) &&
                near(movingStroke.worldOffset, moved.preparedGeometryOffset) &&
                movingStroke.geometryRevision == initialFrame.objects[i].geometryRevision,
                "large group translation must retain mesh identity and stable GPU keys");
        }
        passed &= check(movedFrame.find(stationaryId)->preparedDepthGeometry ==
                            initialFrame.find(stationaryId)->preparedDepthGeometry &&
                        viewportDepthGeometryCacheKey(movedFrame.objects, false) == initialBufferKey &&
                        viewportDepthGeometryCacheKey(movedFrame.objects) != initialPlacementKey,
                        "translation must retain buffer keys while invalidating placed raster geometry");
        const auto &first = movedFrame.objects.first();
        const auto expanded = buildViewportDepthGeometry(QVector<ViewportRenderObject>{first});
        const auto fresh = buildViewportDepthGeometry(first.shape);
        passed &= check(expanded.preciseLineVertices.size() == fresh.preciseLineVertices.size() &&
            near(expanded.preciseLineVertices.first(), fresh.preciseLineVertices.first()),
            "placed depth geometry must match the translated exact solid");
        QImage cachedImage(size, QImage::Format_ARGB32_Premultiplied);
        QImage freshImage(size, QImage::Format_ARGB32_Premultiplied);
        cachedImage.fill(Qt::black);
        freshImage.fill(Qt::black);
        QPainter cachedPainter(&cachedImage);
        renderer.drawShape(cachedPainter, first.shape, size, false, false, false,
            QColor(Qt::white), {}, 0, first.objectId, first.geometryRevision,
            first.preparedDepthGeometry.data(), first.preparedGeometryOffset);
        cachedPainter.end();
        QPainter freshPainter(&freshImage);
        renderer.drawShape(freshPainter, first.shape, size, false, false, false,
            QColor(Qt::white), {}, 0, first.objectId, first.geometryRevision, &fresh);
        freshPainter.end();
        passed &= check(cachedImage == freshImage,
                        "CPU fallback must draw translated cached meshes at the correct position");
        if (step == 3) passed &= nativePlacement(first, transform, size);
    }
    group.mutateGeometry(group.objectIdAt(0), [](Shape &shape) {
        shape.nurbsSolid.displacement.z += 1;
        return true;
    });
    auto deformedFrame = buildViewportRenderFrame(group, transform, size, {});
    groupGeometryCache.prepareFrame(&deformedFrame, &groupSurfaceCache);
    passed &= check(deformedFrame.objects[0].preparedDepthGeometry !=
                        initialFrame.objects[0].preparedDepthGeometry &&
                    near(deformedFrame.objects[0].preparedGeometryOffset, {}),
                    "a changed extrusion vector must rebuild the mesh and reset placement");

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
    // Both selected sources become their connected extrusions in one operation.
    passed &= check(document.size() == 2 && history.undoCount() == 1 &&
                        history.undo() && document.size() == 2 &&
                        history.redo() && document.size() == 2,
                    "mixed extrusion commits and undoes atomically");

    PointExtrudeTool persistentOptionsTool;
    persistentOptionsTool.begin(context);
    passed &= check(!persistentOptionsTool.weldEnabled() &&
                        persistentOptionsTool.manifoldEnabled(),
                    "Extrude must default to connected geometry without a division ring");
    persistentOptionsTool.toggleManifold(context);
    persistentOptionsTool.begin(context);
    passed &= check(!persistentOptionsTool.weldEnabled() &&
                        !persistentOptionsTool.manifoldEnabled(),
                    "division mode must persist when the tool starts again");
    return passed ? 0 : 1;
}
