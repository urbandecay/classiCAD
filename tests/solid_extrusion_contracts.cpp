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
    passed &= check(document.size() == 4 && history.undoCount() == 1 &&
                        history.undo() && document.size() == 2 &&
                        history.redo() && document.size() == 4,
                    "mixed extrusion commits and undoes atomically");
    return passed ? 0 : 1;
}
