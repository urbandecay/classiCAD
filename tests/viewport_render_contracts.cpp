#include "ui/viewport/line_type_style.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_line_weight.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_shading.h"
#include "ui/viewport/viewport_scene_renderer.h"
#include "ui/viewport/viewport_surface_renderer.h"
#include "ui/viewport/viewport_aa_renderer.h"
#include "ui/viewport/viewport_workbench_scene_pass_coordinator.h"
#include "ui/viewport/workbench_lighting.h"
#include "ui/viewport/blender_grid_renderer.h"

#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"
#include "core/geometry/scalar_volume.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/viewport/viewport_transform.h"

#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <utility>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

} // namespace

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication application(argc, argv);
    bool passed = true;

    const qreal defaultLineWidth = viewportLineWeightPixels(0.0);
    const qreal lightLineWidth = viewportLineWeightPixels(0.13);
    const qreal mediumLineWidth = viewportLineWeightPixels(0.25);
    const qreal heavyLineWidth = viewportLineWeightPixels(1.0);
    const qreal maxLineWidth = viewportLineWeightPixels(2.11);
    passed &= check(std::abs(defaultLineWidth - 1.0) < 1.0e-9 &&
                        std::abs(lightLineWidth - 1.0) < 1.0e-9 &&
                        std::abs(mediumLineWidth - 1.5) < 1.0e-9 &&
                        std::abs(heavyLineWidth - 6.0) < 1.0e-9 &&
                        std::abs(maxLineWidth - 12.66) < 1.0e-9 &&
                        defaultLineWidth == lightLineWidth &&
                        lightLineWidth < mediumLineWidth &&
                        mediumLineWidth < heavyLineWidth &&
                        heavyLineWidth < maxLineWidth,
                    "default and 0.13 mm must match Blender's thin outline while heavier line weights grow predictably");

    ViewportRenderObject splineStyleObject;
    splineStyleObject.shape.geometryType = GeometryType::Line;
    splineStyleObject.layerLineWeightMm = 0.25;
    ViewportSceneStroke splineStyle;
    const bool splineStyleReady = makeViewportSceneStrokes(
        splineStyleObject, true, &splineStyle);
    ViewportRenderObject surfaceStyleObject;
    surfaceStyleObject.shape.geometryType = GeometryType::NurbsSurface;
    surfaceStyleObject.layerLineWeightMm = splineStyleObject.layerLineWeightMm;
    auto boundaryGeometry = QSharedPointer<ViewportDepthGeometry>::create();
    boundaryGeometry->lineVertices << QVector3D(0.0f, 0.0f, 0.0f)
                                   << QVector3D(1.0f, 0.0f, 0.0f);
    ViewportSceneStroke surfaceOutlineStyle;
    const bool surfaceOutlineReady = makeViewportSurfaceOutlineStroke(
        surfaceStyleObject, true, QColor(Qt::black), boundaryGeometry,
        &surfaceOutlineStyle);
    passed &= check(splineStyleReady && surfaceOutlineReady &&
                        std::abs(splineStyle.width -
                                 surfaceOutlineStyle.width) < 1.0e-6f &&
                        !surfaceOutlineStyle.editModeWire,
                    "surface perimeters must use the same pixel width and smooth overlay path as spline strokes");

    ViewportTransform surfaceOutlineCamera;
    surfaceOutlineCamera.setViewDirection({0.0, 0.0, -1.0});
    ViewportDepthGeometry planarSurfaceMesh;
    planarSurfaceMesh.surfaceVertices
        << QVector3D(0.0f, 0.0f, 0.0f)
        << QVector3D(1.0f, 0.0f, 0.0f)
        << QVector3D(0.0f, 1.0f, 0.0f)
        << QVector3D(1.0f, 0.0f, 0.0f)
        << QVector3D(1.0f, 1.0f, 0.0f)
        << QVector3D(0.0f, 1.0f, 0.0f);
    const auto planarOutline = makeViewportSurfaceOutlineGeometry(
        planarSurfaceMesh, surfaceOutlineCamera);
    passed &= check(planarOutline &&
                        planarOutline->lineVertices.size() == 8,
                    "a planar two-triangle surface outline must include its four boundary edges and omit the hidden triangulation diagonal");

    ViewportDepthGeometry subdividedSurfaceMesh;
    constexpr int outlineSubdivisions = 16;
    for (int u = 0; u < outlineSubdivisions; ++u) {
        for (int v = 0; v < outlineSubdivisions; ++v) {
            const float x0 = float(u) / outlineSubdivisions;
            const float x1 = float(u + 1) / outlineSubdivisions;
            const float y0 = float(v) / outlineSubdivisions;
            const float y1 = float(v + 1) / outlineSubdivisions;
            subdividedSurfaceMesh.surfaceVertices
                << QVector3D(x0, y0, 0.0f)
                << QVector3D(x1, y0, 0.0f)
                << QVector3D(x0, y1, 0.0f)
                << QVector3D(x1, y0, 0.0f)
                << QVector3D(x1, y1, 0.0f)
                << QVector3D(x0, y1, 0.0f);
        }
    }
    const auto subdividedOutline = makeViewportSurfaceOutlineGeometry(
        subdividedSurfaceMesh, surfaceOutlineCamera);
    passed &= check(subdividedOutline &&
                        subdividedOutline->lineVertices.size() == 8,
                    "a subdivided planar surface must merge each straight boundary into one stroke without overlapping internal end caps");

    ViewportDepthGeometry foldedSurfaceMesh;
    foldedSurfaceMesh.surfaceVertices
        << QVector3D(0.0f, 0.0f, 0.0f)
        << QVector3D(1.0f, 0.0f, 0.0f)
        << QVector3D(0.0f, 1.0f, 0.0f)
        << QVector3D(1.0f, 0.0f, 0.0f)
        << QVector3D(0.0f, 1.0f, 0.0f)
        << QVector3D(1.0f, 1.0f, 0.0f);
    const auto foldedOutline = makeViewportSurfaceOutlineGeometry(
        foldedSurfaceMesh, surfaceOutlineCamera);
    passed &= check(foldedOutline &&
                        foldedOutline->lineVertices.size() == 10,
                    "a folded surface outline must retain its view-dependent silhouette edge");

    const QPixmap xrayIcon(QStringLiteral(":/blender-shading/xray.png"));
    const QPixmap wireframeIcon(
        QStringLiteral(":/blender-shading/shading_wire.png"));
    const QPixmap solidIcon(
        QStringLiteral(":/blender-shading/shading_solid.png"));
    passed &= check(!xrayIcon.isNull() && !wireframeIcon.isNull() &&
                        !solidIcon.isNull(),
                    "viewport shading controls must load Blender's embedded X-Ray, Wireframe, and Solid icons");

    const WorkbenchStudioLighting &workbenchLighting =
        defaultWorkbenchStudioLighting();
    const QVector3D defaultMaterialColor =
        workbenchDefaultSolidMaterialDiffuseColor();
    const QVector3D litDefaultSurface = workbenchStudioShade(
        QVector3D(0.5f, 0.5f, 0.5f),
        QVector3D(0.0f, 0.0f, 1.0f),
        QVector3D(0.0f, 0.0f, 1.0f));
    const QVector3D roundTripColor = workbenchSceneLinearToSrgb(
        workbenchSrgbToSceneLinear(QVector3D(0.2f, 0.5f, 0.8f)));
    const QVector3D defaultCubeFace = workbenchStudioShade(
        defaultMaterialColor, QVector3D(0.0f, 0.0f, 1.0f),
        QVector3D(0.0f, 0.0f, 1.0f));
    const QVector3D defaultCubeFaceAgx =
        workbenchSceneLinearToAgxSrgb(defaultCubeFace);
    const QVector3D blenderDefaultCubeFaceSrgb(
        154.0f / 255.0f, 155.0f / 255.0f, 156.0f / 255.0f);
    passed &= check(workbenchLighting.useSpecular &&
                        workbenchLighting.lights[0].enabled &&
                        workbenchLighting.lights[3].enabled &&
                        std::abs(defaultMaterialColor.x() - 0.8f) < 1.0e-7f &&
                        std::abs(defaultMaterialColor.y() - 0.8f) < 1.0e-7f &&
                        std::abs(defaultMaterialColor.z() - 0.8f) < 1.0e-7f &&
                        std::abs(workbenchLighting.lights[1].diffuseColor.x() -
                                 0.521082997f) < 1.0e-7f &&
                        std::abs(litDefaultSurface.x() - 0.239758f) < 2.0e-3f &&
                        std::abs(litDefaultSurface.y() - 0.245706f) < 2.0e-3f &&
                        std::abs(litDefaultSurface.z() - 0.248113f) < 2.0e-3f &&
                        std::abs(roundTripColor.x() - 0.2f) < 1.0e-5f &&
                        std::abs(roundTripColor.y() - 0.5f) < 1.0e-5f &&
                        std::abs(roundTripColor.z() - 0.8f) < 1.0e-5f &&
                        workbenchAgxDisplayLutData().size() ==
                            workbenchAgxDisplayLutSize *
                                workbenchAgxDisplayLutSize *
                                workbenchAgxDisplayLutSize * 3 &&
                        (defaultCubeFaceAgx - blenderDefaultCubeFaceSrgb)
                                .length() < 0.01f,
                    "default solid cube face must use Blender's Workbench lighting and AgX display transform");

    // Imported scene cameras use Blender's render viewplane, independently
    // of the interactive viewport's navigation projection.
    const QSize cameraRenderSize(640, 480);
    ViewportTransform renderCameraTransform;
    renderCameraTransform.setPerspectiveEnabled(true);
    ViewportCameraPreferences renderCameraPreferences;
    renderCameraPreferences.focalLengthMillimeters = 50.0;
    renderCameraPreferences.clipStart = 0.2;
    renderCameraPreferences.clipEnd = 200.0;
    renderCameraTransform.setCameraPreferences(renderCameraPreferences);
    ViewportRenderCameraProjection renderCameraProjection;
    renderCameraProjection.sensorWidthMillimeters = 40.0;
    renderCameraProjection.sensorHeightMillimeters = 20.0;
    renderCameraProjection.sensorFit = ViewportCameraSensorFit::Horizontal;
    renderCameraTransform.setRenderCameraProjection(renderCameraProjection);
    const Point3D cameraTarget = renderCameraTransform.viewTarget();
    const Point3D cameraOutward = renderCameraTransform.viewDirection();
    const Point3D cameraUp = renderCameraTransform.viewUp();
    const Point3D centerInFrontOfCamera{
        cameraTarget.x - cameraOutward.x * 10.0,
        cameraTarget.y - cameraOutward.y * 10.0,
        cameraTarget.z - cameraOutward.z * 10.0};
    const auto cameraNdc = [&](const ViewportTransform &transform,
                               const Point3D &point) {
        const QVector4D clip = viewportViewProjection(
            transform, cameraRenderSize) *
            QVector4D(float(point.x), float(point.y), float(point.z), 1.0f);
        return std::abs(clip.w()) > 1.0e-7f
            ? QPointF(clip.x() / clip.w(), clip.y() / clip.w())
            : QPointF(std::numeric_limits<qreal>::quiet_NaN(),
                      std::numeric_limits<qreal>::quiet_NaN());
    };
    const QPointF centeredCameraNdc = cameraNdc(
        renderCameraTransform, centerInFrontOfCamera);
    ViewportTransform shiftedCameraTransform = renderCameraTransform;
    renderCameraProjection.shiftX = 0.1;
    renderCameraProjection.shiftY = 0.05;
    shiftedCameraTransform.setRenderCameraProjection(renderCameraProjection);
    const QPointF shiftedCameraNdc = cameraNdc(
        shiftedCameraTransform, centerInFrontOfCamera);
    const ViewportProjectionClipRange cameraClipRange =
        viewportProjectionClipRange(renderCameraTransform, cameraRenderSize);
    ViewportTransform anisotropicCameraTransform = renderCameraTransform;
    renderCameraProjection.shiftX = 0.0;
    renderCameraProjection.shiftY = 0.0;
    renderCameraProjection.pixelAspectY = 2.0;
    anisotropicCameraTransform.setRenderCameraProjection(
        renderCameraProjection);
    const Point3D elevatedCameraPoint{
        centerInFrontOfCamera.x + cameraUp.x,
        centerInFrontOfCamera.y + cameraUp.y,
        centerInFrontOfCamera.z + cameraUp.z};
    const QPointF squarePixelElevatedNdc = cameraNdc(
        renderCameraTransform, elevatedCameraPoint);
    const QPointF anisotropicElevatedNdc = cameraNdc(
        anisotropicCameraTransform, elevatedCameraPoint);
    passed &= check(
        std::abs(centeredCameraNdc.x()) < 1.0e-4 &&
            std::abs(centeredCameraNdc.y()) < 1.0e-4 &&
            std::abs((shiftedCameraNdc.x() - centeredCameraNdc.x()) + 0.2) <
                1.0e-3 &&
            std::abs((shiftedCameraNdc.y() - centeredCameraNdc.y()) +
                     (2.0 * 0.05 * cameraRenderSize.width() /
                      cameraRenderSize.height())) < 1.0e-3 &&
            std::abs(cameraClipRange.nearPlane - 0.2) < 1.0e-9 &&
            std::abs(cameraClipRange.farPlane - 200.0) < 1.0e-9 &&
            std::abs(anisotropicElevatedNdc.x() -
                     squarePixelElevatedNdc.x()) < 1.0e-4 &&
            std::abs(anisotropicElevatedNdc.y() -
                     squarePixelElevatedNdc.y() * 0.5) < 1.0e-3,
        "render camera projection must match Blender shifts, pixel aspect, and depth clip range");

    const WorkbenchStudioLighting basicStudioLight =
        workbenchStudioLightingPreset(QStringLiteral("Basic"));
    const QImage basicGreyDiffuse =
        workbenchMatcapDiffuseImage(QStringLiteral("basic_grey"));
    const QImage basicGreySpecular =
        workbenchMatcapSpecularImage(QStringLiteral("basic_grey"));
    const QImage bronzeDiffuse =
        workbenchMatcapDiffuseImage(QStringLiteral("metal_bronze"));
    const QImage bronzeSpecular =
        workbenchMatcapSpecularImage(QStringLiteral("metal_bronze"));
    const QStringList matcapPresets = workbenchMatcapPresets();
    const bool allMatcapLayersLoaded = std::all_of(
        matcapPresets.cbegin(), matcapPresets.cend(),
        [](const QString &preset) {
            return !workbenchMatcapDiffuseImage(preset).isNull() &&
                   !workbenchMatcapSpecularImage(preset).isNull();
        });
    const WorkbenchStudioLighting cameraFollowingRotatedLighting =
        workbenchStudioLightingForView(
            workbenchLighting, 90, false,
            QVector3D(1.0f, 0.0f, 0.0f),
            QVector3D(0.0f, 1.0f, 0.0f),
            QVector3D(0.0f, 0.0f, 1.0f));
    const WorkbenchStudioLighting worldRotatedLighting =
        workbenchStudioLightingForView(
            workbenchLighting, 90, true,
            QVector3D(1.0f, 0.0f, 0.0f),
            QVector3D(0.0f, 1.0f, 0.0f),
            QVector3D(0.0f, 0.0f, 1.0f));
    passed &= check(basicStudioLight.lights[0].enabled &&
                        std::abs(basicStudioLight.lights[0].wrap - 0.1f) < 1.0e-6f &&
                        !basicGreyDiffuse.isNull() &&
                        !basicGreySpecular.isNull() &&
                        allMatcapLayersLoaded &&
                        basicGreyDiffuse.pixelColor(256, 256).red() > 100 &&
                        bronzeDiffuse.pixelColor(256, 256).red() == 0 &&
                        bronzeSpecular.pixelColor(256, 256).red() > 100 &&
                        workbenchMatcapShade(
                            QStringLiteral("metal_bronze"),
                            QVector3D(0.8f, 0.8f, 0.8f),
                            QVector3D(0.0f, 0.0f, 1.0f),
                            QVector3D(0.0f, 0.0f, 1.0f)).x() > 0.2f &&
                        std::abs(cameraFollowingRotatedLighting.lights[1].direction.x() -
                                 workbenchLighting.lights[1].direction.x()) < 1.0e-6f &&
                        std::abs(worldRotatedLighting.lights[1].direction.x() -
                                 workbenchLighting.lights[1].direction.x()) > 0.1f,
                    "Workbench Studio Light presets and diffuse/specular MatCap layers must load, with rotation applied only to world-space lighting");

    ViewportShadingSettings shading;
    passed &= check(shading.mode == ViewportShadingMode::Solid &&
                        !shading.xrayEnabled() &&
                        shading.lightingMode == ViewportLightingMode::Studio &&
                        shading.studioLightPreset == QStringLiteral("Default") &&
                        !shading.worldSpaceLighting &&
                        shading.studioLightRotationDegrees == 0 &&
                        shading.colorMode == ViewportColorMode::Material &&
                        shading.backgroundMode == ViewportBackgroundMode::Theme &&
                        !shading.backfaceCulling && shading.outline &&
                        shading.specularLighting && !shading.shadows &&
                        !shading.depthOfField && !shading.cavity &&
                        std::abs(shading.xrayAlpha - 0.5) < 1.0e-9 &&
                        std::abs(shading.shadowIntensity - 0.5) < 1.0e-9,
                    "viewport must preserve the solid initial mode and independent X-Ray defaults");
    shading.toggleXray();
    passed &= check(shading.xrayEnabled() && shading.xray &&
                        shading.xrayWireframe,
                    "solid X-Ray toggle must update only its own setting");
    shading.mode = ViewportShadingMode::Wireframe;
    passed &= check(shading.xrayEnabled(),
                    "wireframe mode must restore its independent X-Ray setting");
    shading.toggleXray();
    passed &= check(!shading.xrayEnabled() && shading.xray &&
                        !shading.xrayWireframe,
                    "wireframe X-Ray toggle must preserve solid X-Ray state");
    shading.mode = ViewportShadingMode::Solid;
    passed &= check(shading.xrayEnabled() && shading.xray &&
                        !shading.xrayWireframe,
                    "solid mode must restore its own X-Ray state");
    shading.xrayAlpha = 1.0;
    passed &= check(!shading.xrayEnabled(),
                    "solid X-Ray at full opacity must resolve as normal depth-tested drawing");
    shading.xrayAlpha = 0.5;
    shading.toggleXray();
    passed &= check(!shading.xrayEnabled() && !shading.xray &&
                        !shading.xrayWireframe,
                    "solid X-Ray toggle must preserve wireframe X-Ray state");
    shading.mode = ViewportShadingMode::Wireframe;
    passed &= check(!shading.xrayEnabled(),
                    "returning to wireframe must restore its X-Ray state");

    ViewportRenderFrame aaHistoryFrame;
    aaHistoryFrame.camera = ViewportTransform{};
    aaHistoryFrame.viewportSize = QSize(160, 160);
    ViewportRenderObject aaMeshObject;
    aaMeshObject.objectId = ObjectId::fromValue(101);
    aaMeshObject.shape.geometryType = GeometryType::PolygonMesh;
    aaMeshObject.geometryRevision = 1;
    aaMeshObject.meshMaterials.append(WorkbenchMaterialData{});
    aaHistoryFrame.objects.append(aaMeshObject);
    ViewportShadingSettings aaHistoryShading;
    aaHistoryShading.mode = ViewportShadingMode::Solid;
    aaHistoryShading.colorMode = ViewportColorMode::Texture;
    const auto aaKey = [](const ViewportRenderFrame &frame,
                          const ViewportShadingSettings &settings,
                          std::optional<QVector3D> worldColor = std::nullopt) {
        return viewportAaHistoryKey(frame, settings, 8, 1.0, {}, worldColor);
    };
    const QByteArray aaBaselineKey = aaKey(aaHistoryFrame, aaHistoryShading);
    ViewportRenderFrame changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects[0].geometryRevision++;
    const bool meshGeometryInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects[0].selected = true;
    const bool meshSelectionInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects[0].showInFront = true;
    const bool meshDepthModeInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects[0].meshMaterials[0]
        .baseColorSceneLinear = QVector3D(0.1f, 0.2f, 0.3f);
    const bool meshMaterialInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects[0].displayColorSceneLinear =
        QVector4D(0.3f, 0.2f, 0.1f, 1.0f);
    const bool meshColorInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects[0].meshMaterials[0]
        .textureImagePackedDataBase64 = QStringLiteral("changed packed image");
    const bool meshTextureInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    changedMeshHistoryFrame = aaHistoryFrame;
    changedMeshHistoryFrame.objects.append(aaMeshObject);
    const bool meshAddRemoveInvalidatesAa =
        aaKey(changedMeshHistoryFrame, aaHistoryShading) != aaBaselineKey;
    ViewportRenderFrame aaSurfaceHistoryFrame = aaHistoryFrame;
    aaSurfaceHistoryFrame.objects[0].shape.geometryType =
        GeometryType::NurbsSurface;
    aaSurfaceHistoryFrame.objects[0].workbenchMaterial
        .textureImagePath = QStringLiteral("texture-a.png");
    const QByteArray aaSurfaceTextureKey =
        aaKey(aaSurfaceHistoryFrame, aaHistoryShading);
    aaSurfaceHistoryFrame.objects[0].workbenchMaterial
        .textureImagePath = QStringLiteral("texture-b.png");
    const bool nurbsTextureInvalidatesAa =
        aaKey(aaSurfaceHistoryFrame, aaHistoryShading) != aaSurfaceTextureKey;
    const bool worldBackgroundInvalidatesAa =
        aaKey(aaHistoryFrame, aaHistoryShading, QVector3D(0.1f, 0.2f, 0.3f)) !=
        aaBaselineKey;
    passed &= check(meshGeometryInvalidatesAa &&
                        meshSelectionInvalidatesAa &&
                        meshDepthModeInvalidatesAa &&
                        meshMaterialInvalidatesAa &&
                        meshColorInvalidatesAa &&
                        meshTextureInvalidatesAa &&
                        meshAddRemoveInvalidatesAa &&
                        nurbsTextureInvalidatesAa &&
                        worldBackgroundInvalidatesAa,
                    "temporal AA history keys must change for mesh edits, selection, visibility mode, materials, textures, scene membership, and world color");

    NurbsSurface3D fillSurface;
    fillSurface.degreeU = 1;
    fillSurface.degreeV = 1;
    fillSurface.orderU = 2;
    fillSurface.orderV = 2;
    fillSurface.controlVertexCountU = 2;
    fillSurface.controlVertexCountV = 2;
    fillSurface.controlPoints = {{-1.0, -1.0, 0.0},
                                 {-1.0, 1.0, 0.0},
                                 {1.0, -1.0, 0.0},
                                 {1.0, 1.0, 0.0}};
    fillSurface.weights = {1.0, 1.0, 1.0, 1.0};
    fillSurface.knotsU = {0.0, 1.0};
    fillSurface.knotsV = {0.0, 1.0};
    Shape surfaceShape;
    surfaceShape.geometryType = GeometryType::NurbsSurface;
    surfaceShape.nurbsSurface = fillSurface;
    NurbsSurface3D cubeBaseSurface = fillSurface;
    for (Point3D &point : cubeBaseSurface.controlPoints) {
        point.z = -1.0;
    }
    Shape cubeShape;
    cubeShape.geometryType = GeometryType::NurbsSolid;
    passed &= check(makeNurbsExtrusionSolid(cubeBaseSurface, {0.0, 0.0, 2.0},
                                            &cubeShape.nurbsSolid),
                    "cube display comparison must create an exact NURBS extrusion solid");
    const ViewportDepthGeometry cubeDepthGeometry =
        buildViewportDepthGeometry(cubeShape);
    passed &= check(cubeDepthGeometry.surfaceNormals.size() >= 12 &&
                        cubeDepthGeometry.surfaceNormals[0].z() < -0.99f &&
                        cubeDepthGeometry.surfaceNormals[
                            cubeDepthGeometry.surfaceNormals.size() / 2]
                                .z() > 0.99f,
                    "NURBS extrusion cube cap normals must face outward on both ends");
    ViewportTransform surfaceTransform;
    surfaceTransform.zoom() = 32.0;
    surfaceTransform.setViewPreset(ViewportViewPreset::Top);
    CurveHitTester surfaceHitTester;
    ViewportRenderer surfaceRenderer(surfaceTransform, surfaceHitTester);
    const QSize surfaceViewportSize(160, 160);
    struct RenderedSurface {
        int coveredPixels = 0;
        QColor centerColor;
    };
    const auto renderedSurface = [&](const ViewportShadingSettings &settings,
                                     bool selected = false) {
        QImage image(surfaceViewportSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surfaceRenderer.setShadingSettings(settings);
        QPainter painter(&image);
        surfaceRenderer.drawShape(painter, surfaceShape, surfaceViewportSize,
                                 false, selected);
        painter.end();
        int coveredPixels = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                coveredPixels += image.pixelColor(x, y).alpha() > 0;
            }
        }
        return RenderedSurface{coveredPixels, image.pixelColor(80, 80)};
    };
    const RenderedSurface wireframeSurface = renderedSurface(shading);
    ViewportShadingSettings solidShading;
    solidShading.mode = ViewportShadingMode::Solid;
    const RenderedSurface solidSurface = renderedSurface(solidShading);
    const RenderedSurface selectedSolidSurface = renderedSurface(solidShading,
                                                                  true);
    ViewportShadingSettings matcapShading = solidShading;
    matcapShading.lightingMode = ViewportLightingMode::MatCap;
    matcapShading.matcapPreset = QStringLiteral("metal_bronze");
    const RenderedSurface matcapSurface = renderedSurface(matcapShading);
    const QVector3D surfaceBaseLinear =
        workbenchDefaultSolidMaterialDiffuseColor();
    const QVector3D expectedCpuColor = workbenchSceneLinearToAgxSrgb(
        workbenchStudioShade(surfaceBaseLinear,
                             QVector3D(0.0f, 0.0f, 1.0f),
                             QVector3D(0.0f, 0.0f, 1.0f)));
    const QVector3D expectedMatcapColor = workbenchSceneLinearToAgxSrgb(
        workbenchMatcapShade(QStringLiteral("metal_bronze"),
                             surfaceBaseLinear,
                             QVector3D(0.0f, 0.0f, 1.0f),
                             QVector3D(0.0f, 0.0f, 1.0f)));
    passed &= check(validateNurbsSurface(fillSurface) &&
                        wireframeSurface.coveredPixels > 0 &&
                        solidSurface.coveredPixels >
                            wireframeSurface.coveredPixels * 3 &&
                        std::abs(solidSurface.centerColor.redF() -
                                 expectedCpuColor.x()) < 0.025f &&
                        std::abs(solidSurface.centerColor.greenF() -
                                 expectedCpuColor.y()) < 0.025f &&
                        std::abs(solidSurface.centerColor.blueF() -
                                 expectedCpuColor.z()) < 0.025f &&
                        std::abs(selectedSolidSurface.centerColor.redF() -
                                 expectedCpuColor.x()) < 0.025f &&
                        std::abs(selectedSolidSurface.centerColor.greenF() -
                                 expectedCpuColor.y()) < 0.025f &&
                        std::abs(selectedSolidSurface.centerColor.blueF() -
                                 expectedCpuColor.z()) < 0.025f &&
                        matcapSurface.coveredPixels > wireframeSurface.coveredPixels * 3 &&
                        std::abs(matcapSurface.centerColor.redF() -
                                 expectedMatcapColor.x()) < 0.025f &&
                        std::abs(matcapSurface.centerColor.greenF() -
                                 expectedMatcapColor.y()) < 0.025f &&
                        std::abs(matcapSurface.centerColor.blueF() -
                                 expectedMatcapColor.z()) < 0.025f,
                    "solid viewport shading must render NURBS faces with Workbench Studio and two-layer MatCap colors");

    // Color-mode source contracts for the software renderer. The attribute
    // triangle has three deliberately distinct corner colors; the texture
    // sample is red while its material tint is green, so multiplying the two
    // produces a visibly different (black) result from Blender's override.
    Shape colorSourceMesh;
    colorSourceMesh.geometryType = GeometryType::PolygonMesh;
    colorSourceMesh.polygonMesh.vertices = {{-1.0, -1.0, 0.0},
                                            {1.0, -1.0, 0.0},
                                            {0.0, 1.0, 0.0}};
    colorSourceMesh.polygonMesh.faces = {{0, 1, 2}};
    colorSourceMesh.polygonMesh.faceMaterialIndices = {0};
    MeshColorAttribute cornerColors;
    cornerColors.name = QStringLiteral("Color");
    cornerColors.domain = MeshAttributeDomain::Point;
    cornerColors.colorSpace = QStringLiteral("scene_linear");
    cornerColors.componentCount = 4;
    cornerColors.values = {0.9f, 0.05f, 0.05f, 0.1f,
                           0.05f, 0.9f, 0.05f, 0.2f,
                           0.05f, 0.05f, 0.9f, 0.3f};
    colorSourceMesh.polygonMesh.colorAttributes = {cornerColors};
    colorSourceMesh.polygonMesh.activeColorAttributeName = cornerColors.name;

    ViewportShadingSettings attributeSettings;
    attributeSettings.mode = ViewportShadingMode::Solid;
    attributeSettings.lightingMode = ViewportLightingMode::Flat;
    attributeSettings.colorMode = ViewportColorMode::Attribute;
    attributeSettings.outline = false;
    attributeSettings.specularLighting = false;
    const auto renderColorMesh = [&](const ViewportShadingSettings &settings,
                                     const QVector<WorkbenchMaterialData> &materials) {
        QImage image(surfaceViewportSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surfaceRenderer.setShadingSettings(settings);
        QPainter painter(&image);
        surfaceRenderer.drawShape(
            painter, colorSourceMesh, surfaceViewportSize, false, false, true,
            QColor(), QString(), 0.0, ObjectId::invalid(), 0, nullptr, {},
            std::nullopt, std::nullopt, QString(), QString(),
            identityMeshObjectTransform, materials);
        painter.end();
        return image;
    };
    const QImage attributeImage = renderColorMesh(attributeSettings, {});
    QPointF nearRedCorner;
    QPointF nearGreenCorner;
    const bool projectedColorSamples =
        surfaceTransform.worldPointToScreen({-0.8, -0.8, 0.0},
                                            surfaceViewportSize,
                                            &nearRedCorner) &&
        surfaceTransform.worldPointToScreen({0.8, -0.8, 0.0},
                                            surfaceViewportSize,
                                            &nearGreenCorner);
    const QColor nearRedAttribute = attributeImage.pixelColor(
        qRound(nearRedCorner.x()), qRound(nearRedCorner.y()));
    const QColor nearGreenAttribute = attributeImage.pixelColor(
        qRound(nearGreenCorner.x()), qRound(nearGreenCorner.y()));
    passed &= check(projectedColorSamples &&
                        nearRedAttribute.red() > nearRedAttribute.green() + 20 &&
                        nearGreenAttribute.green() > nearGreenAttribute.red() + 20 &&
                        nearRedAttribute.alpha() > 240 &&
                        nearGreenAttribute.alpha() > 240,
                    "QPainter Attribute mode must smoothly interpolate corner RGB while retaining material alpha");

    QTemporaryDir colorTextureDirectory;
    QImage sourceTexture(1, 1, QImage::Format_RGBA8888);
    sourceTexture.fill(QColor(220, 30, 20, 255));
    const QString colorTexturePath =
        colorTextureDirectory.filePath(QStringLiteral("source.png"));
    const bool savedColorTexture = sourceTexture.save(colorTexturePath);
    colorSourceMesh.polygonMesh.uvMaps = {
        {QStringLiteral("UVMap"), {QPointF(0.5, 0.5), QPointF(0.5, 0.5),
                                   QPointF(0.5, 0.5)}}};
    colorSourceMesh.polygonMesh.activeUvMapName = QStringLiteral("UVMap");
    WorkbenchMaterialData greenTextureTint;
    greenTextureTint.baseColorSceneLinear = QVector3D(0.02f, 0.9f, 0.02f);
    greenTextureTint.alpha = 0.7;
    greenTextureTint.textureImagePath = colorTexturePath;
    greenTextureTint.textureImageInterpolation = QStringLiteral("Closest");
    ViewportShadingSettings textureSettings = attributeSettings;
    textureSettings.colorMode = ViewportColorMode::Texture;
    const QImage textureImage = renderColorMesh(
        textureSettings, {greenTextureTint});
    const QColor sampledTexture = textureImage.pixelColor(80, 80);
    passed &= check(savedColorTexture && colorTextureDirectory.isValid() &&
                        sampledTexture.red() > sampledTexture.green() + 35 &&
                        std::abs(sampledTexture.alpha() - 179) <= 2,
                    "QPainter Texture mode must replace material RGB with the image sample and retain material alpha");

    QOpenGLContext context;
    context.setFormat(format);
    passed &= check(context.create(),
                    "NURBS solid display test must create its OpenGL context");
    if (context.isValid()) {
        QOffscreenSurface glSurface;
        glSurface.setFormat(context.format());
        glSurface.create();
        const bool surfaceCurrent = glSurface.isValid() &&
                                    context.makeCurrent(&glSurface);
        passed &= check(surfaceCurrent,
                        "NURBS solid display test must activate an offscreen surface");
        if (surfaceCurrent) {
            QOpenGLFunctions_3_3_Core functions;
            passed &= check(functions.initializeOpenGLFunctions(),
                            "NURBS solid display test must initialize OpenGL 3.3");
            QImage gpuImage;
            QImage gpuWithoutOutlineImage;
            QImage gpuUnselectedNoOutlineImage;
            QImage gpuCavityImage;
            QImage gpuCavityBaselineImage;
            QImage gpuSceneStrokeManagedOutlineImage;
            QImage gpuUnselectedOutlineImage;
            QImage gpuBottomImage;
            QImage gpuShadowImage;
            int gpuCavityChangedPixels = 0;
            int gpuShadowDarkenedPixels = 0;
            QImage gpuMatcapImage;
            bool gpuDrawSucceeded = false;
            bool gpuUnselectedCaptureNonblank = true;
            bool gpuMatcapDrawSucceeded = false;
            bool gpuBottomDrawSucceeded = false;
            bool gpuShadowDrawSucceeded = false;
            bool gpuCavityDrawSucceeded = false;
            bool gpuUnselectedBaselineDrawSucceeded = false;
            bool gpuSceneStrokeSuppressesPostOutline = false;
            bool gpuWireLinearBlendMatches = false;
            bool gpuVolumeDrawSucceeded = false;
            bool gpuVolumePassReported = false;
            int gpuVolumeChangedPixels = 0;
            bool gpuLowDensityVolumePreserved = false;
            bool gpuSignedVolumeScatteringPreserved = false;
            const QSize glSize(640, 480);
            ViewportTransform gpuTransform;
            gpuTransform.zoom() = 17.28;
            gpuTransform.setViewDirection({4.0, -6.0, 4.0});
            {
                QOpenGLFramebufferObjectFormat framebufferFormat;
                framebufferFormat.setAttachment(
                    QOpenGLFramebufferObject::CombinedDepthStencil);
                QOpenGLFramebufferObject framebuffer(glSize,
                                                     framebufferFormat);
                passed &= check(framebuffer.isValid(),
                                "NURBS solid display test must allocate a depth framebuffer");
                if (framebuffer.isValid()) {
                    ViewportRenderObject surfaceObject;
                    surfaceObject.shape = cubeShape;
                    surfaceObject.objectId = ObjectId::fromValue(1);
                    surfaceObject.selected = true;
                    surfaceObject.geometryRevision = 1;
                    surfaceObject.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(surfaceObject.shape));
                    ViewportShadingSettings gpuSolidShading;
                    gpuSolidShading.mode = ViewportShadingMode::Solid;
                    ViewportSurfaceRenderer gpuSurfaceRenderer;
                    framebuffer.bind();
                    functions.glViewport(0, 0, glSize.width(), glSize.height());
                    ViewportAaRenderer historyResetRenderer;
                    historyResetRenderer.setMode(static_cast<int>(
                        ViewportAntiAliasingMode::Samples5));
                    historyResetRenderer.setDisplayTransform(gpuSolidShading,
                                                             true);
                    bool aaHistoryResetContract =
                        historyResetRenderer.beginSceneSample(
                            glSize, 1.0, gpuTransform, aaBaselineKey,
                            false, true) &&
                        historyResetRenderer.targets().sampleIndex == 0;
                    const quint64 firstAaHistoryEpoch =
                        historyResetRenderer.targets().historyEpoch;
                    if (aaHistoryResetContract) {
                        historyResetRenderer.finishSceneSample();
                        aaHistoryResetContract =
                            historyResetRenderer.beginSceneSample(
                                glSize, 1.0, gpuTransform, aaBaselineKey,
                                false, true) &&
                            historyResetRenderer.targets().sampleIndex == 1;
                    }
                    if (aaHistoryResetContract) {
                        historyResetRenderer.finishSceneSample();
                        aaHistoryResetContract =
                            historyResetRenderer.beginSceneSample(
                                glSize, 1.0, gpuTransform,
                                aaKey(changedMeshHistoryFrame,
                                      aaHistoryShading),
                                false, true) &&
                            historyResetRenderer.targets().sampleIndex == 0 &&
                            historyResetRenderer.targets().historyEpoch >
                                firstAaHistoryEpoch;
                        if (historyResetRenderer.sceneSampleActive()) {
                            historyResetRenderer.finishSceneSample();
                        }
                    }
                    passed &= check(aaHistoryResetContract,
                                    "changing a visible mesh input must restart temporal AA accumulation at sample zero");
                    functions.glClearColor(34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, gpuTransform, glSize, 1.0,
                        gpuSolidShading);
                    functions.glFinish();
                    gpuImage = framebuffer.toImage();
                    const QString capturePath = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_CAPTURE");
                    if (!capturePath.isEmpty()) {
                        gpuImage.save(capturePath);
                    }

                    ViewportShadingSettings noOutline = gpuSolidShading;
                    noOutline.outline = false;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuSurfaceRenderer.draw({surfaceObject}, gpuTransform,
                                            glSize, 1.0, noOutline);
                    functions.glFinish();
                    gpuWithoutOutlineImage = framebuffer.toImage();

                    surfaceObject.outlineHandledBySceneStroke = true;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuSurfaceRenderer.draw({surfaceObject}, gpuTransform,
                                            glSize, 1.0, gpuSolidShading);
                    functions.glFinish();
                    gpuSceneStrokeManagedOutlineImage = framebuffer.toImage();
                    surfaceObject.outlineHandledBySceneStroke = false;

                    surfaceObject.selected = false;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuSurfaceRenderer.draw({surfaceObject}, gpuTransform,
                                            glSize, 1.0, gpuSolidShading);
                    functions.glFinish();
                    gpuUnselectedOutlineImage = framebuffer.toImage();
                    ViewportShadingSettings rawFaceShading = gpuSolidShading;
                    rawFaceShading.outline = false;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuUnselectedBaselineDrawSucceeded =
                        gpuSurfaceRenderer.draw(
                            {surfaceObject}, gpuTransform, glSize, 1.0,
                            rawFaceShading);
                    functions.glFinish();
                    gpuUnselectedNoOutlineImage = framebuffer.toImage();

                    ViewportShadingSettings shadowShading = rawFaceShading;
                    shadowShading.shadows = true;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuShadowDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, gpuTransform, glSize, 1.0,
                        shadowShading);
                    functions.glFinish();
                    gpuShadowImage = framebuffer.toImage();

                    const auto renderCoordinatedCavity = [&](
                        const ViewportShadingSettings &shading,
                        const QByteArray &historyKey, QImage *image) {
                        ViewportAaRenderer cavityAaRenderer;
                        cavityAaRenderer.setMode(static_cast<int>(
                            ViewportAntiAliasingMode::Off));
                        cavityAaRenderer.setDisplayTransform(shading, true);
                        if (!cavityAaRenderer.beginSceneSample(
                                glSize, 1.0, gpuTransform, historyKey,
                                false, true)) {
                            return false;
                        }
                        const float targetDisplayGray = 34.0f / 255.0f;
                        float low = 0.0f;
                        float high = 16.0f;
                        for (int iteration = 0; iteration < 32; ++iteration) {
                            const float middle = (low + high) * 0.5f;
                            const float displayGray =
                                workbenchSceneLinearToAgxSrgb(
                                    QVector3D(middle, middle, middle)).x();
                            if (displayGray < targetDisplayGray) low = middle;
                            else high = middle;
                        }
                        const GLfloat linearBackground[4] = {
                            (low + high) * 0.5f,
                            (low + high) * 0.5f,
                            (low + high) * 0.5f,
                            1.0f};
                        functions.glClearBufferfv(GL_COLOR, 0,
                                                  linearBackground);
                        ViewportWorkbenchSceneColorPassRequest request;
                        request.sharedTargets = &cavityAaRenderer.targets();
                        request.sceneLinearOutput = true;
                        const auto result =
                            ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                                gpuSurfaceRenderer, {surfaceObject},
                                cavityAaRenderer.sampleCamera(), glSize, 1.0,
                                shading, request);
                        if (result.rendered) {
                            cavityAaRenderer.markCurrentSampleAttachmentsValid(
                                result.report.populatedAttachments);
                        }
                        cavityAaRenderer.finishSceneSample();
                        if (!result.rendered) return false;
                        functions.glFinish();
                        *image = framebuffer.toImage();
                        return !image->isNull();
                    };
                    gpuCavityDrawSucceeded = renderCoordinatedCavity(
                        rawFaceShading, QByteArrayLiteral("cavity-baseline"),
                        &gpuCavityBaselineImage);
                    ViewportShadingSettings cavityShading = rawFaceShading;
                    cavityShading.cavity = true;
                    cavityShading.cavityType = ViewportCavityType::World;
                    gpuCavityDrawSucceeded = renderCoordinatedCavity(
                        cavityShading, QByteArrayLiteral("cavity-enabled"),
                        &gpuCavityImage) && gpuCavityDrawSucceeded;
                    const QString unselectedCapturePath = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_UNSELECTED_CAPTURE");
                    if (!unselectedCapturePath.isEmpty()) {
                        functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                        const bool rawFaceDrawSucceeded = gpuSurfaceRenderer.draw(
                            {surfaceObject}, gpuTransform, glSize, 1.0,
                            rawFaceShading);
                        functions.glFinish();
                        const QImage rawFaceImage = framebuffer.toImage();
                        int rawFaceChangedPixels = 0;
                        for (int y = 0; y < rawFaceImage.height(); ++y) {
                            for (int x = 0; x < rawFaceImage.width(); ++x) {
                                const QColor pixel = rawFaceImage.pixelColor(x, y);
                                rawFaceChangedPixels += pixel.red() != 34 ||
                                    pixel.green() != 34 || pixel.blue() != 34;
                            }
                        }
                        gpuUnselectedCaptureNonblank =
                            rawFaceDrawSucceeded && rawFaceChangedPixels > 4000;
                        passed &= check(
                            gpuUnselectedCaptureNonblank &&
                                rawFaceImage.save(unselectedCapturePath),
                            "unselected outline-free NURBS cube baseline must be nonblank and save");
                    }
                    const auto captureAaMode = [&](const char *environmentVariable,
                                                   int aaMode,
                                                   int expectedSamples,
                                                   const char *label) {
                        const QString capturePath =
                            qEnvironmentVariable(environmentVariable);
                        if (capturePath.isEmpty()) return true;

                        ViewportAaRenderer aaRenderer;
                        aaRenderer.setMode(aaMode);
                        aaRenderer.setDisplayTransform(gpuSolidShading, true);
                        int completedSamples = 0;
                        bool captureSucceeded = true;
                        for (int sample = 0; sample < expectedSamples; ++sample) {
                            const QByteArray historyKey =
                                QByteArray("viewport-aa-capture-") + label;
                            if (!aaRenderer.beginSceneSample(
                                    glSize, 1.0, gpuTransform, historyKey,
                                    false, true)) {
                                captureSucceeded = false;
                                break;
                            }
                            const QPointF renderJitter =
                                aaRenderer.sampleCamera().renderJitterPixels();
                            const bool jitterMatchesMode =
                                aaMode == static_cast<int>(
                                              ViewportAntiAliasingMode::Off)
                                    ? renderJitter.isNull()
                                    : !renderJitter.isNull();
                            if (!jitterMatchesMode) {
                                captureSucceeded = false;
                                aaRenderer.finishSceneSample();
                                break;
                            }
                            const float targetDisplayGray = 34.0f / 255.0f;
                            float low = 0.0f;
                            float high = 16.0f;
                            for (int iteration = 0; iteration < 32; ++iteration) {
                                const float middle = (low + high) * 0.5f;
                                const float displayGray =
                                    workbenchSceneLinearToAgxSrgb(
                                        QVector3D(middle, middle, middle)).x();
                                if (displayGray < targetDisplayGray) low = middle;
                                else high = middle;
                            }
                            const GLfloat linearBackground[4] = {
                                (low + high) * 0.5f,
                                (low + high) * 0.5f,
                                (low + high) * 0.5f,
                                1.0f};
                            functions.glClearBufferfv(GL_COLOR, 0,
                                                      linearBackground);
                            ViewportWorkbenchSceneColorPassRequest request;
                            request.sharedTargets = &aaRenderer.targets();
                            request.sceneLinearOutput = true;
                            const ViewportWorkbenchSceneColorPassResult result =
                                ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                                    gpuSurfaceRenderer, {surfaceObject},
                                    aaRenderer.sampleCamera(), glSize, 1.0,
                                    rawFaceShading, request);
                            if (!result.rendered) {
                                captureSucceeded = false;
                                aaRenderer.finishSceneSample();
                                break;
                            }
                            aaRenderer.markCurrentSampleAttachmentsValid(
                                result.report.populatedAttachments);
                            aaRenderer.finishSceneSample();
                            ++completedSamples;
                        }
                        const QImage image = framebuffer.toImage();
                        int changedPixels = 0;
                        for (int y = 0; y < image.height(); ++y) {
                            for (int x = 0; x < image.width(); ++x) {
                                const QColor pixel = image.pixelColor(x, y);
                                changedPixels += pixel.red() != 34 ||
                                    pixel.green() != 34 || pixel.blue() != 34;
                            }
                        }
                        return check(
                            captureSucceeded &&
                                completedSamples == expectedSamples &&
                                changedPixels > 4000 && image.save(capturePath),
                            label);
                    };
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_OFF_CAPTURE", 0, 1,
                        "AA Off baseline must draw one nonblank outline-free cube sample");
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_SINGLE_PASS_CAPTURE", 1, 1,
                        "Single Pass baseline must resolve one nonblank outline-free cube sample");
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_SAMPLES8_CAPTURE", 8, 8,
                        "8-sample baseline must resolve eight nonblank outline-free cube samples");
                    surfaceObject.selected = true;

                    ViewportTransform bottomTransform;
                    bottomTransform.zoom() = gpuTransform.zoom();
                    bottomTransform.setViewDirection({4.0, -6.0, -4.0});
                    ViewportShadingSettings bottomShading = gpuSolidShading;
                    bottomShading.backfaceCulling = true;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuBottomDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, bottomTransform, glSize, 1.0,
                        bottomShading);
                    functions.glFinish();
                    gpuBottomImage = framebuffer.toImage();

                    ViewportShadingSettings gpuMatcapShading = gpuSolidShading;
                    gpuMatcapShading.lightingMode = ViewportLightingMode::MatCap;
                    gpuMatcapShading.matcapPreset = QStringLiteral("metal_bronze");
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuMatcapDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, gpuTransform, glSize, 1.0,
                        gpuMatcapShading);
                    functions.glFinish();
                    gpuMatcapImage = framebuffer.toImage();

                    QVector<float> clearSceneDepth(glSize.width() *
                                                       glSize.height(),
                                                   1.0f);
                    GLuint sceneDepthTexture = 0;
                    functions.glGenTextures(1, &sceneDepthTexture);
                    functions.glBindTexture(GL_TEXTURE_2D,
                                            sceneDepthTexture);
                    functions.glTexParameteri(GL_TEXTURE_2D,
                                              GL_TEXTURE_MIN_FILTER,
                                              GL_NEAREST);
                    functions.glTexParameteri(GL_TEXTURE_2D,
                                              GL_TEXTURE_MAG_FILTER,
                                              GL_NEAREST);
                    functions.glTexParameteri(GL_TEXTURE_2D,
                                              GL_TEXTURE_WRAP_S,
                                              GL_CLAMP_TO_EDGE);
                    functions.glTexParameteri(GL_TEXTURE_2D,
                                              GL_TEXTURE_WRAP_T,
                                              GL_CLAMP_TO_EDGE);
                    functions.glTexImage2D(
                        GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                        glSize.width(), glSize.height(), 0,
                        GL_DEPTH_COMPONENT, GL_FLOAT,
                        clearSceneDepth.constData());

                    ScalarVolume3D scalarVolume;
                    scalarVolume.name = QStringLiteral("density");
                    scalarVolume.activeBounds = {-1, -1, -1, 0, 0, 0};
                    scalarVolume.expandedVoxelCount = 8;
                    scalarVolume.denseVoxelCount = 8;
                    scalarVolume.activeRecords.append(
                        {-1, -1, -1, 2, true, 1.0f});
                    scalarVolume.displayDensity = 32.0f;
                    scalarVolume.densityScale = 32.0f;
                    ViewportRenderObject volumeObject;
                    volumeObject.shape.geometryType =
                        GeometryType::ScalarVolume;
                    volumeObject.shape.scalarVolume = scalarVolume;
                    volumeObject.displayColorSceneLinear =
                        QVector4D(4.0f, 0.15f, 0.05f, 1.0f);
                    ViewportRenderTargets volumeTargets;
                    volumeTargets.depthTexture = sceneDepthTexture;
                    volumeTargets.pixelSize = glSize;
                    volumeTargets.validAttachments =
                        ViewportRenderTargets::Depth;
                    ViewportRenderPassReport volumePassReport;
                    ViewportShadingSettings volumeShading = gpuSolidShading;
                    volumeShading.colorMode = ViewportColorMode::Object;
                    functions.glBindTexture(GL_TEXTURE_2D, 0);
                    framebuffer.bind();
                    functions.glViewport(0, 0, glSize.width(),
                                         glSize.height());
                    functions.glClearColor(34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           34.0f / 255.0f, 1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT |
                                      GL_DEPTH_BUFFER_BIT);
                    gpuVolumeDrawSucceeded = gpuSurfaceRenderer.draw(
                        {volumeObject}, gpuTransform, glSize, 1.0,
                        volumeShading, false, true, false,
                        &volumeTargets, &volumePassReport);
                    functions.glFinish();
                    const QImage gpuVolumeImage = framebuffer.toImage();
                    for (int y = 0; y < gpuVolumeImage.height(); ++y) {
                        for (int x = 0; x < gpuVolumeImage.width(); ++x) {
                            const QColor pixel =
                                gpuVolumeImage.pixelColor(x, y);
                            gpuVolumeChangedPixels +=
                                pixel.red() != 34 || pixel.green() != 34 ||
                                pixel.blue() != 34;
                        }
                    }
                    gpuVolumePassReported = volumePassReport.passCompleted(
                        ViewportRenderPass::Volume);
                    ScalarVolume3D lowDensityVolume = scalarVolume;
                    lowDensityVolume.activeRecords[0].value = 1.0e-9f;
                    ViewportRenderObject lowDensityVolumeObject = volumeObject;
                    lowDensityVolumeObject.shape.scalarVolume = lowDensityVolume;
                    QOpenGLFramebufferObjectFormat floatFramebufferFormat;
                    floatFramebufferFormat.setAttachment(
                        QOpenGLFramebufferObject::CombinedDepthStencil);
                    floatFramebufferFormat.setInternalTextureFormat(GL_RGBA16F);
                    QOpenGLFramebufferObject floatFramebuffer(
                        glSize, floatFramebufferFormat);
                    if (floatFramebuffer.isValid()) {
                        floatFramebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        functions.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        const bool lowDensityDrawSucceeded =
                            gpuSurfaceRenderer.draw(
                                {lowDensityVolumeObject}, gpuTransform,
                                glSize, 1.0, volumeShading, false, true,
                                false, &volumeTargets, nullptr, true);
                        QVector<GLfloat> lowDensityPixels(
                            glSize.width() * glSize.height() * 4);
                        functions.glReadPixels(
                            0, 0, glSize.width(), glSize.height(), GL_RGBA,
                            GL_FLOAT, lowDensityPixels.data());
                        GLfloat maximumLowDensityAlpha = 0.0f;
                        for (qsizetype pixel = 0;
                             pixel < qsizetype(glSize.width()) *
                                         glSize.height();
                             ++pixel) {
                            maximumLowDensityAlpha = std::max(
                                maximumLowDensityAlpha,
                                lowDensityPixels[pixel * 4 + 3]);
                        }
                        gpuLowDensityVolumePreserved =
                            lowDensityDrawSucceeded &&
                            maximumLowDensityAlpha > 0.0f;
                        ScalarVolume3D negativeDensityVolume = scalarVolume;
                        negativeDensityVolume.activeRecords[0].value =
                            -1.0e-6f;
                        ViewportRenderObject negativeDensityVolumeObject =
                            volumeObject;
                        negativeDensityVolumeObject.shape.scalarVolume =
                            negativeDensityVolume;
                        functions.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        const bool negativeDensityDrawSucceeded =
                            gpuSurfaceRenderer.draw(
                                {negativeDensityVolumeObject}, gpuTransform,
                                glSize, 1.0, volumeShading, false, true,
                                false, &volumeTargets, nullptr, true);
                        functions.glReadPixels(
                            0, 0, glSize.width(), glSize.height(), GL_RGBA,
                            GL_FLOAT, lowDensityPixels.data());
                        GLfloat minimumNegativeDensityRed = 0.0f;
                        maximumLowDensityAlpha = 0.0f;
                        for (qsizetype pixel = 0;
                             pixel < qsizetype(glSize.width()) *
                                         glSize.height();
                             ++pixel) {
                            minimumNegativeDensityRed = std::min(
                                minimumNegativeDensityRed,
                                lowDensityPixels[pixel * 4]);
                            maximumLowDensityAlpha = std::max(
                                maximumLowDensityAlpha,
                                lowDensityPixels[pixel * 4 + 3]);
                        }
                        gpuSignedVolumeScatteringPreserved =
                            negativeDensityDrawSucceeded &&
                            minimumNegativeDensityRed < 0.0f &&
                            maximumLowDensityAlpha > 0.0f;
                        floatFramebuffer.release();
                    }
                    functions.glDeleteTextures(1, &sceneDepthTexture);
                    framebuffer.release();
                }
            }
            {
                QOpenGLFramebufferObjectFormat wireFramebufferFormat;
                wireFramebufferFormat.setAttachment(
                    QOpenGLFramebufferObject::CombinedDepthStencil);
                wireFramebufferFormat.setInternalTextureFormat(GL_SRGB8_ALPHA8);
                QOpenGLFramebufferObject wireFramebuffer(
                    glSize, wireFramebufferFormat);
                passed &= check(wireFramebuffer.isValid(),
                                "edit-wire color test must allocate an sRGB framebuffer");
                if (wireFramebuffer.isValid()) {
                    ViewportTransform wireTransform;
                    wireTransform.zoom() = 32.0;
                    wireTransform.setViewPreset(ViewportViewPreset::Top);
                    QPointF firstWorld;
                    QPointF secondWorld;
                    const bool linePointsValid =
                        wireTransform.screenToWorkPlane(
                            QPointF(80.0, 240.0), glSize, WorkPlane::XY, 0.0,
                            &firstWorld) &&
                        wireTransform.screenToWorkPlane(
                            QPointF(560.0, 240.0), glSize, WorkPlane::XY, 0.0,
                            &secondWorld);
                    ViewportDepthGeometry wireGeometry;
                    if (linePointsValid) {
                        wireGeometry.lineVertices = {
                            QVector3D(float(firstWorld.x()),
                                      float(firstWorld.y()), 0.0f),
                            QVector3D(float(secondWorld.x()),
                                      float(secondWorld.y()), 0.0f)};
                    }
                    ViewportSceneStroke editWire;
                    editWire.color = Qt::black;
                    editWire.width = 1.0f;
                    editWire.editModeWire = true;
                    editWire.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            std::move(wireGeometry));
                    ViewportSceneRenderer wireRenderer;
                    wireFramebuffer.bind();
                    functions.glViewport(0, 0, glSize.width(), glSize.height());
                    functions.glDisable(GL_FRAMEBUFFER_SRGB);
                    GLint wireColorEncoding = GL_LINEAR;
                    functions.glGetFramebufferAttachmentParameteriv(
                        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
                        &wireColorEncoding);
                    functions.glClearColor(128.0f / 255.0f,
                                           128.0f / 255.0f,
                                           128.0f / 255.0f, 1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    const bool wireDrawSucceeded = linePointsValid &&
                        wireRenderer.draw({editWire}, wireTransform, glSize,
                                          1.0, false, 1.0, true, true);
                    functions.glFinish();
                    // Read the encoded attachment directly; toImage() applies
                    // a Qt color-space conversion to sRGB framebuffer values.
                    unsigned char wireRawPixel[4] = {};
                    functions.glReadPixels(300, 239, 1, 1, GL_RGBA,
                                          GL_UNSIGNED_BYTE, wireRawPixel);
                    gpuWireLinearBlendMatches =
                        wireColorEncoding == GL_SRGB &&
                        wireRawPixel[0] >= 82 && wireRawPixel[0] <= 105 &&
                        wireRawPixel[0] == wireRawPixel[1] &&
                        wireRawPixel[1] == wireRawPixel[2];
                    passed &= check(wireDrawSucceeded &&
                                        gpuWireLinearBlendMatches,
                                    "smooth edit-wire coverage must blend in linear display space on an sRGB target");
                    // Regular curve and surface-outline strokes must use the
                    // same coverage blending, not darker display-space alpha.
                    editWire.editModeWire = false;
                    functions.glDisable(GL_FRAMEBUFFER_SRGB);
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    const bool objectWireDrawSucceeded = wireRenderer.draw(
                        {editWire}, wireTransform, glSize, 1.0, false,
                        1.0, true, true);
                    functions.glFinish();
                    unsigned char objectWireRawPixel[4] = {};
                    functions.glReadPixels(300, 239, 1, 1, GL_RGBA,
                                          GL_UNSIGNED_BYTE, objectWireRawPixel);
                    passed &= check(objectWireDrawSucceeded &&
                                        std::abs(int(objectWireRawPixel[0]) -
                                                 int(wireRawPixel[0])) <= 1,
                                    "curve and surface-outline antialias pixels must match edit-wire linear blending");
                    wireFramebuffer.release();
                }
            }
            context.doneCurrent();
            int gpuFacePixels = 0;
            bool gpuLightingMatchesWorkbench = false;
            bool gpuCubeFaceVisible = false;
            bool gpuMatcapMatchesWorkbench = false;
            bool gpuOutlineRendered = false;
            bool gpuUnselectedFaceColorPreserved = false;
            bool gpuBackfaceCullingKeepsOutwardFaces = false;
            if (!gpuImage.isNull()) {
                for (int y = 0; y < gpuImage.height(); ++y) {
                    for (int x = 0; x < gpuImage.width(); ++x) {
                        const QColor pixel = gpuImage.pixelColor(x, y);
                        gpuFacePixels += pixel.red() > 70 &&
                                         pixel.green() > 70 &&
                                         pixel.blue() > 70;
                    }
                }
                const QVector3D linearBaseColor =
                    workbenchDefaultSolidMaterialDiffuseColor();
                const Point3D viewDirectionPoint =
                    gpuTransform.viewDirection();
                const Point3D viewUpPoint = gpuTransform.viewUp();
                const QVector3D facingVector = workbenchNormalize(
                    QVector3D(static_cast<float>(viewDirectionPoint.x),
                              static_cast<float>(viewDirectionPoint.y),
                              static_cast<float>(viewDirectionPoint.z)),
                    QVector3D(0.0f, 0.0f, 1.0f));
                const QVector3D upVector = workbenchNormalize(
                    QVector3D(static_cast<float>(viewUpPoint.x),
                              static_cast<float>(viewUpPoint.y),
                              static_cast<float>(viewUpPoint.z)),
                    QVector3D(0.0f, 1.0f, 0.0f));
                const QVector3D rightVector = workbenchNormalize(
                    QVector3D::crossProduct(upVector, facingVector),
                    QVector3D(1.0f, 0.0f, 0.0f));
                const QVector3D incidentView(0.0f, 0.0f, 1.0f);
                const auto pixelForWorldPoint = [&](const Point3D &position) {
                    QPointF screen;
                    if (!gpuTransform.worldPointToScreen(position, glSize,
                                                         &screen)) {
                        return QPoint(-1, -1);
                    }
                    return QPoint(qRound(screen.x()), qRound(screen.y()));
                };
                const auto sampleCubeFace = [&](const Point3D &position) {
                    const QPoint pixel = pixelForWorldPoint(position);
                    if (pixel.x() < 0 || pixel.y() < 0) {
                        return QColor();
                    }
                    return gpuImage.pixelColor(pixel);
                };
                const auto viewNormal = [&](const QVector3D &normal) {
                    return QVector3D(
                        QVector3D::dotProduct(normal, rightVector),
                        QVector3D::dotProduct(normal, upVector),
                        QVector3D::dotProduct(normal, facingVector));
                };
                const QVector3D frontNormal = viewNormal(
                    QVector3D(0.0f, -1.0f, 0.0f));
                const QVector3D topNormal = viewNormal(
                    QVector3D(0.0f, 0.0f, 1.0f));
                const QVector3D rightNormal = viewNormal(
                    QVector3D(1.0f, 0.0f, 0.0f));
                const QVector3D unselectedFrontLinear = workbenchStudioShade(
                    linearBaseColor, frontNormal, incidentView);
                const QVector3D unselectedTopLinear = workbenchStudioShade(
                    linearBaseColor, topNormal, incidentView);
                const QVector3D unselectedRightLinear = workbenchStudioShade(
                    linearBaseColor, rightNormal, incidentView);
                const QVector3D selectedOrange = workbenchSrgbToSceneLinear(
                    QVector3D(1.0f, 163.0f / 255.0f, 0.0f));
                const auto selectedFaceColor = [&selectedOrange](
                                                   const QVector3D &sceneLinear) {
                    const QVector3D baseDisplaySrgb =
                        workbenchSceneLinearToAgxSrgb(sceneLinear);
                    const QVector3D baseDisplayLinear =
                        workbenchSrgbToSceneLinear(baseDisplaySrgb);
                    const QVector3D selectedDisplayLinear =
                        baseDisplayLinear * (204.0f / 255.0f) +
                        selectedOrange * (51.0f / 255.0f);
                    return workbenchSceneLinearToSrgb(
                        selectedDisplayLinear);
                };
                const QVector3D expectedFront =
                    selectedFaceColor(unselectedFrontLinear);
                const QVector3D expectedTop =
                    selectedFaceColor(unselectedTopLinear);
                const QVector3D expectedRight =
                    selectedFaceColor(unselectedRightLinear);
                const QVector3D unselectedFront =
                    workbenchSceneLinearToAgxSrgb(unselectedFrontLinear);
                const QVector3D unselectedTop =
                    workbenchSceneLinearToAgxSrgb(unselectedTopLinear);
                const QVector3D unselectedRight =
                    workbenchSceneLinearToAgxSrgb(unselectedRightLinear);
                const QColor gpuFront = sampleCubeFace({0.0, -1.0, 0.0});
                const QColor gpuFrontNearCrease = sampleCubeFace(
                    {0.95, -1.0, 0.0});
                const QColor gpuTop = sampleCubeFace({0.0, 0.0, 1.0});
                const QColor gpuRight = sampleCubeFace({1.0, 0.0, 0.0});
                constexpr qreal channelTolerance = 2.0 / 255.0;
                const auto matchesColor = [channelTolerance](
                                              const QColor &actual,
                                              const QVector3D &expected) {
                    return actual.isValid() &&
                           std::abs(actual.redF() - expected.x()) <
                               channelTolerance &&
                           std::abs(actual.greenF() - expected.y()) <
                               channelTolerance &&
                           std::abs(actual.blueF() - expected.z()) <
                               channelTolerance;
                };
                // Reference values sampled from Blender 5.2.2's Workbench
                // render of the factory cube at this same camera angle.
                const bool lightingMatchesBlender =
                    matchesColor(QColor(138, 139, 139), unselectedFront) &&
                    matchesColor(QColor(142, 144, 145), unselectedTop) &&
                    matchesColor(QColor(53, 50, 50), unselectedRight);
                gpuLightingMatchesWorkbench = lightingMatchesBlender &&
                    matchesColor(gpuFront, expectedFront) &&
                    matchesColor(gpuFrontNearCrease, expectedFront) &&
                    matchesColor(gpuTop, expectedTop) &&
                    matchesColor(gpuRight, expectedRight);
                gpuUnselectedFaceColorPreserved =
                    matchesColor(gpuUnselectedOutlineImage.pixelColor(
                                     pixelForWorldPoint({0.0, -1.0, 0.0})),
                                 unselectedFront) &&
                    matchesColor(gpuUnselectedOutlineImage.pixelColor(
                                     pixelForWorldPoint({0.0, 0.0, 1.0})),
                                 unselectedTop) &&
                    matchesColor(gpuUnselectedOutlineImage.pixelColor(
                                     pixelForWorldPoint({1.0, 0.0, 0.0})),
                                 unselectedRight);
                gpuCubeFaceVisible = gpuFront.isValid() &&
                                     gpuFront.red() > 70;
                const auto countChangedPixels = [](const QImage &withOutline,
                                                   const QImage &withoutOutline) {
                    int count = 0;
                    for (int y = 0; y < withOutline.height(); ++y) {
                        for (int x = 0; x < withOutline.width(); ++x) {
                            count += withOutline.pixel(x, y) !=
                                     withoutOutline.pixel(x, y);
                        }
                    }
                    return count;
                };
                int matchingOutlinePixels = 0;
                bool selectedAndUnselectedOutlineMatch =
                    gpuImage.size() == gpuWithoutOutlineImage.size() &&
                    gpuImage.size() == gpuUnselectedOutlineImage.size() &&
                    gpuImage.size() == gpuUnselectedNoOutlineImage.size();
                const QColor configuredOutline = solidShading.outlineColor;
                if (selectedAndUnselectedOutlineMatch) {
                    for (int y = 0; y < gpuImage.height(); ++y) {
                        for (int x = 0; x < gpuImage.width(); ++x) {
                            const QColor selectedOutline =
                                gpuImage.pixelColor(x, y);
                            const QColor selectedBaseline =
                                gpuWithoutOutlineImage.pixelColor(x, y);
                            const QColor unselectedOutline =
                                gpuUnselectedOutlineImage.pixelColor(x, y);
                            const QColor unselectedBaseline =
                                gpuUnselectedNoOutlineImage.pixelColor(x, y);
                            if (selectedOutline == selectedBaseline ||
                                unselectedOutline == unselectedBaseline) {
                                continue;
                            }
                            ++matchingOutlinePixels;
                            const auto channelRatio = [](int outlined,
                                                         int baseline) {
                                return baseline > 0
                                           ? static_cast<double>(outlined) /
                                                 baseline
                                           : 1.0;
                            };
                            constexpr double outlineRatioTolerance = 0.025;
                            selectedAndUnselectedOutlineMatch &=
                                selectedOutline.alpha() ==
                                    unselectedOutline.alpha() &&
                                configuredOutline == QColor(Qt::black) &&
                                std::abs(channelRatio(selectedOutline.red(),
                                                     selectedBaseline.red()) -
                                         channelRatio(unselectedOutline.red(),
                                                     unselectedBaseline.red())) <
                                    outlineRatioTolerance &&
                                std::abs(channelRatio(selectedOutline.green(),
                                                     selectedBaseline.green()) -
                                         channelRatio(unselectedOutline.green(),
                                                     unselectedBaseline.green())) <
                                    outlineRatioTolerance &&
                                std::abs(channelRatio(selectedOutline.blue(),
                                                     selectedBaseline.blue()) -
                                         channelRatio(unselectedOutline.blue(),
                                                     unselectedBaseline.blue())) <
                                    outlineRatioTolerance;
                        }
                    }
                }
                gpuOutlineRendered =
                    countChangedPixels(gpuImage, gpuWithoutOutlineImage) > 100 &&
                    countChangedPixels(gpuUnselectedOutlineImage,
                                       gpuUnselectedNoOutlineImage) > 100 &&
                    matchingOutlinePixels > 20 &&
                    selectedAndUnselectedOutlineMatch;
                gpuSceneStrokeSuppressesPostOutline =
                    countChangedPixels(gpuSceneStrokeManagedOutlineImage,
                                       gpuWithoutOutlineImage) == 0;
                if (!gpuShadowImage.isNull() &&
                    gpuShadowImage.size() == gpuUnselectedNoOutlineImage.size()) {
                    for (int y = 0; y < gpuShadowImage.height(); ++y) {
                        for (int x = 0; x < gpuShadowImage.width(); ++x) {
                            const QColor shadow = gpuShadowImage.pixelColor(x, y);
                            const QColor baseline =
                                gpuUnselectedNoOutlineImage.pixelColor(x, y);
                            if (std::max({baseline.red() - shadow.red(),
                                          baseline.green() - shadow.green(),
                                          baseline.blue() - shadow.blue()}) >= 2) {
                                ++gpuShadowDarkenedPixels;
                            }
                        }
                    }
                }
                if (!gpuCavityImage.isNull() &&
                    gpuCavityImage.size() == gpuCavityBaselineImage.size()) {
                    for (int y = 0; y < gpuCavityImage.height(); ++y) {
                        for (int x = 0; x < gpuCavityImage.width(); ++x) {
                            const QColor cavity = gpuCavityImage.pixelColor(x, y);
                            const QColor baseline =
                                gpuCavityBaselineImage.pixelColor(x, y);
                            const int maxDelta = std::max({
                                std::abs(cavity.red() - baseline.red()),
                                std::abs(cavity.green() - baseline.green()),
                                std::abs(cavity.blue() - baseline.blue())});
                            gpuCavityChangedPixels += maxDelta >= 2;
                        }
                    }
                }
                int bottomFacePixels = 0;
                for (int y = 0; y < gpuBottomImage.height(); ++y) {
                    for (int x = 0; x < gpuBottomImage.width(); ++x) {
                        const QColor pixel = gpuBottomImage.pixelColor(x, y);
                        bottomFacePixels += pixel.red() > 70 &&
                                            pixel.green() > 70 &&
                                            pixel.blue() > 70;
                    }
                }
                gpuBackfaceCullingKeepsOutwardFaces =
                    bottomFacePixels > 4000;
                const QVector3D expectedMatcap = selectedFaceColor(
                    workbenchMatcapShade(QStringLiteral("metal_bronze"),
                                         linearBaseColor, frontNormal,
                                         incidentView));
                const QPoint frontPixel = pixelForWorldPoint(
                    {0.0, -1.0, 0.0});
                const QColor gpuMatcapCenter = gpuMatcapImage.isNull()
                    || frontPixel.x() < 0 || frontPixel.y() < 0
                    ? QColor()
                    : gpuMatcapImage.pixelColor(frontPixel);
                gpuMatcapMatchesWorkbench =
                    std::abs(gpuMatcapCenter.redF() - expectedMatcap.x()) <
                        channelTolerance &&
                    std::abs(gpuMatcapCenter.greenF() - expectedMatcap.y()) <
                        channelTolerance &&
                    std::abs(gpuMatcapCenter.blueF() - expectedMatcap.z()) <
                        channelTolerance;
            }
            passed &= check(gpuDrawSucceeded && gpuFacePixels > 4000 &&
                            gpuCubeFaceVisible &&
                                gpuLightingMatchesWorkbench &&
                                gpuUnselectedFaceColorPreserved &&
                                gpuUnselectedCaptureNonblank &&
                                gpuMatcapDrawSucceeded &&
                                !gpuMatcapImage.isNull() &&
                                gpuMatcapMatchesWorkbench &&
                                gpuOutlineRendered &&
                                gpuSceneStrokeSuppressesPostOutline &&
                                gpuBottomDrawSucceeded &&
                                gpuBackfaceCullingKeepsOutwardFaces &&
                                gpuShadowDrawSucceeded &&
                                !gpuShadowImage.isNull() &&
                                gpuUnselectedBaselineDrawSucceeded &&
                                gpuShadowDarkenedPixels > 0,
                            "OpenGL NURBS cubes must match Blender face colors, show outlines, retain outward bottom faces under culling, and render the Shadow option");
            passed &= check(gpuCavityDrawSucceeded &&
                                gpuCavityImage.size() == glSize &&
                                gpuCavityBaselineImage.size() == glSize &&
                                gpuCavityChangedPixels > 0,
                            "GPU Workbench world cavity must change pixels from the matching outline-free cube baseline");
            passed &= check(gpuVolumeDrawSucceeded && gpuVolumePassReported,
                            "a bounded scalar volume must complete the GPU volume pass");
            passed &= check(gpuVolumeChangedPixels > 100,
                            "a bounded scalar volume must change offscreen framebuffer pixels");
            passed &= check(gpuLowDensityVolumePreserved,
                            "low-density volume extinction must remain nonzero in the float target");
            passed &= check(gpuSignedVolumeScatteringPreserved,
                            "negative scalar density must retain Blender's signed scattering");
        }
    }

    const QStringList layerLineTypes = standardLayerLineTypes();
    const QVector<qreal> dashedPattern = layerLineTypePattern(QStringLiteral("DASHED"));
    const QVector<qreal> dashedHalfPattern = layerLineTypePattern(QStringLiteral("DASHED2"));
    const QVector<qreal> dashedDoublePattern = layerLineTypePattern(QStringLiteral("DASHEDX2"));
    const QPen dashedPen = layerLineTypePen(Qt::white, 1.0, QStringLiteral("DASHED"));
    passed &= check(layerLineTypes.size() >= 24 &&
                        layerLineTypes.contains(QStringLiteral("CENTER")) &&
                        layerLineTypes.contains(QStringLiteral("DIVIDEX2")) &&
                        layerLineTypes.contains(QStringLiteral("DOTX2")) &&
                        canonicalLayerLineTypeName(QStringLiteral("Dash-Dot")) ==
                            QStringLiteral("DASHDOT"),
                    "layer linetype picker must include standard CAD pattern variants and legacy aliases");
    passed &= check(dashedPattern.size() == 2 && dashedHalfPattern.size() == 2 &&
                        dashedDoublePattern.size() == 2 &&
                        qFuzzyCompare(dashedHalfPattern.first() * 2.0 + 1.0,
                                      dashedPattern.first() + 1.0) &&
                        qFuzzyCompare(dashedDoublePattern.first() + 1.0,
                                      dashedPattern.first() * 2.0 + 1.0) &&
                        dashedPen.style() == Qt::CustomDashLine,
                    "linetype size variants must use scaled, visibly distinct custom dash patterns");

    ViewportSceneStroke gpuStroke;
    gpuStroke.width = 2.0f;
    const ViewportSceneStrokePattern solidGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dashed;
    const ViewportSceneStrokePattern dashedGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dotted;
    const ViewportSceneStrokePattern dottedGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dashed;
    const ViewportSceneStrokePattern highDpiDashPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width * 2.0f);
    gpuStroke.linePatternScale = 0.5f;
    const ViewportSceneStrokePattern halfScaleDashPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    const LayerGpuLinePattern dashedLayerPattern =
        layerGpuLinePattern(QStringLiteral("DASHED2"));
    const LayerGpuLinePattern dottedLayerPattern =
        layerGpuLinePattern(QStringLiteral("DOTX2"));
    const LayerGpuLinePattern complexLayerPattern =
        layerGpuLinePattern(QStringLiteral("CENTER"));
    const LayerGpuLinePattern dashDotLayerPattern =
        layerGpuLinePattern(QStringLiteral("DASHDOT"));
    const LayerGpuLinePattern divideLayerPattern =
        layerGpuLinePattern(QStringLiteral("DIVIDE"));
    const LayerGpuLinePattern borderLayerPattern =
        layerGpuLinePattern(QStringLiteral("BORDER"));
    const LayerGpuLinePattern phantomLayerPattern =
        layerGpuLinePattern(QStringLiteral("PHANTOM"));
    const LayerGpuLinePattern doubledCenterLayerPattern =
        layerGpuLinePattern(QStringLiteral("CENTERX2"));
    const bool allStandardLineTypesSupported = std::all_of(
        layerLineTypes.cbegin(), layerLineTypes.cend(),
        [](const QString &lineType) {
            return layerGpuLinePattern(lineType).kind !=
                   LayerGpuLinePatternKind::Unsupported;
        });
    gpuStroke.lineStyle = ViewportSceneLineStyle::Pattern;
    gpuStroke.linePatternScale = 1.0f;
    gpuStroke.linePatternSegmentCount = complexLayerPattern.segments.size();
    for (int segmentIndex = 0;
         segmentIndex < gpuStroke.linePatternSegmentCount;
         ++segmentIndex) {
        gpuStroke.linePatternSegmentsWidthUnits[
            static_cast<std::size_t>(segmentIndex)] =
            static_cast<float>(complexLayerPattern.segments[segmentIndex]);
    }
    const ViewportSceneStrokePattern complexGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    passed &= check(solidGpuPattern.style == ViewportSceneLineStyle::Solid &&
                        solidGpuPattern.periodPixels == 0.0f &&
                        dashedGpuPattern.style == ViewportSceneLineStyle::Dashed &&
                        dashedGpuPattern.periodPixels == 20.0f &&
                        dashedGpuPattern.onLengthPixels == 14.0f &&
                        dottedGpuPattern.style == ViewportSceneLineStyle::Dotted &&
                        dottedGpuPattern.periodPixels == 6.0f &&
                        dottedGpuPattern.onLengthPixels == 2.0f &&
                        highDpiDashPattern.periodPixels == 40.0f &&
                        highDpiDashPattern.onLengthPixels == 28.0f &&
                        halfScaleDashPattern.periodPixels == 10.0f &&
                        halfScaleDashPattern.onLengthPixels == 7.0f &&
                        dashedLayerPattern.kind ==
                            LayerGpuLinePatternKind::Dashed &&
                        dashedLayerPattern.scale == 0.5 &&
                        dottedLayerPattern.kind ==
                            LayerGpuLinePatternKind::Dotted &&
                        dottedLayerPattern.scale == 2.0 &&
                        complexLayerPattern.kind ==
                            LayerGpuLinePatternKind::Pattern &&
                        dashDotLayerPattern.segments.size() == 4 &&
                        divideLayerPattern.segments.size() == 6 &&
                        borderLayerPattern.segments.size() == 6 &&
                        phantomLayerPattern.segments.size() == 8 &&
                        doubledCenterLayerPattern.segments.size() == 6 &&
                        doubledCenterLayerPattern.segments.first() == 20.0 &&
                        complexGpuPattern.style ==
                            ViewportSceneLineStyle::Pattern &&
                        complexGpuPattern.segmentCount == 6 &&
                        complexGpuPattern.periodPixels == 36.0f &&
                        complexGpuPattern.segmentsPixels[0] == 20.0f &&
                        complexGpuPattern.segmentsPixels[2] == 2.0f &&
                        allStandardLineTypesSupported,
                    "GPU scene strokes must support every built-in layer pattern, including scaled multi-dash and dash-dot styles");
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dotted;
    gpuStroke.dashed = true;
    passed &= check(effectiveViewportSceneLineStyle(gpuStroke) ==
                            ViewportSceneLineStyle::Dashed,
                    "legacy dashed scene strokes must retain dashed rendering when explicit styles are available");
    gpuStroke.dashed = false;
    gpuStroke.controlGuide = true;
    passed &= check(effectiveViewportSceneLineStyle(gpuStroke) ==
                            ViewportSceneLineStyle::Dashed,
                    "control-guide strokes must remain dashed regardless of the selected layer line style");
    return passed ? 0 : 1;
}
