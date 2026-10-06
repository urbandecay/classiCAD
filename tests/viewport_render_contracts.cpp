#include "ui/viewport/line_type_style.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_shading.h"
#include "ui/viewport/viewport_scene_renderer.h"
#include "ui/viewport/viewport_surface_renderer.h"
#include "ui/viewport/workbench_lighting.h"

#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"
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

#include <algorithm>
#include <cstddef>
#include <cmath>

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
            QImage gpuUnselectedOutlineImage;
            QImage gpuBottomImage;
            QImage gpuShadowImage;
            QImage gpuMatcapImage;
            bool gpuDrawSucceeded = false;
            bool gpuMatcapDrawSucceeded = false;
            bool gpuBottomDrawSucceeded = false;
            bool gpuShadowDrawSucceeded = false;
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

                    surfaceObject.selected = false;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuSurfaceRenderer.draw({surfaceObject}, gpuTransform,
                                            glSize, 1.0, gpuSolidShading);
                    functions.glFinish();
                    gpuUnselectedOutlineImage = framebuffer.toImage();
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

                    ViewportShadingSettings shadowShading = gpuSolidShading;
                    shadowShading.shadows = true;
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuShadowDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, gpuTransform, glSize, 1.0,
                        shadowShading);
                    functions.glFinish();
                    gpuShadowImage = framebuffer.toImage();

                    ViewportShadingSettings gpuMatcapShading = gpuSolidShading;
                    gpuMatcapShading.lightingMode = ViewportLightingMode::MatCap;
                    gpuMatcapShading.matcapPreset = QStringLiteral("metal_bronze");
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuMatcapDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, gpuTransform, glSize, 1.0,
                        gpuMatcapShading);
                    functions.glFinish();
                    gpuMatcapImage = framebuffer.toImage();
                    framebuffer.release();
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
                gpuOutlineRendered =
                    countChangedPixels(gpuImage, gpuWithoutOutlineImage) > 100 &&
                    countChangedPixels(gpuUnselectedOutlineImage,
                                       gpuWithoutOutlineImage) > 100;
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
                                gpuMatcapDrawSucceeded &&
                                !gpuMatcapImage.isNull() &&
                                gpuMatcapMatchesWorkbench &&
                                gpuOutlineRendered &&
                                gpuBottomDrawSucceeded &&
                                gpuBackfaceCullingKeepsOutwardFaces &&
                                gpuShadowDrawSucceeded &&
                                !gpuShadowImage.isNull(),
                            "OpenGL NURBS cubes must match Blender face colors, show outlines, retain outward bottom faces under culling, and render the Shadow option");
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
