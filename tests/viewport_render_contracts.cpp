#include "ui/viewport/line_type_style.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_shading.h"
#include "ui/viewport/viewport_scene_renderer.h"
#include "ui/viewport/viewport_surface_renderer.h"
#include "ui/viewport/workbench_lighting.h"

#include "core/geometry/nurbs_surface.h"
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
                        std::abs(roundTripColor.z() - 0.8f) < 1.0e-5f,
                    "solid NURBS shading must use Workbench's studio rig and convert face colors through scene-linear space");

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
    passed &= check(shading.mode == ViewportShadingMode::Wireframe &&
                        shading.xrayEnabled() &&
                        shading.lightingMode == ViewportLightingMode::Studio &&
                        shading.studioLightPreset == QStringLiteral("Default") &&
                        !shading.worldSpaceLighting &&
                        shading.studioLightRotationDegrees == 0,
                    "viewport must preserve through-visible wireframe as its initial mode");
    shading.toggleXray();
    passed &= check(!shading.xrayEnabled() && !shading.xrayWireframe &&
                        !shading.xray,
                    "wireframe X-Ray toggle must update its independent setting");
    shading.mode = ViewportShadingMode::Solid;
    passed &= check(!shading.xrayEnabled(),
                    "solid mode must use its own X-Ray setting");
    shading.toggleXray();
    passed &= check(shading.xrayEnabled() && shading.xray &&
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
    const auto renderedSurface = [&](const ViewportShadingSettings &settings) {
        QImage image(surfaceViewportSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surfaceRenderer.setShadingSettings(settings);
        QPainter painter(&image);
        surfaceRenderer.drawShape(painter, surfaceShape, surfaceViewportSize,
                                 false);
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
    ViewportShadingSettings matcapShading = solidShading;
    matcapShading.lightingMode = ViewportLightingMode::MatCap;
    matcapShading.matcapPreset = QStringLiteral("metal_bronze");
    const RenderedSurface matcapSurface = renderedSurface(matcapShading);
    const QVector3D surfaceBaseLinear =
        workbenchDefaultSolidMaterialDiffuseColor();
    const QVector3D expectedCpuColor = workbenchSceneLinearToSrgb(
        workbenchStudioShade(surfaceBaseLinear,
                             QVector3D(0.0f, 0.0f, 1.0f),
                             QVector3D(0.0f, 0.0f, 1.0f)));
    const QVector3D expectedMatcapColor = workbenchSceneLinearToSrgb(
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
            QImage gpuMatcapImage;
            bool gpuDrawSucceeded = false;
            bool gpuMatcapDrawSucceeded = false;
            const QSize glSize(160, 160);
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
                    surfaceObject.shape = surfaceShape;
                    surfaceObject.objectId = ObjectId::fromValue(1);
                    surfaceObject.geometryRevision = 1;
                    surfaceObject.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(surfaceObject.shape));
                    ViewportTransform gpuTransform;
                    gpuTransform.zoom() = 32.0;
                    gpuTransform.setViewPreset(ViewportViewPreset::Top);
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
            bool gpuMatcapMatchesWorkbench = false;
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
                const QVector3D expectedLighting = workbenchSceneLinearToSrgb(
                    workbenchStudioShade(linearBaseColor,
                                         QVector3D(0.0f, 0.0f, 1.0f),
                                         QVector3D(0.0f, 0.0f, 1.0f)));
                const QColor gpuCenter = gpuImage.pixelColor(80, 80);
                constexpr qreal channelTolerance = 0.025;
                gpuLightingMatchesWorkbench =
                    std::abs(gpuCenter.redF() - expectedLighting.x()) <
                        channelTolerance &&
                    std::abs(gpuCenter.greenF() - expectedLighting.y()) <
                        channelTolerance &&
                    std::abs(gpuCenter.blueF() - expectedLighting.z()) <
                        channelTolerance;
                const QVector3D expectedMatcap = workbenchSceneLinearToSrgb(
                    workbenchMatcapShade(QStringLiteral("metal_bronze"),
                                         linearBaseColor,
                                         QVector3D(0.0f, 0.0f, 1.0f),
                                         QVector3D(0.0f, 0.0f, 1.0f)));
                const QColor gpuMatcapCenter = gpuMatcapImage.isNull()
                    ? QColor()
                    : gpuMatcapImage.pixelColor(80, 80);
                gpuMatcapMatchesWorkbench =
                    std::abs(gpuMatcapCenter.redF() - expectedMatcap.x()) <
                        channelTolerance &&
                    std::abs(gpuMatcapCenter.greenF() - expectedMatcap.y()) <
                        channelTolerance &&
                    std::abs(gpuMatcapCenter.blueF() - expectedMatcap.z()) <
                        channelTolerance;
            }
            passed &= check(gpuDrawSucceeded && gpuFacePixels > 4000 &&
                            gpuImage.pixelColor(80, 80).red() > 70 &&
                                gpuLightingMatchesWorkbench &&
                                gpuMatcapDrawSucceeded &&
                                !gpuMatcapImage.isNull() &&
                                gpuMatcapImage.pixelColor(80, 80).red() > 70 &&
                                gpuMatcapMatchesWorkbench,
                            "OpenGL solid shading must fill NURBS faces with Blender Workbench studio-light colors");
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
