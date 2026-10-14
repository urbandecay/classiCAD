#include "ui/viewport/line_type_style.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_line_weight.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_shading.h"
#include "ui/viewport/viewport_scene_renderer.h"
#include "ui/viewport/viewport_surface_renderer.h"
#include "ui/viewport/viewport_raster_workbench_effects.h"
#include "ui/viewport/viewport_raster_workbench_volume.h"
#include "ui/viewport/viewport_workbench_effect_scale.h"
#include "ui/viewport/viewport_aa_renderer.h"
#include "ui/viewport/viewport_workbench_scene_pass_coordinator.h"
#include "ui/viewport/workbench_render_result.h"
#include "ui/viewport/workbench_lighting.h"
#include "ui/viewport/blender_grid_renderer.h"

#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"
#include "core/geometry/scalar_volume.h"
#include "core/document/document.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/viewport/viewport_transform.h"

#include <QDebug>
#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSurfaceFormat>
#include <QSet>
#include <QTemporaryDir>
#include <QVector>
#include <QVector4D>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

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

    // Blender 5.2.2 DofPass uses sample / samples_len, with no half-sample
    // offset. The gather shader combines this value with IGN and blue-noise
    // alpha to produce a deterministic per-pixel vector.
    passed &= check(
        std::abs(workbenchDofNoiseOffset(0, 8) - 0.0f) < 1.0e-7f &&
            std::abs(workbenchDofNoiseOffset(1, 8) - 0.125f) < 1.0e-7f &&
            std::abs(workbenchDofNoiseOffset(7, 8) - 0.875f) < 1.0e-7f &&
            workbenchDofNoiseOffset(0, 0) == 0.0f,
        "GPU Workbench DOF noise offset must follow Blender's temporal sample index divided by sample count");
    const auto dofNoiseVector = [](float x, float y, float blueNoise,
                                   float offset) {
        const auto fract = [](float value) { return value - std::floor(value); };
        const float ign = fract(offset + 52.9829189f * fract(
            0.06711056f * x + 0.00583715f * y));
        const float angle = 6.28318530718f * fract(blueNoise + offset);
        const float radius = std::sqrt(ign);
        return QVector2D(std::cos(angle) * radius,
                         std::sin(angle) * radius);
    };
    const QVector2D expectedDofNoise = dofNoiseVector(12.5f, 7.5f, 0.4f,
                                                       0.375f);
    passed &= check(std::abs(expectedDofNoise.x() - 0.05872736f) < 2.0e-6f &&
                        std::abs(expectedDofNoise.y() + 0.37078980f) < 2.0e-6f,
                    "Workbench DOF IGN and blue-noise vector must match the pinned shader equation");
    passed &= check(
        rasterWorkbenchDofResolveBlend(0.8f) == 0.0f &&
            rasterWorkbenchDofResolveBlend(1.0f) == 0.0f &&
            std::abs(rasterWorkbenchDofResolveBlend(2.0f) - 0.5f) < 1.0e-7f &&
            rasterWorkbenchDofResolveBlend(3.0f) == 1.0f &&
            rasterWorkbenchDofResolveBlend(4.0f) == 1.0f &&
            rasterWorkbenchDofResolveBlend(-2.0f) ==
                rasterWorkbenchDofResolveBlend(2.0f),
        "QPainter Workbench DOF resolve must use Blender's smoothstep CoC blend from one to three pixels");
    const std::array<QRgb, 9> dofMedianSamples{{
        qRgba(0, 80, 0, 255), qRgba(10, 70, 80, 255),
        qRgba(20, 60, 10, 255), qRgba(30, 50, 70, 0),
        qRgba(40, 40, 20, 255), qRgba(50, 30, 60, 255),
        qRgba(60, 20, 30, 255), qRgba(70, 10, 50, 255),
        qRgba(255, 0, 255, 255)}};
    passed &= check(rasterWorkbenchDofMedian3x3(dofMedianSamples) ==
                        qRgba(40, 40, 50, 255),
                    "QPainter Workbench DOF median cleanup must select the component-wise center of nine RGBA samples");
    if (application.arguments().contains(QStringLiteral("--dof-jitter-only"))) {
        return passed ? 0 : 1;
    }

    ViewportShadingSettings interactiveRenderShading;
    interactiveRenderShading.colorMode = ViewportColorMode::Object;
    ViewportShadingSettings importedSceneRenderShading;
    importedSceneRenderShading.colorMode = ViewportColorMode::Custom;
    importedSceneRenderShading.customColor = QColor(Qt::red);
    const ViewportShadingSettings selectedSceneRenderShading =
        workbenchRenderShadingSettings(interactiveRenderShading,
                                       importedSceneRenderShading, true);
    const ViewportShadingSettings nativeRenderShading =
        workbenchRenderShadingSettings(interactiveRenderShading,
                                       importedSceneRenderShading, false);
    passed &= check(selectedSceneRenderShading.colorMode ==
                            ViewportColorMode::Custom &&
                        selectedSceneRenderShading.customColor == QColor(Qt::red) &&
                        nativeRenderShading.colorMode ==
                            ViewportColorMode::Object,
                    "Workbench render shading must prefer the selected imported scene settings while preserving interactive shading as the native-document fallback");

    ViewportRenderFrame packetFrame;
    packetFrame.viewportSize = QSize(64, 48);
    packetFrame.camera = ViewportTransform();
    const QVector<ViewportRenderObject> packetObjects;
    const ViewportShadingSettings packetSettings;
    const auto packet = ViewportWorkbenchScenePassPacket::fromFrame(
        packetFrame, packetObjects, packetFrame.camera, 1.0,
        packetSettings, nullptr, true);
    const auto invalidPacket = ViewportWorkbenchScenePassPacket::fromScene(
        packetObjects, packetFrame.camera, QSize(), 1.0,
        packetSettings, nullptr, true);
    ViewportRenderTargets mismatchedPacketTargets;
    mismatchedPacketTargets.pixelSize = QSize(32, 24);
    const auto mismatchedTargetPacket =
        ViewportWorkbenchScenePassPacket::fromFrame(
            packetFrame, packetObjects, packetFrame.camera, 1.0,
            packetSettings, &mismatchedPacketTargets, true);
    passed &= check(packet.isValid() && packet.viewportSize == QSize(64, 48) &&
                        packet.objects.isEmpty() &&
                        !invalidPacket.isValid() &&
                        !mismatchedTargetPacket.isValid(),
                    "shared Workbench scene-pass packets must carry render-frame dimensions and reject empty or size-mismatched AA targets");

    const Point3D cavityCamera{0.0, 0.0, 10.0};
    const Point3D cavityTarget{0.0, 0.0, 0.0};
    const Point3D cavityViewDirection{0.0, 0.0, 1.0};
    const float nearCavityStep = rasterWorkbenchCavityStep(
        20.0f, QVector3D(0.0f, 0.0f, 8.0f), cavityCamera,
        cavityTarget, cavityViewDirection, true);
    const float farCavityStep = rasterWorkbenchCavityStep(
        20.0f, QVector3D(0.0f, 0.0f, 5.0f), cavityCamera,
        cavityTarget, cavityViewDirection, true);
    const float orthographicNearStep = rasterWorkbenchCavityStep(
        20.0f, QVector3D(0.0f, 0.0f, 8.0f), cavityCamera,
        cavityTarget, cavityViewDirection, false);
    const float orthographicFarStep = rasterWorkbenchCavityStep(
        20.0f, QVector3D(0.0f, 0.0f, 5.0f), cavityCamera,
        cavityTarget, cavityViewDirection, false);
    passed &= check(nearCavityStep > farCavityStep &&
                        orthographicNearStep == 10.0f &&
                        orthographicFarStep == orthographicNearStep,
                    "QPainter cavity SSAO footprint must shrink with perspective receiver depth and stay fixed in orthographic views");

    const QVector3D frontPrepassNormal = rasterWorkbenchPrepassNormal(
        QVector3D(0.2f, -0.3f, 0.9327379f), true,
        QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
        QVector3D(0.0f, 0.0f, 1.0f));
    const QVector3D backPrepassNormal = rasterWorkbenchPrepassNormal(
        QVector3D(-0.2f, 0.3f, -0.9327379f), false,
        QVector3D(1.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f),
        QVector3D(0.0f, 0.0f, 1.0f));
    passed &= check((frontPrepassNormal - backPrepassNormal).length() < 1.0e-5f &&
                        frontPrepassNormal.z() > 0.9f &&
                        std::abs(frontPrepassNormal.length() - 1.0f) < 0.01f,
                    "QPainter Workbench effect normals must apply Blender facing orientation and RG16F encode/decode");

    const float ridgeResponse = rasterWorkbenchCurvatureResponse(
        1, 1, 1, 1, 0.5f, 0.0f, 0.25f, 0.0f, 1.0f, 1.0f);
    const float valleyResponse = rasterWorkbenchCurvatureResponse(
        1, 1, 1, 1, 0.0f, 0.5f, 0.0f, 0.25f, 1.0f, 1.0f);
    const float reducedRidgeResponse = rasterWorkbenchCurvatureResponse(
        1, 1, 1, 1, 0.5f, 0.0f, 0.25f, 0.0f, 0.5f, 1.0f);
    passed &= check(ridgeResponse > 0.0f && valleyResponse < 0.0f &&
                        reducedRidgeResponse > 0.0f &&
                        reducedRidgeResponse < ridgeResponse,
                    "QPainter curvature must preserve Blender ridge/valley signs and respond to the separate ridge control");
    passed &= check(
        rasterWorkbenchCurvatureResponse(1, 2, 1, 1, 0.5f, 0.0f,
                                         0.25f, 0.0f, 1.0f, 1.0f) == 0.0f &&
            rasterWorkbenchCurvatureResponse(0, 0, 0, 0, 0.5f, 0.0f,
                                             0.25f, 0.0f, 1.0f, 1.0f) == 0.0f,
        "QPainter curvature must suppress object-ID outlines and background");

    passed &= check(
        blenderRenderAntiAliasingMode(QStringLiteral("OFF")) == 0 &&
            blenderRenderAntiAliasingMode(QStringLiteral("FXAA")) == 1 &&
            blenderRenderAntiAliasingMode(QStringLiteral("5")) == 5 &&
            blenderRenderAntiAliasingMode(QStringLiteral("8")) == 8 &&
            blenderRenderAntiAliasingMode(QStringLiteral("11")) == 11 &&
            blenderRenderAntiAliasingMode(QStringLiteral("16")) == 16 &&
            blenderRenderAntiAliasingMode(QStringLiteral("32")) == 32 &&
            blenderRenderAntiAliasingMode(QStringLiteral("")) == -1 &&
            blenderRenderAntiAliasingMode(QStringLiteral("UNKNOWN")) == -1,
        "Blender render AA enum must override viewport AA only for supported values");

    passed &= check(workbenchRenderDitherIntensity(1.0, 8) == 1.0 &&
                        workbenchRenderDitherIntensity(1.0, 16) == 0.0 &&
                        workbenchRenderDitherIntensity(1.0, 32) == 0.0 &&
                        workbenchRenderDitherIntensity(-1.0, 8) == 0.0 &&
                        workbenchRenderDitherIntensity(
                            std::numeric_limits<qreal>::quiet_NaN(), 8) == 0.0,
                    "Blender render dither must apply only to finite nonnegative 8-bit byte output");

    {
        const auto frameContains = [](const Document &document,
                                       ObjectId id,
                                       ViewportRenderFrameInput::Visibility visibility) {
            ViewportRenderFrameInput input;
            input.visibility = visibility;
            return buildViewportRenderFrame(document, ViewportTransform(),
                                            QSize(64, 64), input).find(id) != nullptr;
        };
        const auto insertVisibilityObject = [](Document *document,
                                                bool hideViewport,
                                                bool hiddenInViewport,
                                                bool hideRender) {
            SceneObject object;
            object.geometry.geometryType = GeometryType::PolygonMesh;
            object.hideViewport = hideViewport;
            object.hiddenInViewport = hiddenInViewport;
            object.hideRender = hideRender;
            return document->insertObject(0, object);
        };

        Document viewportHiddenDocument;
        const ObjectId viewportHiddenId = insertVisibilityObject(
            &viewportHiddenDocument, true, true, false);
        passed &= check(
            !frameContains(viewportHiddenDocument, viewportHiddenId,
                           ViewportRenderFrameInput::Visibility::Viewport) &&
                frameContains(viewportHiddenDocument, viewportHiddenId,
                              ViewportRenderFrameInput::Visibility::Render),
            "Workbench render frames must include objects hidden only in the viewport");

        Document renderHiddenDocument;
        const ObjectId renderHiddenId = insertVisibilityObject(
            &renderHiddenDocument, false, false, true);
        passed &= check(
            frameContains(renderHiddenDocument, renderHiddenId,
                          ViewportRenderFrameInput::Visibility::Viewport) &&
                !frameContains(renderHiddenDocument, renderHiddenId,
                               ViewportRenderFrameInput::Visibility::Render),
            "Workbench render frames must omit objects hidden only from rendering");

        DocumentCollection collection;
        collection.name = QStringLiteral("Visibility");
        collection.sourceId = QStringLiteral("collection-visibility");
        collection.viewLayerStates.append(QJsonObject{
            {QStringLiteral("view_layer_key"), QStringLiteral("view-layer")},
            {QStringLiteral("path"), QJsonArray{QStringLiteral("collection-visibility")}},
            {QStringLiteral("exclude"), false},
            {QStringLiteral("hide_viewport"), false}});
        Document renderViewLayerDocument;
        DocumentSettings visibilitySettings = renderViewLayerDocument.settings();
        visibilitySettings.activeViewLayerKey = QStringLiteral("view-layer");
        renderViewLayerDocument.setSettings(visibilitySettings);
        const CollectionId visibilityCollection =
            renderViewLayerDocument.insertCollection(collection);
        const ObjectId collectionObjectId = insertVisibilityObject(
            &renderViewLayerDocument, false, false, false);
        renderViewLayerDocument.setObjectCollections(
            collectionObjectId, {visibilityCollection});
        renderViewLayerDocument.setCollectionVisibility(
            visibilityCollection, true, false);
        passed &= check(
            !frameContains(renderViewLayerDocument, collectionObjectId,
                           ViewportRenderFrameInput::Visibility::Viewport) &&
                frameContains(renderViewLayerDocument, collectionObjectId,
                              ViewportRenderFrameInput::Visibility::Render),
            "render visibility must honor active-layer collection membership while ignoring its viewport hide flag");

        renderViewLayerDocument.setCollectionVisibility(
            visibilityCollection, false, true);
        passed &= check(
            frameContains(renderViewLayerDocument, collectionObjectId,
                          ViewportRenderFrameInput::Visibility::Viewport) &&
                !frameContains(renderViewLayerDocument, collectionObjectId,
                               ViewportRenderFrameInput::Visibility::Render),
            "Workbench render frames must honor collection hide_render without hiding its viewport object");
    }

    const Point3D dofCameraPosition{2.0, -3.0, 10.0};
    const Point3D dofViewDirection{0.0, 0.0, 1.0};
    const QVector3D dofFocusedPoint(2.0f, -3.0f, 2.0f);
    const QVector3D dofWorldShift(7.0f, -11.0f, 19.0f);
    const Point3D shiftedDofCameraPosition{
        dofCameraPosition.x + dofWorldShift.x(),
        dofCameraPosition.y + dofWorldShift.y(),
        dofCameraPosition.z + dofWorldShift.z()};
    passed &= check(
        std::abs(rasterWorkbenchCameraDepth(
                     dofFocusedPoint, dofCameraPosition,
                     dofViewDirection) - 8.0f) < 1.0e-6f &&
            std::abs(rasterWorkbenchCameraDepth(
                         dofFocusedPoint + dofWorldShift,
                         shiftedDofCameraPosition, dofViewDirection) -
                     8.0f) < 1.0e-6f,
        "QPainter Workbench DOF depth must stay camera-relative when the camera and scene translate together");

    const QPointF rotatedFourBladeTap = rasterWorkbenchApertureTap(
        QPointF(1.0, 0.0), 4, 0.7853981633974483, 1.0);
    passed &= check(
        std::abs(rotatedFourBladeTap.x() - 0.5) < 1.0e-9 &&
            std::abs(rotatedFourBladeTap.y() - 0.5) < 1.0e-9,
        "QPainter Workbench polygon aperture must shape from the unrotated sector, then rotate the tap once");

    const auto dofKernel = rasterWorkbenchDofKernel(0, 0.0, 1.0);
    qreal dofKernelRadiusSum = 0.0;
    for (const RasterWorkbenchDofTap &tap : dofKernel)
        dofKernelRadiusSum += std::hypot(tap.position.x(), tap.position.y());
    passed &= check(
        dofKernel.front().position == QPointF(0.0, 0.0) &&
            dofKernel.front().radius == 0.0f &&
            std::abs(std::hypot(dofKernel.back().position.x(),
                                dofKernel.back().position.y()) - 1.0) < 1.0e-9 &&
            std::abs(dofKernel.back().radius - 1.0f) < 1.0e-7f &&
            std::abs(dofKernelRadiusSum - 112.0 / 3.0) < 1.0e-8,
        "QPainter Workbench DOF kernel must preserve Blender's 49 square-ring perimeter samples and disk radii");
    const auto hexagonalDofKernel = rasterWorkbenchDofKernel(
        6, 0.2, 2.0);
    constexpr qreal dofPi = 3.141592653589793;
    const qreal expectedHexRadius =
        (2.0 / 3.0) * std::cos(dofPi / 6.0) /
        std::cos(dofPi / 12.0);
    passed &= check(
        hexagonalDofKernel.size() == 49 &&
            std::abs(hexagonalDofKernel[9].position.x() -
                     expectedHexRadius *
                         std::cos(5.0 * dofPi / 4.0 + 0.2) / 2.0) < 1.0e-8 &&
            std::abs(hexagonalDofKernel[9].position.y() -
                     expectedHexRadius *
                         std::sin(5.0 * dofPi / 4.0 + 0.2)) < 1.0e-8 &&
            std::abs(hexagonalDofKernel[9].radius - 2.0f / 3.0f) < 1.0e-7f,
        "QPainter Workbench DOF kernel must apply Blender's blade polygon, rotation, and reciprocal X ratio to all 49 taps");

    const RasterWorkbenchDofPrefilter dofPrefilter =
        rasterWorkbenchDofPrefilter2x2({4.0f, 3.0f, -4.0f, -3.0f});
    passed &= check(
        dofPrefilter.nearCoc == 4.0f && dofPrefilter.farCoc == 4.0f &&
            dofPrefilter.colorWeights ==
                std::array<float, 4>{0.0f, 0.0f, 1.0f, 0.0f},
        "QPainter Workbench DOF prefilter must retain separate near/far maxima, reject unmatched CoCs, and prioritize far color");
    const RasterWorkbenchDofPrefilter zeroDofPrefilter =
        rasterWorkbenchDofPrefilter2x2({0.0f, 0.0f, 0.0f, 0.0f});
    const RasterWorkbenchDofPrefilter mixedZeroDofPrefilter =
        rasterWorkbenchDofPrefilter2x2({0.0f, 1.0f, 0.0f, -1.0f});
    passed &= check(
        zeroDofPrefilter.nearCoc == 0.0f &&
            zeroDofPrefilter.farCoc == 0.0f &&
            zeroDofPrefilter.colorWeights ==
                std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f} &&
            mixedZeroDofPrefilter.nearCoc == 1.0f &&
            mixedZeroDofPrefilter.farCoc == 1.0f &&
            mixedZeroDofPrefilter.colorWeights ==
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f},
        "QPainter Workbench DOF prefilter must include zero CoCs in both signed classes and keep far priority");
    passed &= check(
        std::abs(rasterWorkbenchDofGatherWeight(4.0f, 4.0f, 0.5f) - 4.0f) <
                1.0e-6f &&
            std::abs(rasterWorkbenchDofGatherWeight(1.5f, 4.0f, 0.375f) -
                     0.75f) < 1.0e-6f &&
            rasterWorkbenchDofGatherWeight(0.0f, 4.0f, 0.0f) == 0.0f,
        "QPainter Workbench DOF gather weights must match Blender's radial CoC smoothstep");
    passed &= check(
        rasterWorkbenchDofMipLod(21.0f, 21.0f) == 0.0f &&
            rasterWorkbenchDofMipLod(22.0f, 22.0f) == 0.0f &&
            std::abs(rasterWorkbenchDofMipLod(29.0f, 29.0f) - 1.0f) < 1.0e-6f &&
            rasterWorkbenchDofMipLod(37.0f, 37.0f) == 2.0f &&
            rasterWorkbenchDofMipLod(100.0f, 100.0f) == 2.0f &&
            std::abs(rasterWorkbenchDofMipLod(50.0f, 10.0f) -
                     std::log2(2.25f)) < 1.0e-6f &&
            rasterWorkbenchDofMipLod(-50.0f, -10.0f) ==
                rasterWorkbenchDofMipLod(50.0f, 10.0f),
        "QPainter Workbench DOF mip LOD must follow Blender's CoC formula and stay within its three-level pyramid");
    passed &= check(
        rasterWorkbenchDofTapCountForBudget(0) == 49 &&
            rasterWorkbenchDofTapCountForBudget(100) == 49 &&
            rasterWorkbenchDofTapCountForBudget(1'000'000) == 24 &&
            rasterWorkbenchDofTapCountForBudget(24'000'001) == 1,
        "QPainter Workbench DOF gather must preserve 49 taps for sparse regions and cap dense-region work at 24 million taps");

    ViewportTransform orthographicDofTransform;
    orthographicDofTransform.setViewPreset(ViewportViewPreset::Top);
    orthographicDofTransform.setPerspectiveEnabled(false);
    ViewportCameraPreferences orthographicDofOptics;
    orthographicDofOptics.sceneCameraEnabled = true;
    orthographicDofOptics.focusDistance = 8.0;
    orthographicDofOptics.fStop = 0.1;
    orthographicDofOptics.clipEnd = 80.0;
    orthographicDofOptics.opticalSensorSizeMillimeters = 36.0;
    passed &= check(orthographicDofTransform.setCameraPreferences(
                        orthographicDofOptics),
                    "orthographic DOF contract optics must be valid");
    const QSize orthographicDofSize(128, 128);
    const Point3D orthographicDofTarget =
        orthographicDofTransform.viewTarget();
    const Point3D orthographicDofOutward =
        orthographicDofTransform.viewDirection();
    const auto makeOrthographicDofObject = [&](quint64 id, qreal depth,
                                                qreal left, qreal right) {
        ViewportRenderObject object;
        object.shape.geometryType = GeometryType::NurbsSurface;
        object.objectId = ObjectId::fromValue(id);
        auto geometry = QSharedPointer<ViewportDepthGeometry>::create();
        const auto worldAt = [&](qreal x, qreal y) {
            QPointF local;
            orthographicDofTransform.screenToWorkPlaneUnclipped(
                QPointF(x, y), orthographicDofSize, WorkPlane::XY, 0.0,
                &local);
            return QVector3D(
                float(local.x() - orthographicDofOutward.x * depth),
                float(local.y() - orthographicDofOutward.y * depth),
                float(orthographicDofTarget.z -
                      orthographicDofOutward.z * depth));
        };
        const QVector3D a = worldAt(left, 8.0);
        const QVector3D b = worldAt(right, 8.0);
        const QVector3D c = worldAt(right, 120.0);
        const QVector3D d = worldAt(left, 120.0);
        // screenToWorkPlane maps screen-down to decreasing world Y. Keep the
        // triangles counter-clockwise from Top so their +Z normals are front
        // facing, matching Blender's gl_FrontFacing prepass semantics.
        geometry->surfaceVertices = {a, c, b, a, d, c};
        geometry->surfaceNormals =
            QVector<QVector3D>(6, QVector3D(0.0f, 0.0f, 1.0f));
        object.preparedDepthGeometry = geometry;
        return object;
    };
    const QVector<ViewportRenderObject> orthographicDofObjects{
        makeOrthographicDofObject(1, 8.0, 8.0, 56.0),
        makeOrthographicDofObject(2, 16.0, 72.0, 120.0)};
    QImage orthographicDofImage(orthographicDofSize,
                                QImage::Format_ARGB32_Premultiplied);
    orthographicDofImage.fill(Qt::black);
    orthographicDofImage.setPixelColor(32, 64, Qt::red);
    orthographicDofImage.setPixelColor(96, 64, Qt::red);
    ViewportShadingSettings orthographicDofSettings;
    orthographicDofSettings.mode = ViewportShadingMode::Solid;
    orthographicDofSettings.outline = false;
    orthographicDofSettings.depthOfField = true;
    applyRasterWorkbenchEffects(
        &orthographicDofImage, orthographicDofObjects,
        orthographicDofTransform, orthographicDofSize, 1.0,
        orthographicDofSettings);
    const int focusedOrthographicRed =
        orthographicDofImage.pixelColor(32, 64).red();
    const int defocusedOrthographicRed =
        orthographicDofImage.pixelColor(96, 64).red();
    const qreal focalMeters =
        orthographicDofOptics.focalLengthMillimeters * 0.001;
    const qreal focusDistance = orthographicDofOptics.focusDistance;
    const qreal sensorWidthMeters =
        orthographicDofOptics.opticalSensorSizeMillimeters * 0.001;
    const qreal cocScale =
        0.5 * focalMeters * focalMeters /
        (orthographicDofOptics.fStop *
         std::abs(focusDistance - focalMeters)) *
        orthographicDofSize.width() / sensorWidthMeters;
    const qreal subpixelCocDepth =
        focusDistance / (1.0 - 0.8 / cocScale);
    const ViewportRenderObject subpixelCocObject =
        makeOrthographicDofObject(5, subpixelCocDepth, 8.0, 56.0);
    QImage subpixelCocImage(orthographicDofSize,
                            QImage::Format_ARGB32_Premultiplied);
    subpixelCocImage.fill(Qt::black);
    subpixelCocImage.setPixelColor(32, 64, Qt::red);
    const QImage subpixelCocSharpImage = subpixelCocImage.copy();
    applyRasterWorkbenchEffects(
        &subpixelCocImage, {subpixelCocObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, orthographicDofSettings);

    const auto renderRetainedOutlineDof = [&](int colorTransform,
                                              bool withOutline,
                                              bool withDof) {
        QImage image(orthographicDofSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers layers;
        layers.sceneLinearSize = orthographicDofSize;
        const std::size_t pixelCount = static_cast<std::size_t>(
            orthographicDofSize.width() * orthographicDofSize.height());
        layers.sceneLinearColor.resize(pixelCount);
        layers.sceneLinearOutlineCoverage.assign(pixelCount, 0.0f);
        for (int y = 0; y < orthographicDofSize.height(); ++y) {
            for (int x = 0; x < orthographicDofSize.width(); ++x) {
                const float alpha = 0.75f;
                const QVector3D straight = x < 64
                    ? QVector3D(0.9f, 0.08f, 0.03f)
                    : QVector3D(0.02f, 0.12f, 0.85f);
                layers.sceneLinearColor[
                    static_cast<std::size_t>(y) * orthographicDofSize.width() + x] = {
                    straight.x() * alpha, straight.y() * alpha,
                    straight.z() * alpha, alpha};
                if (withOutline && x == 64)
                    layers.sceneLinearOutlineCoverage[
                        static_cast<std::size_t>(y) *
                            orthographicDofSize.width() + x] = 0.5f;
            }
        }
        layers.sceneLinearOutlinePending = withOutline;
        layers.sceneLinearDofPending = withDof;
        ViewportShadingSettings settings = orthographicDofSettings;
        settings.outline = withOutline;
        settings.depthOfField = withDof;
        settings.outlineColor = QColor(235, 185, 35);
        settings.colorTransform = colorTransform;
        applyRasterWorkbenchEffects(
            &image, orthographicDofObjects, orthographicDofTransform,
            orthographicDofSize, 1.0, settings, &layers);
        return std::make_pair(std::move(image), std::move(layers));
    };
    auto standardOutlineDof = renderRetainedOutlineDof(1, true, true);
    auto agxOutlineDof = renderRetainedOutlineDof(0, true, true);
    auto noOutlineDof = renderRetainedOutlineDof(1, false, true);
    auto sharpOutline = renderRetainedOutlineDof(1, true, false);
    bool retainedDofDisplayMatches = true;
    bool retainedDofOutlineChangedScene = false;
    bool retainedDofChangedScene = false;
    for (int y = 0; y < orthographicDofSize.height(); ++y) {
        for (int x = 0; x < orthographicDofSize.width(); ++x) {
            const std::size_t index = static_cast<std::size_t>(
                y * orthographicDofSize.width() + x);
            const RasterWorkbenchLinearPixel &pixel =
                standardOutlineDof.second.sceneLinearColor[index];
            const QVector3D display = workbenchSceneLinearToDisplaySrgb(
                QVector3D(pixel.red / pixel.alpha, pixel.green / pixel.alpha,
                          pixel.blue / pixel.alpha),
                1, 0.0f, 1.0f, {}, {});
            const QRgb expected = qPremultiply(qRgba(
                qRound(display.x() * 255.0f),
                qRound(display.y() * 255.0f),
                qRound(display.z() * 255.0f), qRound(pixel.alpha * 255.0f)));
            retainedDofDisplayMatches &=
                standardOutlineDof.first.pixel(x, y) == expected;
            retainedDofChangedScene |= std::abs(
                pixel.red - sharpOutline.second.sceneLinearColor[index].red) >
                1.0e-4f || std::abs(
                pixel.green - sharpOutline.second.sceneLinearColor[index].green) >
                1.0e-4f || std::abs(
                pixel.blue - sharpOutline.second.sceneLinearColor[index].blue) >
                1.0e-4f;
            retainedDofOutlineChangedScene |= std::abs(
                pixel.red - noOutlineDof.second.sceneLinearColor[index].red) >
                1.0e-4f || std::abs(
                pixel.green - noOutlineDof.second.sceneLinearColor[index].green) >
                1.0e-4f || std::abs(
                pixel.blue - noOutlineDof.second.sceneLinearColor[index].blue) >
                1.0e-4f;
            const float expectedAlpha = x == 64 ? 0.875f : 0.75f;
            passed &= check(std::abs(pixel.alpha - expectedAlpha) < 1.0e-5f,
                "scene-linear QPainter DOF must preserve sharp alpha apart from outline coverage");
        }
    }
    passed &= check(
        !standardOutlineDof.second.sceneLinearDofPending &&
            !standardOutlineDof.second.sceneLinearOutlinePending &&
            standardOutlineDof.second.outlineAppliedBeforeDisplay &&
            !agxOutlineDof.second.sceneLinearDofPending &&
            !agxOutlineDof.second.sceneLinearOutlinePending &&
            !sharpOutline.second.sceneLinearDofPending &&
            sharpOutline.second.outlineAppliedBeforeDisplay &&
            retainedDofDisplayMatches && retainedDofChangedScene &&
            retainedDofOutlineChangedScene &&
            standardOutlineDof.first != agxOutlineDof.first,
        "weighted QPainter outline-plus-DOF must filter retained scene-linear color before one Standard or AgX display transform");

    CurveHitTester sortedDofHitTester;
    ViewportRenderer sortedDofRenderer(orthographicDofTransform,
                                       sortedDofHitTester);
    QVector<ViewportRenderObject> sortedDofObjects = orthographicDofObjects;
    for (ViewportRenderObject &object : sortedDofObjects) {
        object.shape.geometryType = GeometryType::PolygonMesh;
        const QVector<QVector3D> &vertices =
            object.preparedDepthGeometry->surfaceVertices;
        object.shape.polygonMesh.vertices.reserve(vertices.size());
        for (const QVector3D &vertex : vertices) {
            object.shape.polygonMesh.vertices.append(
                Point3D{vertex.x(), vertex.y(), vertex.z()});
        }
        object.shape.polygonMesh.faces = {{0, 1, 2}, {3, 4, 5}};
        object.shape.polygonMesh.faceMaterialIndices = {0, 0};
        object.hasWorkbenchMaterial = true;
        object.workbenchMaterial.baseColorSceneLinear =
            object.objectId.value() == 1
                ? QVector3D(0.9f, 0.08f, 0.03f)
                : QVector3D(0.02f, 0.12f, 0.85f);
        object.meshMaterials = {object.workbenchMaterial};
    }
    const auto drawSortedDofObject = [&sortedDofRenderer,
                                      &orthographicDofSize](
        QPainter &painter, const ViewportRenderObject &object,
        RasterWorkbenchFragmentSink *fragmentSink) {
        sortedDofRenderer.drawShape(
            painter, object.shape, orthographicDofSize, false, false, true,
            QColor(), QString(), 0.0, object.objectId,
            object.geometryRevision, object.preparedDepthGeometry.data(),
            object.preparedGeometryOffset, object.displayColorSceneLinear,
            object.workbenchMaterial, object.displayName, object.libraryPath,
            object.meshObjectTransform, object.meshMaterials, fragmentSink);
    };
    struct SortedDofOutlineRender {
        QImage image;
        RasterWorkbenchSurfaceLayers layers;
        std::vector<RasterWorkbenchLinearPixel> beforeEffects;
        bool rendered = false;
    };
    const auto renderSortedDofOutline = [&](int colorTransform,
                                            bool withOutline,
                                            bool withDof,
                                            bool withShadows = false,
                                            bool useWeightedOit = false) {
        ViewportShadingSettings settings = orthographicDofSettings;
        settings.outline = withOutline;
        settings.depthOfField = withDof;
        settings.shadows = withShadows;
        settings.shadowIntensity = 1.0;
        settings.shadowFocus = 0.9;
        settings.outlineColor = QColor(235, 185, 35);
        settings.colorTransform = colorTransform;
        QImage image(orthographicDofSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        SortedDofOutlineRender result;
        sortedDofRenderer.setShadingSettings(settings);
        result.rendered = compositeRasterWorkbenchSurfaces(
            &image, sortedDofObjects, orthographicDofTransform,
            orthographicDofSize, 1.0, false, useWeightedOit, settings,
            QVector3D(0.12f, 0.07f, 0.035f), drawSortedDofObject,
            &result.layers);
        result.beforeEffects = result.layers.sceneLinearColor;
        applyRasterWorkbenchEffects(
            &image, sortedDofObjects, orthographicDofTransform,
            orthographicDofSize, 1.0, settings, &result.layers);
        result.image = std::move(image);
        return result;
    };
    const SortedDofOutlineRender sortedStandardDofOutline =
        renderSortedDofOutline(1, true, true);
    const SortedDofOutlineRender sortedAgxDofOutline =
        renderSortedDofOutline(0, true, true);
    const SortedDofOutlineRender sortedStandardDofOnly =
        renderSortedDofOutline(1, false, true);
    const SortedDofOutlineRender sortedStandardShadowDofOutline =
        renderSortedDofOutline(1, true, true, true);
    const SortedDofOutlineRender sortedAgxShadowDofOutline =
        renderSortedDofOutline(0, true, true, true);
    const SortedDofOutlineRender weightedStandardShadowDofOutline =
        renderSortedDofOutline(1, true, true, true, true);
    const SortedDofOutlineRender weightedAgxShadowDofOutline =
        renderSortedDofOutline(0, true, true, true, true);
    bool sortedDofDisplayMatches = true;
    bool sortedDofChangedScene = false;
    bool sortedDofOutlineChangedScene = false;
    bool sortedShadowChangedScene = false;
    bool weightedDofDisplayMatches = true;
    bool weightedDofChangedScene = false;
    if (sortedStandardDofOutline.layers.sceneLinearSize ==
            orthographicDofSize &&
        sortedAgxDofOutline.layers.sceneLinearSize == orthographicDofSize &&
        sortedStandardDofOnly.layers.sceneLinearSize == orthographicDofSize &&
        sortedStandardShadowDofOutline.layers.sceneLinearSize ==
            orthographicDofSize &&
        sortedAgxShadowDofOutline.layers.sceneLinearSize ==
            orthographicDofSize &&
        weightedStandardShadowDofOutline.layers.sceneLinearSize ==
            orthographicDofSize &&
        weightedAgxShadowDofOutline.layers.sceneLinearSize ==
            orthographicDofSize) {
        for (int y = 0; y < orthographicDofSize.height(); ++y) {
            for (int x = 0; x < orthographicDofSize.width(); ++x) {
                const std::size_t index = static_cast<std::size_t>(
                    y * orthographicDofSize.width() + x);
                const RasterWorkbenchLinearPixel &standard =
                    sortedStandardDofOutline.layers.sceneLinearColor[index];
                const RasterWorkbenchLinearPixel &agx =
                    sortedAgxDofOutline.layers.sceneLinearColor[index];
                const RasterWorkbenchLinearPixel &withoutOutline =
                    sortedStandardDofOnly.layers.sceneLinearColor[index];
                const RasterWorkbenchLinearPixel &withShadow =
                    sortedStandardShadowDofOutline.layers.sceneLinearColor[index];
                sortedDofChangedScene |= std::abs(
                    standard.red - sortedStandardDofOutline.beforeEffects[
                        index].red) > 1.0e-5f || std::abs(
                    standard.green - sortedStandardDofOutline.beforeEffects[
                        index].green) > 1.0e-5f || std::abs(
                    standard.blue - sortedStandardDofOutline.beforeEffects[
                        index].blue) > 1.0e-5f;
                sortedDofOutlineChangedScene |= std::abs(
                    standard.red - withoutOutline.red) > 1.0e-5f ||
                    std::abs(standard.green - withoutOutline.green) > 1.0e-5f ||
                    std::abs(standard.blue - withoutOutline.blue) > 1.0e-5f;
                sortedShadowChangedScene |= std::abs(
                    standard.red - withShadow.red) > 1.0e-5f ||
                    std::abs(standard.green - withShadow.green) > 1.0e-5f ||
                    std::abs(standard.blue - withShadow.blue) > 1.0e-5f;
                sortedDofDisplayMatches &=
                    std::abs(standard.red - agx.red) < 1.0e-5f &&
                    std::abs(standard.green - agx.green) < 1.0e-5f &&
                    std::abs(standard.blue - agx.blue) < 1.0e-5f &&
                    std::abs(standard.alpha - agx.alpha) < 1.0e-5f;
                for (const auto &[pixel, transformIndex] : {
                         std::pair<QRgb, int>{
                             sortedStandardDofOutline.image.pixel(x, y), 1},
                         std::pair<QRgb, int>{
                             sortedAgxDofOutline.image.pixel(x, y), 0}}) {
                    const RasterWorkbenchLinearPixel &linear =
                        transformIndex == 1 ? standard : agx;
                    const QVector3D display =
                        workbenchSceneLinearToDisplaySrgb(
                            QVector3D(linear.red / linear.alpha,
                                      linear.green / linear.alpha,
                                      linear.blue / linear.alpha),
                            transformIndex, 0.0f, 1.0f, {}, {});
                    sortedDofDisplayMatches &= pixel == qPremultiply(qRgba(
                        qRound(display.x() * 255.0f),
                        qRound(display.y() * 255.0f),
                        qRound(display.z() * 255.0f),
                        qRound(linear.alpha * 255.0f)));
                }
                const RasterWorkbenchLinearPixel &shadowStandard =
                    sortedStandardShadowDofOutline.layers
                        .sceneLinearColor[index];
                const RasterWorkbenchLinearPixel &shadowAgx =
                    sortedAgxShadowDofOutline.layers.sceneLinearColor[index];
                sortedDofDisplayMatches &=
                    std::abs(shadowStandard.red - shadowAgx.red) < 1.0e-5f &&
                    std::abs(shadowStandard.green - shadowAgx.green) < 1.0e-5f &&
                    std::abs(shadowStandard.blue - shadowAgx.blue) < 1.0e-5f &&
                    std::abs(shadowStandard.alpha - shadowAgx.alpha) < 1.0e-5f;
                for (const auto &[pixel, linear, transformIndex] : {
                         std::tuple<QRgb, const RasterWorkbenchLinearPixel &, int>{
                             sortedStandardShadowDofOutline.image.pixel(x, y),
                             shadowStandard, 1},
                         std::tuple<QRgb, const RasterWorkbenchLinearPixel &, int>{
                             sortedAgxShadowDofOutline.image.pixel(x, y),
                             shadowAgx, 0}}) {
                    const QVector3D display =
                        workbenchSceneLinearToDisplaySrgb(
                            QVector3D(linear.red / linear.alpha,
                                      linear.green / linear.alpha,
                                      linear.blue / linear.alpha),
                            transformIndex, 0.0f, 1.0f, {}, {});
                    sortedDofDisplayMatches &= pixel == qPremultiply(qRgba(
                        qRound(display.x() * 255.0f),
                        qRound(display.y() * 255.0f),
                        qRound(display.z() * 255.0f),
                        qRound(linear.alpha * 255.0f)));
                }
                const RasterWorkbenchLinearPixel &weightedStandard =
                    weightedStandardShadowDofOutline.layers
                        .sceneLinearColor[index];
                const RasterWorkbenchLinearPixel &weightedAgx =
                    weightedAgxShadowDofOutline.layers
                        .sceneLinearColor[index];
                const RasterWorkbenchLinearPixel &weightedBefore =
                    weightedStandardShadowDofOutline.beforeEffects[index];
                weightedDofChangedScene |= std::abs(
                    weightedStandard.red - weightedBefore.red) > 1.0e-5f ||
                    std::abs(weightedStandard.green - weightedBefore.green) >
                        1.0e-5f ||
                    std::abs(weightedStandard.blue - weightedBefore.blue) >
                        1.0e-5f;
                weightedDofDisplayMatches &=
                    std::abs(weightedStandard.red - weightedAgx.red) <
                        1.0e-5f &&
                    std::abs(weightedStandard.green - weightedAgx.green) <
                        1.0e-5f &&
                    std::abs(weightedStandard.blue - weightedAgx.blue) <
                        1.0e-5f &&
                    std::abs(weightedStandard.alpha - weightedAgx.alpha) <
                        1.0e-5f;
                for (const auto &[pixel, linear, transformIndex] : {
                         std::tuple<QRgb,
                                    const RasterWorkbenchLinearPixel &, int>{
                             weightedStandardShadowDofOutline.image.pixel(x, y),
                             weightedStandard, 1},
                         std::tuple<QRgb,
                                    const RasterWorkbenchLinearPixel &, int>{
                             weightedAgxShadowDofOutline.image.pixel(x, y),
                             weightedAgx, 0}}) {
                    const QVector3D display =
                        workbenchSceneLinearToDisplaySrgb(
                            QVector3D(linear.red / linear.alpha,
                                      linear.green / linear.alpha,
                                      linear.blue / linear.alpha),
                            transformIndex, 0.0f, 1.0f, {}, {});
                    weightedDofDisplayMatches &= pixel == qPremultiply(qRgba(
                        qRound(display.x() * 255.0f),
                        qRound(display.y() * 255.0f),
                        qRound(display.z() * 255.0f),
                        qRound(linear.alpha * 255.0f)));
                }
            }
        }
    } else {
        sortedDofDisplayMatches = false;
    }
    sortedDofRenderer.setShadingSettings(orthographicDofSettings);
    passed &= check(
        sortedStandardDofOutline.rendered && sortedAgxDofOutline.rendered &&
            sortedStandardDofOnly.rendered &&
            sortedStandardShadowDofOutline.rendered &&
            sortedAgxShadowDofOutline.rendered &&
            weightedStandardShadowDofOutline.rendered &&
            weightedAgxShadowDofOutline.rendered &&
            sortedStandardDofOutline.layers.outlineAppliedBeforeDisplay &&
            sortedAgxDofOutline.layers.outlineAppliedBeforeDisplay &&
            sortedStandardShadowDofOutline.layers.outlineAppliedBeforeDisplay &&
            sortedAgxShadowDofOutline.layers.outlineAppliedBeforeDisplay &&
            !sortedStandardDofOutline.layers.sceneLinearDofPending &&
            !sortedAgxDofOutline.layers.sceneLinearDofPending &&
            !sortedStandardShadowDofOutline.layers.sceneLinearDofPending &&
            !sortedAgxShadowDofOutline.layers.sceneLinearDofPending &&
            weightedStandardShadowDofOutline.layers
                    .outlineAppliedBeforeDisplay &&
            weightedAgxShadowDofOutline.layers
                    .outlineAppliedBeforeDisplay &&
            !weightedStandardShadowDofOutline.layers.sceneLinearDofPending &&
            !weightedAgxShadowDofOutline.layers.sceneLinearDofPending &&
            sortedDofChangedScene && sortedDofOutlineChangedScene &&
            sortedShadowChangedScene && sortedDofDisplayMatches &&
            weightedDofChangedScene && weightedDofDisplayMatches,
        "sorted and weighted QPainter shadow-plus-outline-plus-DOF must resolve every effect in scene-linear color before one Standard or AgX display transform");

    // CPU shadow fallback: an opaque elevated quad casts onto an opaque
    // receiver. The input image is deliberately uniform so changed pixels
    // expose only the software shadow pass.
    const auto makeShadowQuad = [&](quint64 id, qreal left, qreal top,
                                    qreal right, qreal bottom, qreal height) {
        ViewportRenderObject object;
        object.shape.geometryType = GeometryType::NurbsSurface;
        object.objectId = ObjectId::fromValue(id);
        auto geometry = QSharedPointer<ViewportDepthGeometry>::create();
        const auto pointAt = [&](qreal x, qreal y) {
            QPointF local;
            orthographicDofTransform.screenToWorkPlaneUnclipped(
                QPointF(x, y), orthographicDofSize, WorkPlane::XY, 0.0,
                &local);
            return QVector3D(float(local.x()), float(local.y()), float(height));
        };
        const QVector3D a = pointAt(left, top);
        const QVector3D b = pointAt(right, top);
        const QVector3D c = pointAt(right, bottom);
        const QVector3D d = pointAt(left, bottom);
        geometry->surfaceVertices = {a, c, b, a, d, c};
        geometry->surfaceNormals = QVector<QVector3D>(
            6, QVector3D(0.0f, 0.0f, 1.0f));
        geometry->surfaceTextureCoordinates = {
            QPointF(0.0, 0.0), QPointF(1.0, 1.0), QPointF(1.0, 0.0),
            QPointF(0.0, 0.0), QPointF(0.0, 1.0), QPointF(1.0, 1.0)};
        object.preparedDepthGeometry = geometry;
        object.shape.nurbsSurface.controlVertexCountU = 2;
        object.shape.nurbsSurface.controlVertexCountV = 2;
        object.shape.nurbsSurface.controlPoints = {
            {a.x(), a.y(), a.z()}, {d.x(), d.y(), d.z()},
            {b.x(), b.y(), b.z()}, {c.x(), c.y(), c.z()}};
        object.shape.nurbsSurface.weights = {1.0, 1.0, 1.0, 1.0};
        object.shape.nurbsSurface.knotsU = {0.0, 1.0};
        object.shape.nurbsSurface.knotsV = {0.0, 1.0};
        return object;
    };
    const QVector<ViewportRenderObject> shadowObjects{
        makeShadowQuad(11, 4, 4, 124, 124, 0.0),
        makeShadowQuad(12, 48, 48, 80, 80, 2.0)};
    const auto renderCpuShadow = [&](ViewportShadingSettings shading) {
        QImage image(orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        shading.mode = ViewportShadingMode::Solid;
        shading.outline = false;
        shading.cavity = false;
        shading.depthOfField = false;
        applyRasterWorkbenchEffects(&image, shadowObjects,
            orthographicDofTransform, orthographicDofSize, 1.0, shading);
        return image;
    };
    ViewportShadingSettings shadowSettings;
    shadowSettings.shadows = true;
    shadowSettings.shadowIntensity = 0.8;
    shadowSettings.shadowFocus = 0.9;
    const QImage shadowImage = renderCpuShadow(shadowSettings);
    ViewportShadingSettings noShadowSettings = shadowSettings;
    noShadowSettings.shadows = false;
    const QImage noShadowImage = renderCpuShadow(noShadowSettings);
    int shadowPixels = 0;
    qreal shadowX = 0.0;
    for (int y = 0; y < shadowImage.height(); ++y) {
        for (int x = 0; x < shadowImage.width(); ++x) {
            if (shadowImage.pixelColor(x, y) != noShadowImage.pixelColor(x, y)) {
                ++shadowPixels;
                shadowX += x;
            }
        }
    }
    passed &= check(shadowPixels > 0,
        "QPainter Workbench shadow setting must darken receiver pixels for opaque triangulated casters");
    const QVector3D shadowFactorLight(0.83516467f, 0.0f, -0.55f);
    const float lowFocusShadowFactor = rasterWorkbenchShadowFactor(
        QVector3D(0.0f, 0.0f, 1.0f), shadowFactorLight,
        1.0f, 0.1f, 0.0f, false);
    const float highFocusShadowFactor = rasterWorkbenchShadowFactor(
        QVector3D(0.0f, 0.0f, 1.0f), shadowFactorLight,
        1.0f, 0.1f, 0.9f, false);
    passed &= check(
        highFocusShadowFactor > lowFocusShadowFactor &&
            rasterWorkbenchShadowFactor(
                QVector3D(0.0f, 0.0f, 1.0f),
                shadowFactorLight, 1.0f, 0.1f, 0.9f, true) ==
                0.0f &&
            rasterWorkbenchShadowFactor(
                QVector3D(0.0f, 0.0f, 1.0f),
                shadowFactorLight, 0.0f, 0.1f, 0.9f, false) ==
                1.0f,
        "QPainter shadow focus must follow Blender's shift/focus smoothstep while shadowed and zero-intensity limits remain stable");
    const float unitShadowBias = rasterWorkbenchShadowDepthBias(-1.0f, 1.0f);
    const float smallShadowBias = rasterWorkbenchShadowDepthBias(-0.001f,
                                                                  0.001f);
    const float largeShadowBias = rasterWorkbenchShadowDepthBias(-1000.0f,
                                                                  1000.0f);
    passed &= check(
        unitShadowBias > 0.0f && smallShadowBias > 0.0f &&
            largeShadowBias > 0.0f &&
            std::abs(smallShadowBias * 1000.0f - unitShadowBias) <
                unitShadowBias * 0.01f &&
            std::abs(largeShadowBias / 1000.0f - unitShadowBias) <
                unitShadowBias * 0.01f &&
            rasterWorkbenchShadowDepthBias(
                std::numeric_limits<float>::quiet_NaN(), 1.0f) == 0.0f,
        "QPainter shadow comparison tolerance must scale with light-depth range and reject non-finite bounds");
    ViewportShadingSettings highShadowFocusSettings = shadowSettings;
    highShadowFocusSettings.shadowFocus = 0.0;
    const QImage highShadowFocusImage = renderCpuShadow(
        highShadowFocusSettings);
    passed &= check(
        highShadowFocusImage != shadowImage,
        "QPainter Workbench shadow focus must change lit receiver shading");

    ViewportRenderObject translucentShadowOverlay =
        makeShadowQuad(13, 4, 4, 124, 124, -0.5);
    translucentShadowOverlay.hasWorkbenchMaterial = true;
    translucentShadowOverlay.workbenchMaterial.alpha = 0.5f;
    translucentShadowOverlay.meshMaterials = {
        translucentShadowOverlay.workbenchMaterial};
    const auto renderShadowThroughTransparency = [&] (
        ViewportShadingSettings settings) {
        QImage image(orthographicDofSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(127, 127, 255));
        RasterWorkbenchSurfaceLayers layers;
        layers.opaqueColor = QImage(
            orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        layers.opaqueColor.fill(Qt::white);
        layers.transparentColor = QImage(
            orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        layers.transparentColor.fill(QColor(0, 0, 255, 128));
        settings.colorMode = ViewportColorMode::Material;
        applyRasterWorkbenchEffects(
            &image,
            {translucentShadowOverlay, shadowObjects[0], shadowObjects[1]},
            orthographicDofTransform, orthographicDofSize, 1.0,
            settings, &layers);
        return image;
    };
    ViewportShadingSettings transparentShadowSettings = shadowSettings;
    const QImage transparentShadowImage = renderShadowThroughTransparency(
        transparentShadowSettings);
    transparentShadowSettings.shadows = false;
    const QImage transparentNoShadowImage = renderShadowThroughTransparency(
        transparentShadowSettings);
    bool shadowVisibleThroughTransparency = false;
    for (int y = 0; y < shadowImage.height() &&
                    !shadowVisibleThroughTransparency; ++y) {
        for (int x = 0; x < shadowImage.width(); ++x) {
            const QColor before = transparentNoShadowImage.pixelColor(x, y);
            const QColor after = transparentShadowImage.pixelColor(x, y);
            const int redDelta = before.red() - after.red();
            const int greenDelta = before.green() - after.green();
            const int blueDelta = before.blue() - after.blue();
            if (shadowImage.pixel(x, y) != noShadowImage.pixel(x, y) &&
                redDelta > 0 && std::abs(redDelta - greenDelta) <= 1 &&
                std::abs(redDelta - blueDelta) <= 1 &&
                before.alpha() == after.alpha()) {
                shadowVisibleThroughTransparency = true;
                break;
            }
        }
    }
    passed &= check(
        shadowVisibleThroughTransparency,
        "QPainter shadows must shade the opaque receiver beneath a retained translucent layer without changing that layer's contribution or alpha");

    ViewportRenderObject translucentShadowReceiver =
        makeShadowQuad(14, 4, 4, 124, 124, 0.5);
    translucentShadowReceiver.hasWorkbenchMaterial = true;
    translucentShadowReceiver.workbenchMaterial.alpha = 0.5f;
    translucentShadowReceiver.meshMaterials = {
        translucentShadowReceiver.workbenchMaterial};
    const auto renderTransparentShadowReceiver = [&] (
        ViewportShadingSettings settings) {
        QImage image(orthographicDofSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(127, 127, 255));
        RasterWorkbenchSurfaceLayers layers;
        layers.opaqueColor = QImage(
            orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        layers.opaqueColor.fill(Qt::white);
        layers.transparentColor = QImage(
            orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        layers.transparentColor.fill(QColor(0, 0, 255, 128));
        settings.colorMode = ViewportColorMode::Material;
        applyRasterWorkbenchEffects(
            &image,
            {translucentShadowReceiver, shadowObjects[0], shadowObjects[1]},
            orthographicDofTransform, orthographicDofSize, 1.0,
            settings, &layers);
        return image;
    };
    const QImage transparentReceiverShadowImage =
        renderTransparentShadowReceiver(shadowSettings);
    ViewportShadingSettings transparentReceiverNoShadowSettings =
        shadowSettings;
    transparentReceiverNoShadowSettings.shadows = false;
    const QImage transparentReceiverNoShadowImage =
        renderTransparentShadowReceiver(transparentReceiverNoShadowSettings);
    bool transparentReceiverWasShadowed = false;
    for (int y = 0; y < transparentReceiverShadowImage.height() &&
                    !transparentReceiverWasShadowed; ++y) {
        for (int x = 0; x < transparentReceiverShadowImage.width(); ++x) {
            if (shadowImage.pixel(x, y) == noShadowImage.pixel(x, y)) continue;
            const QColor before = transparentReceiverNoShadowImage.pixelColor(x, y);
            const QColor after = transparentReceiverShadowImage.pixelColor(x, y);
            const int redDelta = before.red() - after.red();
            const int greenDelta = before.green() - after.green();
            const int blueDelta = before.blue() - after.blue();
            if (redDelta > 0 && greenDelta > 0 && blueDelta > redDelta + 5 &&
                std::abs(redDelta - greenDelta) <= 1 &&
                before.alpha() == after.alpha()) {
                transparentReceiverWasShadowed = true;
                break;
            }
        }
    }
    passed &= check(
        transparentReceiverWasShadowed,
        "QPainter software shadows must darken a frontmost translucent receiver contribution while preserving its alpha");

    // Retain and shadow each transparent layer independently. The fixture
    // searches for a pixel where the lower receiver is shadowed, the upper
    // receiver and opaque underlay are lit, so a top-layer-only factor cannot
    // accidentally satisfy the combined-scene assertion.
    struct TransparentShadowCapture {
        std::vector<RasterWorkbenchLinearPixel> pixels;
        QSize size;
        bool rendered = false;
    };
    const auto captureLayeredTransparentShadow = [&](
        const QVector<ViewportRenderObject> &receivers, bool shadows) {
        ViewportShadingSettings settings = shadowSettings;
        settings.shadows = shadows;
        settings.outline = true;
        settings.outlineColor = QColor(0, 0, 0, 0);
        settings.colorMode = ViewportColorMode::Material;
        settings.lightingMode = ViewportLightingMode::Flat;
        settings.depthOfField = false;
        QVector<ViewportRenderObject> scene;
        ViewportRenderObject opaque = makeShadowQuad(
            35, 4, 4, 124, 124, -0.5);
        opaque.hasWorkbenchMaterial = true;
        opaque.workbenchMaterial.baseColorSceneLinear =
            QVector3D(0.7f, 0.7f, 0.7f);
        opaque.workbenchMaterial.alpha = 1.0f;
        opaque.meshMaterials = {opaque.workbenchMaterial};
        scene.append(opaque);
        for (const ViewportRenderObject &receiver : receivers)
            scene.append(receiver);
        scene.append(shadowObjects[1]);

        CurveHitTester layeredHitTester;
        ViewportRenderer layeredRenderer(orthographicDofTransform,
                                         layeredHitTester);
        layeredRenderer.setShadingSettings(settings);
        const auto drawLayeredObject = [&layeredRenderer,
                                        &orthographicDofSize](
            QPainter &painter, const ViewportRenderObject &object,
            RasterWorkbenchFragmentSink *fragmentSink) {
            layeredRenderer.drawShape(
                painter, object.shape, orthographicDofSize, false, false,
                true, QColor(), QString(), 0.0, object.objectId,
                object.geometryRevision, object.preparedDepthGeometry.data(),
                object.preparedGeometryOffset, object.displayColorSceneLinear,
                object.workbenchMaterial, object.displayName,
                object.libraryPath, object.meshObjectTransform,
                object.meshMaterials, fragmentSink);
        };
        QImage image(orthographicDofSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers layers;
        const bool composited = compositeRasterWorkbenchSurfaces(
            &image, scene, orthographicDofTransform, orthographicDofSize,
            1.0, false, true, settings, QVector3D(0.7f, 0.7f, 0.7f),
            drawLayeredObject, &layers);
        if (composited) {
            applyRasterWorkbenchEffects(
                &image, scene, orthographicDofTransform,
                orthographicDofSize, 1.0, settings, &layers);
        }
        return TransparentShadowCapture{
            std::move(layers.sceneLinearColor), layers.sceneLinearSize,
            composited};
    };
    ViewportRenderObject rearShadowReceiver = makeShadowQuad(
        36, 4, 4, 124, 124, 0.0);
    rearShadowReceiver.hasWorkbenchMaterial = true;
    rearShadowReceiver.workbenchMaterial.baseColorSceneLinear =
        QVector3D(0.05f, 0.85f, 0.1f);
    rearShadowReceiver.workbenchMaterial.alpha = 0.5f;
    rearShadowReceiver.meshMaterials = {rearShadowReceiver.workbenchMaterial};
    ViewportRenderObject frontShadowReceiver = makeShadowQuad(
        37, 4, 4, 124, 124, 0.5);
    frontShadowReceiver.hasWorkbenchMaterial = true;
    frontShadowReceiver.workbenchMaterial.baseColorSceneLinear =
        QVector3D(0.85f, 0.05f, 0.1f);
    frontShadowReceiver.workbenchMaterial.alpha = 0.5f;
    frontShadowReceiver.meshMaterials = {frontShadowReceiver.workbenchMaterial};
    const TransparentShadowCapture opaqueShadowLayers =
        captureLayeredTransparentShadow({}, true);
    const TransparentShadowCapture opaqueLitLayers =
        captureLayeredTransparentShadow({}, false);
    const TransparentShadowCapture rearShadowLayers =
        captureLayeredTransparentShadow({rearShadowReceiver}, true);
    const TransparentShadowCapture rearLitLayers =
        captureLayeredTransparentShadow({rearShadowReceiver}, false);
    const TransparentShadowCapture frontShadowLayers =
        captureLayeredTransparentShadow({frontShadowReceiver}, true);
    const TransparentShadowCapture frontLitLayers =
        captureLayeredTransparentShadow({frontShadowReceiver}, false);
    const TransparentShadowCapture stackedShadowLayers =
        captureLayeredTransparentShadow(
            {rearShadowReceiver, frontShadowReceiver}, true);
    const TransparentShadowCapture stackedLitLayers =
        captureLayeredTransparentShadow(
            {rearShadowReceiver, frontShadowReceiver}, false);
    bool rearOnlyShadowPixel = false;
    bool stackedAlphaUnchanged = false;
    bool opaqueShadowVisibleThroughStack = false;
    const int layeredPixelCount = orthographicDofSize.width() *
                                  orthographicDofSize.height();
    if (opaqueShadowLayers.rendered && opaqueLitLayers.rendered &&
        rearShadowLayers.rendered && rearLitLayers.rendered &&
        frontShadowLayers.rendered && frontLitLayers.rendered &&
        stackedShadowLayers.rendered && stackedLitLayers.rendered &&
        opaqueShadowLayers.pixels.size() ==
            static_cast<std::size_t>(layeredPixelCount) &&
        rearShadowLayers.pixels.size() ==
            static_cast<std::size_t>(layeredPixelCount) &&
        frontShadowLayers.pixels.size() ==
            static_cast<std::size_t>(layeredPixelCount) &&
        stackedShadowLayers.pixels.size() ==
            static_cast<std::size_t>(layeredPixelCount)) {
        const auto colorChangedAt = [](const TransparentShadowCapture &a,
                                       const TransparentShadowCapture &b,
                                       std::size_t index) {
            return std::abs(a.pixels[index].red - b.pixels[index].red) >
                       1.0e-4f ||
                   std::abs(a.pixels[index].green - b.pixels[index].green) >
                       1.0e-4f ||
                   std::abs(a.pixels[index].blue - b.pixels[index].blue) >
                       1.0e-4f;
        };
        for (std::size_t index = 0; index <
             static_cast<std::size_t>(layeredPixelCount); ++index) {
            if (colorChangedAt(opaqueShadowLayers, opaqueLitLayers, index) &&
                colorChangedAt(stackedShadowLayers, stackedLitLayers, index) &&
                std::abs(stackedShadowLayers.pixels[index].alpha -
                         stackedLitLayers.pixels[index].alpha) < 1.0e-5f) {
                opaqueShadowVisibleThroughStack = true;
            }
            if (colorChangedAt(opaqueShadowLayers, opaqueLitLayers, index) ||
                !colorChangedAt(rearShadowLayers, rearLitLayers, index) ||
                colorChangedAt(frontShadowLayers, frontLitLayers, index)) {
                continue;
            }
            rearOnlyShadowPixel = true;
            stackedAlphaUnchanged = std::abs(
                stackedShadowLayers.pixels[index].alpha -
                stackedLitLayers.pixels[index].alpha) < 1.0e-5f;
            rearOnlyShadowPixel &= colorChangedAt(
                stackedShadowLayers, stackedLitLayers, index);
            break;
        }
    }
    passed &= check(
        rearOnlyShadowPixel && stackedAlphaUnchanged,
        "QPainter stacked transparent receivers must shade a shadowed rear layer independently of a lit front layer while preserving final alpha and the lit opaque underlay");
    passed &= check(
        opaqueShadowVisibleThroughStack,
        "QPainter stacked transparent receivers must preserve shadowing of the opaque underlay through transparency without changing final alpha");

    const auto renderTransparentCasterShadow = [&](float casterAlpha,
                                                    bool includeCaster) {
        ViewportRenderObject receiver = makeShadowQuad(25, 4, 4, 124, 124, 0.0);
        receiver.hasWorkbenchMaterial = true;
        receiver.workbenchMaterial.alpha = 0.5f;
        receiver.meshMaterials = {receiver.workbenchMaterial};
        QVector<ViewportRenderObject> objects{receiver};
        if (includeCaster) {
            ViewportRenderObject caster =
                makeShadowQuad(26, 48, 48, 80, 80, 2.0);
            caster.hasWorkbenchMaterial = true;
            caster.workbenchMaterial.alpha = casterAlpha;
            caster.meshMaterials = {caster.workbenchMaterial};
            objects.append(caster);
        }
        ViewportShadingSettings settings = shadowSettings;
        settings.colorMode = ViewportColorMode::Material;
        QImage image(orthographicDofSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(127, 127, 255));
        RasterWorkbenchSurfaceLayers layers;
        layers.opaqueColor = QImage(
            orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        layers.opaqueColor.fill(Qt::white);
        layers.transparentColor = QImage(
            orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
        layers.transparentColor.fill(QColor(0, 0, 255, 128));
        applyRasterWorkbenchEffects(
            &image, objects, orthographicDofTransform,
            orthographicDofSize, 1.0, settings, &layers);
        return image;
    };
    const QImage noTransparentCasterImage =
        renderTransparentCasterShadow(1.0f, false);
    const QImage transparentCasterImage =
        renderTransparentCasterShadow(0.5f, true);
    const QImage lowAlphaCasterImage =
        renderTransparentCasterShadow(0.25f, true);
    bool transparentCasterCastsBeyondItsVisibleFootprint = false;
    bool transparentCasterAlphaDoesNotScaleShadow = true;
    for (int y = 4; y < transparentCasterImage.height(); ++y) {
        for (int x = 4; x < transparentCasterImage.width(); ++x) {
            if (x >= 40 && x < 88 && y >= 40 && y < 88) continue;
            const QRgb withoutCaster = noTransparentCasterImage.pixel(x, y);
            const QRgb transparent = transparentCasterImage.pixel(x, y);
            if (transparent != withoutCaster)
                transparentCasterCastsBeyondItsVisibleFootprint = true;
            if (transparent != lowAlphaCasterImage.pixel(x, y))
                transparentCasterAlphaDoesNotScaleShadow = false;
        }
    }
    passed &= check(
        transparentCasterCastsBeyondItsVisibleFootprint,
        "QPainter transparent-material geometry must cast a binary shadow onto a retained translucent receiver");
    passed &= check(
        transparentCasterAlphaDoesNotScaleShadow,
        "QPainter transparent caster material alpha must not scale Blender's binary shadow-volume mask");
    ViewportShadingSettings zeroShadowSettings = shadowSettings;
    zeroShadowSettings.shadowIntensity = 0.0;
    passed &= check(renderCpuShadow(zeroShadowSettings) == noShadowImage,
        "QPainter Workbench shadow intensity zero must preserve the unshadowed image");
    ViewportShadingSettings lowShadowSettings = shadowSettings;
    lowShadowSettings.shadowIntensity = 0.2;
    const QImage lowIntensityShadowImage = renderCpuShadow(lowShadowSettings);
    qint64 highIntensityLuminance = 0;
    qint64 lowIntensityLuminance = 0;
    for (int y = 0; y < shadowImage.height(); ++y) {
        for (int x = 0; x < shadowImage.width(); ++x) {
            highIntensityLuminance += shadowImage.pixelColor(x, y).red();
            lowIntensityLuminance +=
                lowIntensityShadowImage.pixelColor(x, y).red();
        }
    }
    passed &= check(highIntensityLuminance < lowIntensityLuminance,
        "QPainter Workbench shadow darkening must increase monotonically with shadow intensity");
    ViewportShadingSettings alternateShadowSettings = shadowSettings;
    alternateShadowSettings.shadowDirection = QVector3D(1.0f, 0.2f, 0.1f);
    const QImage alternateShadowImage = renderCpuShadow(alternateShadowSettings);
    qreal alternateShadowX = 0.0;
    int alternateShadowPixels = 0;
    for (int y = 0; y < shadowImage.height(); ++y) {
        for (int x = 0; x < shadowImage.width(); ++x) {
            if (alternateShadowImage.pixelColor(x, y) !=
                noShadowImage.pixelColor(x, y)) {
                ++alternateShadowPixels;
                alternateShadowX += x;
            }
        }
    }
    passed &= check(alternateShadowPixels > 0 &&
                        std::abs(shadowX / std::max(shadowPixels, 1) -
                                 alternateShadowX /
                                     std::max(alternateShadowPixels, 1)) > 1.0,
        "QPainter Workbench shadow direction must move the software shadow mask");
    QTemporaryDir shadowTextureDirectory;
    QImage transparentShadowTexture(1, 1, QImage::Format_ARGB32);
    transparentShadowTexture.fill(Qt::transparent);
    const QString transparentShadowTexturePath =
        shadowTextureDirectory.filePath(QStringLiteral("transparent.png"));
    const bool transparentShadowTextureSaved =
        transparentShadowTexture.save(transparentShadowTexturePath);
    QVector<ViewportRenderObject> cutoutShadowObjects{
        makeShadowQuad(21, 4, 4, 124, 124, 0.0),
        makeShadowQuad(22, 48, 48, 80, 80, 2.0)};
    cutoutShadowObjects[1].hasWorkbenchMaterial = true;
    cutoutShadowObjects[1].workbenchMaterial.textureImagePath =
        transparentShadowTexturePath;
    cutoutShadowObjects[1].workbenchMaterial.textureImageAlphaMode =
        QStringLiteral("STRAIGHT");
    ViewportShadingSettings cutoutShadowSettings = shadowSettings;
    cutoutShadowSettings.colorMode = ViewportColorMode::Texture;
    QImage cutoutShadowImage(orthographicDofSize,
                             QImage::Format_ARGB32_Premultiplied);
    cutoutShadowImage.fill(Qt::white);
    applyRasterWorkbenchEffects(&cutoutShadowImage, cutoutShadowObjects,
        orthographicDofTransform, orthographicDofSize, 1.0,
        cutoutShadowSettings);
    QImage receiverOnlyImage(orthographicDofSize,
                             QImage::Format_ARGB32_Premultiplied);
    receiverOnlyImage.fill(Qt::white);
    const QVector<ViewportRenderObject> receiverOnlyObjects{
        cutoutShadowObjects[0]};
    applyRasterWorkbenchEffects(&receiverOnlyImage, receiverOnlyObjects,
        orthographicDofTransform, orthographicDofSize, 1.0,
        cutoutShadowSettings);
    int transparentTextureCasterShadowPixels = 0;
    for (int y = 0; y < cutoutShadowImage.height(); ++y) {
        for (int x = 0; x < cutoutShadowImage.width(); ++x) {
            transparentTextureCasterShadowPixels +=
                cutoutShadowImage.pixelColor(x, y) !=
                receiverOnlyImage.pixelColor(x, y);
        }
    }
    passed &= check(transparentShadowTextureSaved &&
                        transparentTextureCasterShadowPixels > 0,
        "QPainter software shadows must use opaque caster geometry despite transparent Image Texture alpha");
    ViewportTransform translatedOrthographicDofTransform =
        orthographicDofTransform;
    const Point3D translatedDofTarget{
        orthographicDofTarget.x, orthographicDofTarget.y,
        orthographicDofTarget.z - 20.0};
    translatedOrthographicDofTransform.setOrbitPivotPreservingView(
        translatedDofTarget);
    QVector<ViewportRenderObject> translatedOrthographicDofObjects =
        orthographicDofObjects;
    for (ViewportRenderObject &object : translatedOrthographicDofObjects) {
        auto translatedGeometry = QSharedPointer<ViewportDepthGeometry>::create(
            *object.preparedDepthGeometry);
        for (QVector3D &point : translatedGeometry->surfaceVertices) {
            point.setZ(point.z() - 20.0f);
        }
        object.preparedDepthGeometry = translatedGeometry;
    }
    QImage translatedOrthographicDofImage(
        orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
    translatedOrthographicDofImage.fill(Qt::black);
    translatedOrthographicDofImage.setPixelColor(32, 64, Qt::red);
    translatedOrthographicDofImage.setPixelColor(96, 64, Qt::red);
    applyRasterWorkbenchEffects(
        &translatedOrthographicDofImage,
        translatedOrthographicDofObjects,
        translatedOrthographicDofTransform, orthographicDofSize, 1.0,
        orthographicDofSettings);
    passed &= check(
        focusedOrthographicRed == 255 && defocusedOrthographicRed > 0 &&
            defocusedOrthographicRed < 240 &&
            subpixelCocImage == subpixelCocSharpImage &&
            translatedOrthographicDofImage == orthographicDofImage,
        "orthographic scene-camera DOF must preserve an in-focus impulse, leave a 0.8-pixel-CoC image sharp, gather a defocused surface, and remain invariant when the view target and scene translate together");
    ViewportTransform largeCocDofTransform = orthographicDofTransform;
    ViewportCameraPreferences largeCocDofOptics = orthographicDofOptics;
    largeCocDofOptics.opticalSensorSizeMillimeters = 3.0;
    const bool largeCocDofOpticsSet =
        largeCocDofTransform.setCameraPreferences(largeCocDofOptics);
    QImage largeCocImpulseImage(orthographicDofSize,
                                QImage::Format_ARGB32_Premultiplied);
    largeCocImpulseImage.fill(Qt::black);
    largeCocImpulseImage.setPixelColor(96, 64, Qt::red);
    applyRasterWorkbenchEffects(
        &largeCocImpulseImage, {orthographicDofObjects[1]},
        largeCocDofTransform, orthographicDofSize, 1.0,
        orthographicDofSettings);
    QImage largeCocPatchImage(orthographicDofSize,
                              QImage::Format_ARGB32_Premultiplied);
    largeCocPatchImage.fill(Qt::black);
    for (int y = 60; y < 68; ++y) {
        for (int x = 92; x < 100; ++x)
            largeCocPatchImage.setPixelColor(x, y, Qt::red);
    }
    applyRasterWorkbenchEffects(
        &largeCocPatchImage, {orthographicDofObjects[1]},
        largeCocDofTransform, orthographicDofSize, 1.0,
        orthographicDofSettings);
    bool largeCocMipGatherSpillsBeyondTwelvePixels = false;
    for (int y = 48; y <= 80; ++y) {
        for (int x = 109; x < largeCocPatchImage.width(); ++x) {
            largeCocMipGatherSpillsBeyondTwelvePixels |=
                largeCocPatchImage.pixelColor(x, y).red() > 0;
        }
    }
    passed &= check(largeCocDofOpticsSet &&
                        largeCocImpulseImage.pixelColor(96, 64).red() <= 5,
                    "QPainter Workbench DOF median cleanup must suppress an isolated impulse with a large CoC");
    passed &= check(largeCocMipGatherSpillsBeyondTwelvePixels,
                    "QPainter Workbench DOF mip gather must spill a large CoC beyond the former 12-pixel cap");

    const auto makeOutlineScaleObject = [&]() {
        ViewportRenderObject object;
        object.shape.geometryType = GeometryType::NurbsSurface;
        object.objectId = ObjectId::fromValue(71);
        auto geometry = QSharedPointer<ViewportDepthGeometry>::create();
        const auto worldAt = [&](qreal x, qreal y) {
            QPointF local;
            orthographicDofTransform.screenToWorkPlaneUnclipped(
                QPointF(x, y), orthographicDofSize, WorkPlane::XY, 0.0,
                &local);
            return QVector3D(float(local.x()), float(local.y()),
                             float(orthographicDofTarget.z));
        };
        const QVector3D a = worldAt(32.0, 32.0);
        const QVector3D b = worldAt(96.0, 32.0);
        const QVector3D c = worldAt(96.0, 96.0);
        const QVector3D d = worldAt(32.0, 96.0);
        // screenToWorkPlane maps screen-down to decreasing world Y. Keep the
        // triangles counter-clockwise from Top so their +Z normals are front
        // facing, matching Blender's gl_FrontFacing prepass semantics.
        geometry->surfaceVertices = {a, c, b, a, d, c};
        geometry->surfaceNormals =
            QVector<QVector3D>(6, QVector3D(0.0f, 0.0f, 1.0f));
        object.preparedDepthGeometry = geometry;
        return object;
    };
    ViewportShadingSettings outlineScaleSettings;
    outlineScaleSettings.mode = ViewportShadingMode::Solid;
    outlineScaleSettings.outline = true;
    outlineScaleSettings.outlineColor = QColor(255, 0, 0);
    const ViewportRenderObject outlineScaleObject = makeOutlineScaleObject();
    QImage outlineScaleDpr1(orthographicDofSize,
                            QImage::Format_ARGB32_Premultiplied);
    outlineScaleDpr1.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &outlineScaleDpr1, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, outlineScaleSettings);
    QImage outlineScaleDpr2(orthographicDofSize * 2,
                            QImage::Format_ARGB32_Premultiplied);
    outlineScaleDpr2.setDevicePixelRatio(2.0);
    outlineScaleDpr2.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &outlineScaleDpr2, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 2.0, outlineScaleSettings);
    passed &= check(
        workbenchEffectUiScale(1.5) == 2 &&
            workbenchEffectUiScale(1.0) == 1 &&
            workbenchEffectUiScale(2.0) == 2,
        "Workbench GPU and QPainter cardinal ID taps must use the nearest integer UI scale (DPR 1.5 maps to 2)");
    passed &= check(
        std::abs(workbenchEffectFractionalUiScale(1.5) - 1.5f) < 1.0e-6f &&
            std::abs(workbenchEffectFractionalUiScale(1.25) - 1.25f) <
                1.0e-6f,
        "QPainter curvature taps must preserve fractional DPR offsets");

    QImage outlineScaleDpr15(orthographicDofSize * 3 / 2,
                             QImage::Format_ARGB32_Premultiplied);
    outlineScaleDpr15.setDevicePixelRatio(1.5);
    outlineScaleDpr15.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &outlineScaleDpr15, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.5, outlineScaleSettings);
    passed &= check(
        outlineScaleDpr15.pixelColor(46, 96).red() > 40,
        "QPainter outline at DPR 1.5 must sample two device pixels from the edge, matching integer Workbench UI scale");

    passed &= check(
        outlineScaleDpr1.pixelColor(31, 64).red() > 40 &&
            outlineScaleDpr2.pixelColor(62, 128).red() > 40,
        "QPainter outlines must sample cardinal object IDs at the integer Blender UI footprint");

    ViewportRenderObject selectedOutlineObject = outlineScaleObject;
    selectedOutlineObject.selected = true;
    QImage selectedOutlineImage(orthographicDofSize,
                                QImage::Format_ARGB32_Premultiplied);
    selectedOutlineImage.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &selectedOutlineImage, {selectedOutlineObject},
        orthographicDofTransform, orthographicDofSize, 1.0,
        outlineScaleSettings);
    passed &= check(
        selectedOutlineImage == outlineScaleDpr1,
        "QPainter Workbench outline color must stay uniform for selected and unselected object IDs");

    ViewportShadingSettings xrayOutlineSettings = outlineScaleSettings;
    xrayOutlineSettings.xray = true;
    xrayOutlineSettings.xrayAlpha = 0.5;
    QImage xrayOutlineImage(orthographicDofSize,
                            QImage::Format_ARGB32_Premultiplied);
    xrayOutlineImage.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &xrayOutlineImage, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, xrayOutlineSettings);
    passed &= check(
        xrayOutlineSettings.xrayEnabled() &&
            xrayOutlineImage.pixelColor(31, 64).red() > 40,
        "QPainter X-Ray must retain Workbench object outlines");

    ViewportRenderObject inFrontOutlineObject = makeOutlineScaleObject();
    inFrontOutlineObject.objectId = ObjectId::fromValue(72);
    inFrontOutlineObject.showInFront = true;
    auto inFrontOutlineGeometry =
        QSharedPointer<ViewportDepthGeometry>::create();
    const auto outlineWorldAt = [&](qreal x, qreal y) {
        QPointF local;
        orthographicDofTransform.screenToWorkPlaneUnclipped(
            QPointF(x, y), orthographicDofSize, WorkPlane::XY, 0.0, &local);
        return QVector3D(float(local.x()), float(local.y()),
                         float(orthographicDofTarget.z - 0.5));
    };
    const QVector3D inFrontA = outlineWorldAt(48.0, 48.0);
    const QVector3D inFrontB = outlineWorldAt(80.0, 48.0);
    const QVector3D inFrontC = outlineWorldAt(80.0, 80.0);
    const QVector3D inFrontD = outlineWorldAt(48.0, 80.0);
    inFrontOutlineGeometry->surfaceVertices = {
        inFrontA, inFrontC, inFrontB, inFrontA, inFrontD, inFrontC};
    inFrontOutlineGeometry->surfaceNormals =
        QVector<QVector3D>(6, QVector3D(0.0f, 0.0f, 1.0f));
    inFrontOutlineObject.preparedDepthGeometry = inFrontOutlineGeometry;
    QImage regularOutlineImage(orthographicDofSize,
                               QImage::Format_ARGB32_Premultiplied);
    regularOutlineImage.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &regularOutlineImage, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, outlineScaleSettings);
    QImage inFrontOutlineImage(orthographicDofSize,
                               QImage::Format_ARGB32_Premultiplied);
    inFrontOutlineImage.fill(Qt::black);
    applyRasterWorkbenchEffects(
        &inFrontOutlineImage, {outlineScaleObject, inFrontOutlineObject},
        orthographicDofTransform, orthographicDofSize, 1.0,
        outlineScaleSettings);
    passed &= check(
        regularOutlineImage.pixelColor(47, 64) == QColor(Qt::black) &&
            inFrontOutlineImage.pixelColor(47, 64).red() > 20,
        "QPainter effect ID selection must let an in-front surface define the outline boundary even when regular geometry is geometrically nearer");

    ViewportShadingSettings cavityOnlySettings = outlineScaleSettings;
    cavityOnlySettings.outline = false;
    cavityOnlySettings.cavity = true;
    cavityOnlySettings.cavityType = ViewportCavityType::World;
    cavityOnlySettings.cavityDistance = 0.5;
    cavityOnlySettings.cavitySamples = 32;
    QImage cavityOnlyImage(orthographicDofSize,
                           QImage::Format_ARGB32_Premultiplied);
    cavityOnlyImage.fill(QColor(120, 120, 120));
    applyRasterWorkbenchEffects(
        &cavityOnlyImage, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, cavityOnlySettings);
    ViewportShadingSettings cavityOutlineSettings = cavityOnlySettings;
    cavityOutlineSettings.outline = true;
    QImage cavityOutlineImage(orthographicDofSize,
                              QImage::Format_ARGB32_Premultiplied);
    cavityOutlineImage.fill(QColor(120, 120, 120));
    applyRasterWorkbenchEffects(
        &cavityOutlineImage, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, cavityOutlineSettings);
    bool cavityChanged = false;
    bool outlineVisibleOverCavity = false;
    for (int y = 0; y < orthographicDofSize.height(); ++y) {
        for (int x = 0; x < orthographicDofSize.width(); ++x) {
            const QColor cavityPixel = cavityOnlyImage.pixelColor(x, y);
            cavityChanged = cavityChanged ||
                            cavityPixel != QColor(120, 120, 120);
            outlineVisibleOverCavity = outlineVisibleOverCavity ||
                cavityOutlineImage.pixelColor(x, y).red() > cavityPixel.red();
        }
    }
    passed &= check(cavityChanged && outlineVisibleOverCavity,
        "QPainter applies cavity before the object outline so the outline remains visible over cavity-shaded surfaces");

    std::vector<float> regularDomainCavityFactors;
    std::vector<float> inFrontDomainCavityFactors;
    QImage regularFactorProbe(orthographicDofSize,
                              QImage::Format_ARGB32_Premultiplied);
    regularFactorProbe.fill(Qt::white);
    QImage inFrontFactorProbe(orthographicDofSize,
                              QImage::Format_ARGB32_Premultiplied);
    inFrontFactorProbe.fill(Qt::white);
    applyRasterWorkbenchEffects(
        &regularFactorProbe, {outlineScaleObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, cavityOnlySettings, nullptr,
        &regularDomainCavityFactors);
    applyRasterWorkbenchEffects(
        &inFrontFactorProbe, {inFrontOutlineObject}, orthographicDofTransform,
        orthographicDofSize, 1.0, cavityOnlySettings, nullptr,
        &inFrontDomainCavityFactors);
    bool distinctDomainFactors = false;
    if (regularDomainCavityFactors.size() ==
            inFrontDomainCavityFactors.size()) {
        for (std::size_t index = 0;
             index < regularDomainCavityFactors.size(); ++index) {
            if (std::abs(regularDomainCavityFactors[index] -
                         inFrontDomainCavityFactors[index]) > 1.0e-5f) {
                distinctDomainFactors = true;
                break;
            }
        }
    }
    passed &= check(
        regularDomainCavityFactors.size() ==
                static_cast<std::size_t>(orthographicDofSize.width() *
                                         orthographicDofSize.height()) &&
            inFrontDomainCavityFactors.size() ==
                regularDomainCavityFactors.size() && distinctDomainFactors,
        "QPainter regular and in-front opaque geometry must produce independent cavity factor maps");

    // Keep the transparency fixture independent of an OpenGL context so it
    // can be captured and compared in the offscreen contract run.
    {
        const QSize size(640, 480);
        ViewportTransform transform;
        transform.setViewPreset(ViewportViewPreset::Top);
        transform.setPerspectiveEnabled(false);
        const auto worldAtScreen = [&](qreal x, qreal y, qreal z) {
            QPointF local;
            transform.screenToWorkPlaneUnclipped(QPointF(x, y), size,
                                                 WorkPlane::XY, z, &local);
            return QVector3D(float(local.x()), float(local.y()), float(z));
        };
        const auto makeQuad = [&](quint64 id, qreal left, qreal top,
                                  qreal right, qreal bottom, qreal z,
                                  const QVector3D &color, qreal alpha) {
            ViewportRenderObject object;
            object.shape.geometryType = GeometryType::PolygonMesh;
            const QVector3D a = worldAtScreen(left, bottom, z);
            const QVector3D b = worldAtScreen(right, bottom, z);
            const QVector3D c = worldAtScreen(right, top, z);
            const QVector3D d = worldAtScreen(left, top, z);
            object.shape.polygonMesh.vertices = {
                Point3D{a.x(), a.y(), a.z()}, Point3D{b.x(), b.y(), b.z()},
                Point3D{c.x(), c.y(), c.z()}, Point3D{d.x(), d.y(), d.z()}};
            object.shape.polygonMesh.faces = {{0, 1, 2, 3}};
            object.shape.polygonMesh.faceMaterialIndices = {0};
            object.objectId = ObjectId::fromValue(id);
            object.geometryRevision = 1;
            object.hasWorkbenchMaterial = true;
            object.workbenchMaterial.baseColorSceneLinear = color;
            object.workbenchMaterial.alpha = alpha;
            object.meshMaterials = {object.workbenchMaterial};
            auto geometry = QSharedPointer<ViewportDepthGeometry>::create();
            geometry->surfaceVertices = {a, b, c, a, c, d};
            geometry->surfaceNormals = QVector<QVector3D>(6, QVector3D(0, 0, 1));
            object.preparedDepthGeometry = geometry;
            return object;
        };
        QVector<ViewportRenderObject> objects{
            makeQuad(9101, 134.4, 86.4, 441.6, 393.6, 0.0,
                    QVector3D(0.08f, 0.25f, 0.95f), 0.55),
            makeQuad(9102, 224, 112, 480, 368, 0.0,
                    QVector3D(0.95f, 0.12f, 0.04f), 0.48)};
        ViewportShadingSettings shading;
        shading.lightingMode = ViewportLightingMode::Flat;
        shading.colorMode = ViewportColorMode::Material;
        shading.colorTransform = 1; // Match the Blender Standard reference.
        shading.outline = false;
        shading.shadows = false;
        shading.cavity = false;
        const QVector3D backgroundLinear(0.12f, 0.07f, 0.035f);
        const QVector3D backgroundDisplay = workbenchSceneLinearToDisplaySrgb(
            backgroundLinear, shading.colorTransform, shading.colorExposure,
            shading.colorGamma, shading.colorLutData,
            shading.colorLutMetadata);
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        const QColor background = QColor::fromRgbF(
            backgroundDisplay.x(), backgroundDisplay.y(),
            backgroundDisplay.z());
        image.fill(Qt::transparent);
        CurveHitTester transparencyHitTester;
        ViewportRenderer renderer(transform, transparencyHitTester);
        renderer.setShadingSettings(shading);
        const auto drawObject = [&renderer, &size](
                                    QPainter &painter,
                                    const ViewportRenderObject &object,
                                    RasterWorkbenchFragmentSink *fragmentSink) {
            renderer.drawShape(
                painter, object.shape, size, false, false, true, QColor(),
                QString(), 0.0, object.objectId, object.geometryRevision,
                object.preparedDepthGeometry.data(),
                object.preparedGeometryOffset, object.displayColorSceneLinear,
                object.workbenchMaterial, object.displayName,
                object.libraryPath, object.meshObjectTransform,
                object.meshMaterials, fragmentSink);
        };
        RasterWorkbenchSurfaceLayers layers;
        const bool rendered = compositeRasterWorkbenchSurfaces(
            &image, objects, transform, size, 1.0, false, true, shading,
            backgroundLinear, drawObject, &layers);
        QVector<ViewportRenderObject> generatedDepthObjects = objects;
        for (ViewportRenderObject &object : generatedDepthObjects) {
            object.preparedDepthGeometry.clear();
        }
        QImage generatedDepthImage(size, QImage::Format_ARGB32_Premultiplied);
        generatedDepthImage.fill(Qt::transparent);
        const bool generatedDepthRendered = compositeRasterWorkbenchSurfaces(
            &generatedDepthImage, generatedDepthObjects, transform, size, 1.0,
            false, true, shading, backgroundLinear, drawObject, nullptr, {},
            true);
        QImage rejectedMissingDepthImage(
            size, QImage::Format_ARGB32_Premultiplied);
        rejectedMissingDepthImage.fill(Qt::transparent);
        const bool rejectedMissingDepthRendered =
            compositeRasterWorkbenchSurfaces(
                &rejectedMissingDepthImage, generatedDepthObjects, transform,
                size, 1.0, false, true, shading, backgroundLinear, drawObject);
        passed &= check(rendered && generatedDepthRendered &&
                            !rejectedMissingDepthRendered &&
                            generatedDepthImage == image,
                        "native-failure compositor may derive bounded depth from scene-linear fragments when prepared geometry is absent");

        ViewportShadingSettings generatedDepthAgxSettings = shading;
        generatedDepthAgxSettings.colorTransform = 0;
        renderer.setShadingSettings(generatedDepthAgxSettings);
        QImage preparedAgxImage(size, QImage::Format_ARGB32_Premultiplied);
        preparedAgxImage.fill(Qt::transparent);
        const bool preparedAgxRendered = compositeRasterWorkbenchSurfaces(
            &preparedAgxImage, objects, transform, size, 1.0, false, true,
            generatedDepthAgxSettings, backgroundLinear, drawObject);
        QImage generatedAgxImage(size, QImage::Format_ARGB32_Premultiplied);
        generatedAgxImage.fill(Qt::transparent);
        const bool generatedAgxRendered = compositeRasterWorkbenchSurfaces(
            &generatedAgxImage, generatedDepthObjects, transform, size, 1.0,
            false, true, generatedDepthAgxSettings, backgroundLinear,
            drawObject, nullptr, {}, true);
        passed &= check(preparedAgxRendered && generatedAgxRendered &&
                            generatedAgxImage == preparedAgxImage,
                        "fragment-derived fallback depth must preserve scene-linear transparency under AgX");
        renderer.setShadingSettings(shading);

        ScalarVolume3D sliceVolume;
        sliceVolume.name = QStringLiteral("qpaint slice density");
        sliceVolume.activeBounds = {-1, -1, -1, 0, 0, 0};
        sliceVolume.expandedVoxelCount = 8;
        sliceVolume.denseVoxelCount = 8;
        sliceVolume.activeRecords.append({-1, -1, -1, 2, true, 1.0f});
        sliceVolume.useSlice = true;
        sliceVolume.sliceAxis = QStringLiteral("Z");
        sliceVolume.sliceDepth = 0.5f;
        sliceVolume.interpolationMethod = QStringLiteral("LINEAR");
        sliceVolume.densityScale = 1.0f;
        ViewportRenderObject sliceVolumeObject;
        sliceVolumeObject.shape.geometryType = GeometryType::ScalarVolume;
        sliceVolumeObject.shape.scalarVolume = sliceVolume;
        sliceVolumeObject.objectId = ObjectId::fromValue(9110);
        sliceVolumeObject.displayColorSceneLinear =
            QVector4D(0.8f, 0.12f, 0.03f, 1.0f);
        ViewportShadingSettings volumeSliceSettings = shading;
        volumeSliceSettings.colorMode = ViewportColorMode::Object;
        QImage volumeSliceImage(size, QImage::Format_ARGB32_Premultiplied);
        volumeSliceImage.fill(Qt::black);
        const QVector<ViewportRenderObject> noSurfaceObjects;
        const QVector<ViewportRenderObject> volumeObjects{sliceVolumeObject};
        const bool volumeSliceRendered = compositeRasterWorkbenchSurfaces(
            &volumeSliceImage, noSurfaceObjects, transform, size, 1.0,
            false, true, volumeSliceSettings, QVector3D(0.0f, 0.0f, 0.0f),
            drawObject, nullptr, {}, false, volumeObjects);
        int volumeSliceChangedPixels = 0;
        for (int y = 0; y < volumeSliceImage.height(); ++y) {
            for (int x = 0; x < volumeSliceImage.width(); ++x) {
                const QColor pixel = volumeSliceImage.pixelColor(x, y);
                volumeSliceChangedPixels += pixel.red() > 0 ||
                                            pixel.green() > 0 ||
                                            pixel.blue() > 0;
            }
        }
        passed &= check(volumeSliceRendered && volumeSliceChangedPixels > 0,
                        "QPainter Workbench fallback composites enabled scalar-volume slice samples in scene linear color");

        ScalarVolume3D rotatedAnisotropicSlice = sliceVolume;
        rotatedAnisotropicSlice.activeBounds = {-1, -1, -2, 0, 0, 1};
        rotatedAnisotropicSlice.expandedVoxelCount = 16;
        rotatedAnisotropicSlice.denseVoxelCount = 16;
        rotatedAnisotropicSlice.activeRecords.clear();
        for (int z = -2; z <= 1; ++z) {
            for (int y = -1; y <= 0; ++y) {
                for (int x = -1; x <= 0; ++x) {
                    rotatedAnisotropicSlice.activeRecords.append(
                        {x, y, z, 1, false, 1.0f});
                }
            }
        }
        rotatedAnisotropicSlice.sliceAxis = QStringLiteral("X");
        ViewportRenderObject rotatedAnisotropicObject = sliceVolumeObject;
        rotatedAnisotropicObject.shape.scalarVolume = rotatedAnisotropicSlice;
        rotatedAnisotropicObject.shape.scalarVolume.objectToWorld = {
            0.0, 0.0, -1.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            1.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0};
        QVector<RasterWorkbenchVolumeSliceSample> rotatedSliceSamples;
        const bool rotatedSliceSampled = rasterWorkbenchVolumeSliceSamples(
            rotatedAnisotropicObject, transform, size, 1.0,
            &rotatedSliceSamples);
        const bool rotatedSliceStepMatchesObjectDimension =
            !rotatedSliceSamples.isEmpty() && std::all_of(
                rotatedSliceSamples.cbegin(), rotatedSliceSamples.cend(),
                [](const RasterWorkbenchVolumeSliceSample &sample) {
                    return std::abs(sample.sliceStepWorld - 0.1f) < 1.0e-5f;
                });
        passed &= check(
            rotatedSliceSampled && rotatedSliceStepMatchesObjectDimension,
            "QPainter rotated anisotropic volume slice opacity must use the selected object-space dimension after object scale");

        ScalarVolume3D layeredVolume = sliceVolume;
        layeredVolume.displayDensity = 20.0f;
        layeredVolume.densityScale = 20.0f;
        ViewportRenderObject redBackVolume = sliceVolumeObject;
        redBackVolume.objectId = ObjectId::fromValue(9112);
        redBackVolume.displayColorSceneLinear =
            QVector4D(0.9f, 0.0f, 0.0f, 1.0f);
        redBackVolume.shape.scalarVolume = layeredVolume;
        redBackVolume.shape.scalarVolume.sliceDepth = 0.25f;
        ViewportRenderObject blueFrontVolume = sliceVolumeObject;
        blueFrontVolume.objectId = ObjectId::fromValue(9113);
        blueFrontVolume.displayColorSceneLinear =
            QVector4D(0.0f, 0.0f, 0.9f, 1.0f);
        blueFrontVolume.shape.scalarVolume = layeredVolume;
        blueFrontVolume.shape.scalarVolume.sliceDepth = 0.5f;
        const auto renderOverlappingVolumes = [&](
            const QVector<ViewportRenderObject> &volumeOrder) {
            QImage result(size, QImage::Format_ARGB32_Premultiplied);
            result.fill(Qt::transparent);
            const bool rendered = compositeRasterWorkbenchSurfaces(
                &result, noSurfaceObjects, transform, size, 1.0, false, true,
                volumeSliceSettings, QVector3D(0.0f, 0.0f, 0.0f),
                drawObject, nullptr, {}, false, volumeOrder);
            return std::pair<bool, QColor>{
                rendered, result.pixelColor(size.width() / 2,
                                            size.height() / 2)};
        };
        const auto [redThenBlueRendered, redThenBluePixel] =
            renderOverlappingVolumes({redBackVolume, blueFrontVolume});
        const auto [blueThenRedRendered, blueThenRedPixel] =
            renderOverlappingVolumes({blueFrontVolume, redBackVolume});
        passed &= check(
            redThenBlueRendered && blueThenRedRendered &&
                redThenBluePixel.blue() > redThenBluePixel.red() &&
                blueThenRedPixel.red() > blueThenRedPixel.blue(),
            "QPainter overlapping volume slices must composite in scene object order instead of reordering them by camera depth");

        ViewportRenderObject transparentSliceOccluder = makeQuad(
            9111, 64, 48, 576, 432, -1.0,
            QVector3D(0.05f, 0.1f, 0.9f), 0.65);
        ScalarVolume3D frontTransparentSlice = sliceVolume;
        frontTransparentSlice.sliceDepth = 0.5f;
        frontTransparentSlice.displayDensity = 20.0f;
        frontTransparentSlice.densityScale = 20.0f;
        ViewportRenderObject frontTransparentSliceObject = sliceVolumeObject;
        frontTransparentSliceObject.shape.scalarVolume = frontTransparentSlice;
        QImage frontTransparentSliceImage(size,
                                          QImage::Format_ARGB32_Premultiplied);
        frontTransparentSliceImage.fill(Qt::transparent);
        const bool frontTransparentSliceRendered =
            compositeRasterWorkbenchSurfaces(
                &frontTransparentSliceImage, {transparentSliceOccluder},
                transform, size, 1.0, false, true, volumeSliceSettings,
                QVector3D(0.0f, 0.0f, 0.0f), drawObject, nullptr, {}, false,
                {frontTransparentSliceObject});
        const QColor frontTransparentSlicePixel =
            frontTransparentSliceImage.pixelColor(size.width() / 2,
                                                  size.height() / 2);
        passed &= check(
            frontTransparentSliceRendered &&
                frontTransparentSlicePixel.red() >
                    frontTransparentSlicePixel.blue(),
            "QPainter volume slices in front of transparent geometry must composite over that geometry");

        ScalarVolume3D behindTransparentSlice = frontTransparentSlice;
        behindTransparentSlice.sliceDepth = 0.1f;
        ViewportRenderObject behindTransparentSliceObject =
            frontTransparentSliceObject;
        behindTransparentSliceObject.shape.scalarVolume =
            behindTransparentSlice;
        QImage behindTransparentSliceImage(
            size, QImage::Format_ARGB32_Premultiplied);
        behindTransparentSliceImage.fill(Qt::transparent);
        const bool behindTransparentSliceRendered =
            compositeRasterWorkbenchSurfaces(
                &behindTransparentSliceImage, {transparentSliceOccluder},
                transform, size, 1.0, false, true, volumeSliceSettings,
                QVector3D(0.0f, 0.0f, 0.0f), drawObject, nullptr, {}, false,
                {behindTransparentSliceObject});
        const QColor behindTransparentSlicePixel =
            behindTransparentSliceImage.pixelColor(size.width() / 2,
                                                   size.height() / 2);
        passed &= check(
            behindTransparentSliceRendered &&
                behindTransparentSlicePixel.blue() >
                    behindTransparentSlicePixel.red(),
            "QPainter transparent depth must hide scalar-volume slices behind the surface");

        ViewportRenderObject inFrontSliceOccluder =
            transparentSliceOccluder;
        inFrontSliceOccluder.showInFront = true;
        QImage inFrontSliceImage(size, QImage::Format_ARGB32_Premultiplied);
        inFrontSliceImage.fill(Qt::transparent);
        const bool inFrontSliceRendered = compositeRasterWorkbenchSurfaces(
            &inFrontSliceImage, {inFrontSliceOccluder}, transform, size, 1.0,
            false, true, volumeSliceSettings, QVector3D(0.0f, 0.0f, 0.0f),
            drawObject, nullptr, {}, false, {frontTransparentSliceObject});
        const QColor inFrontSlicePixel = inFrontSliceImage.pixelColor(
            size.width() / 2, size.height() / 2);
        passed &= check(
            inFrontSliceRendered && inFrontSlicePixel.blue() >
                                        inFrontSlicePixel.red(),
            "QPainter showInFront coverage must reject scalar-volume slice samples");

        ViewportTransform nearPlaneSliceCamera;
        nearPlaneSliceCamera.setViewPreset(ViewportViewPreset::Perspective);
        nearPlaneSliceCamera.zoom() = 120.0;
        const Point3D nearPlaneEye =
            nearPlaneSliceCamera.cameraPosition(size);
        const Point3D nearPlaneForward =
            nearPlaneSliceCamera.viewDirection();
        double minimumSliceDepth = std::numeric_limits<double>::infinity();
        double maximumSliceDepth = -std::numeric_limits<double>::infinity();
        for (double x : {-1.5, 0.5}) {
            for (double y : {-1.5, 0.5}) {
                const double depth = -(
                    (x - nearPlaneEye.x) * nearPlaneForward.x +
                    (y - nearPlaneEye.y) * nearPlaneForward.y +
                    (-0.5 - nearPlaneEye.z) * nearPlaneForward.z);
                minimumSliceDepth = std::min(minimumSliceDepth, depth);
                maximumSliceDepth = std::max(maximumSliceDepth, depth);
            }
        }
        QVector<RasterWorkbenchVolumeSliceSample> nearPlaneSliceSamples;
        const bool nearPlaneSliceSampled = rasterWorkbenchVolumeSliceSamples(
            sliceVolumeObject, nearPlaneSliceCamera, size, 1.0,
            &nearPlaneSliceSamples);
        passed &= check(
            minimumSliceDepth < nearPlaneSliceCamera.cameraPreferences().clipStart &&
                maximumSliceDepth > nearPlaneSliceCamera.cameraPreferences().clipStart,
            "QPainter scalar-volume near-plane fixture must cross the configured perspective clip plane");
        passed &= check(
            nearPlaneSliceSampled && !nearPlaneSliceSamples.isEmpty(),
            "QPainter scalar-volume slice must retain the visible clipped portion when its plane crosses the perspective near plane");

        const auto renderRayMarchVolume = [&](float density) {
            ScalarVolume3D rayVolume = sliceVolume;
            rayVolume.useSlice = false;
            rayVolume.activeRecords[0].value = density;
            ViewportRenderObject rayObject = sliceVolumeObject;
            rayObject.shape.scalarVolume = rayVolume;
            const QVector<ViewportRenderObject> rayVolumes{rayObject};
            QImage rayImage(size, QImage::Format_ARGB32_Premultiplied);
            rayImage.fill(Qt::black);
            const bool rayRendered = compositeRasterWorkbenchSurfaces(
                &rayImage, noSurfaceObjects, transform, size, 1.0, false, true,
                volumeSliceSettings, QVector3D(0.0f, 0.0f, 0.0f), drawObject,
                nullptr, {}, false, rayVolumes);
            int changed = 0;
            int maximumRed = 0;
            for (int y = 0; y < rayImage.height(); ++y) {
                for (int x = 0; x < rayImage.width(); ++x) {
                    const QColor pixel = rayImage.pixelColor(x, y);
                    changed += pixel.red() > 0 || pixel.green() > 0 ||
                               pixel.blue() > 0;
                    maximumRed = std::max(maximumRed, pixel.red());
                }
            }
            return std::tuple<bool, int, int>{rayRendered, changed, maximumRed};
        };
        const auto [lowRayRendered, lowRayPixels, lowRayRed] =
            renderRayMarchVolume(1.0f);
        const auto [highRayRendered, highRayPixels, highRayRed] =
            renderRayMarchVolume(2.0f);
        passed &= check(lowRayRendered && highRayRendered &&
                            lowRayPixels > 0 && highRayPixels > 0 &&
                            highRayRed > lowRayRed,
                        "QPainter scalar-volume ray marching integrates bounded scene-linear density and increases opacity with density");

        ScalarVolume3D boundedRayVolume = sliceVolume;
        boundedRayVolume.useSlice = false;
        ViewportRenderObject boundedRayObject = sliceVolumeObject;
        boundedRayObject.shape.scalarVolume = boundedRayVolume;
        const qsizetype rayPixelCount =
            qsizetype(size.width()) * size.height();
        const QVector<float> unclippedRayDepth(
            rayPixelCount, -std::numeric_limits<float>::infinity());
        QVector<float> beforeNearClipDepth(
            rayPixelCount,
            float(transform.cameraPreferences().clipStart * 0.5));
        const QVector<bool> uncoveredRayPixels(rayPixelCount, false);
        const QVector<bool> fullyInFrontCoveredRayPixels(rayPixelCount, true);
        QVector<RasterWorkbenchVolumeRaySample> boundedRaySamples;
        std::size_t boundedRayIntegrationCount = 0;
        const bool visibleRaySamples = rasterWorkbenchVolumeRaySamples(
            boundedRayObject, transform, size, 1.0, unclippedRayDepth,
            uncoveredRayPixels, 4'000'000, &boundedRayIntegrationCount,
            &boundedRaySamples);
        std::size_t frontRejectedIntegrationCount = 0;
        QVector<RasterWorkbenchVolumeRaySample> frontRejectedRaySamples;
        const bool frontRejectedRay = rasterWorkbenchVolumeRaySamples(
            boundedRayObject, transform, size, 1.0, unclippedRayDepth,
            fullyInFrontCoveredRayPixels, 4'000'000,
            &frontRejectedIntegrationCount, &frontRejectedRaySamples);
        std::size_t nearRejectedIntegrationCount = 0;
        QVector<RasterWorkbenchVolumeRaySample> nearRejectedRaySamples;
        const bool nearRejectedRay = rasterWorkbenchVolumeRaySamples(
            boundedRayObject, transform, size, 1.0, beforeNearClipDepth,
            uncoveredRayPixels, 4'000'000, &nearRejectedIntegrationCount,
            &nearRejectedRaySamples);
        std::size_t cappedRayIntegrationCount = 0;
        QVector<RasterWorkbenchVolumeRaySample> cappedRaySamples;
        const bool cappedRayRendered = rasterWorkbenchVolumeRaySamples(
            boundedRayObject, transform, size, 1.0, unclippedRayDepth,
            uncoveredRayPixels, 1, &cappedRayIntegrationCount,
            &cappedRaySamples);
        passed &= check(
            visibleRaySamples && !boundedRaySamples.isEmpty() &&
                boundedRayIntegrationCount > 0 &&
                boundedRayIntegrationCount <= 4'000'000 &&
                !frontRejectedRay && frontRejectedRaySamples.isEmpty() &&
                frontRejectedIntegrationCount == 0 &&
                !nearRejectedRay && nearRejectedRaySamples.isEmpty() &&
                nearRejectedIntegrationCount == 0 && !cappedRayRendered &&
                cappedRaySamples.isEmpty() && cappedRayIntegrationCount == 1,
            "QPainter scalar-volume rays must honor near-depth and in-front coverage rejection and stop at the configured integration budget");

        std::vector<QVector3D> failureThemeBackground;
        failureThemeBackground.reserve(
            static_cast<std::size_t>(size.width()) * size.height());
        const QVector3D failureThemeHigh = workbenchSrgbToSceneLinear(
            QVector3D(0.34f, 0.48f, 0.66f));
        const QVector3D failureThemeLow = workbenchSrgbToSceneLinear(
            QVector3D(0.12f, 0.16f, 0.22f));
        for (int y = 0; y < size.height(); ++y) {
            const float factor = size.height() > 1
                ? static_cast<float>(y) / (size.height() - 1)
                : 0.0f;
            const QVector3D rowColor = failureThemeHigh * (1.0f - factor) +
                                       failureThemeLow * factor;
            for (int x = 0; x < size.width(); ++x) {
                Q_UNUSED(x);
                failureThemeBackground.push_back(rowColor);
            }
        }
        QImage nativeFailureFallbackImage(
            size, QImage::Format_ARGB32_Premultiplied);
        nativeFailureFallbackImage.fill(Qt::transparent);
        const bool nativeFailureFallbackRendered =
            compositeRasterWorkbenchSurfaces(
                &nativeFailureFallbackImage, objects, transform, size, 1.0,
                false, true, shading, std::nullopt, drawObject, nullptr,
                failureThemeBackground);
        QImage legacyFailureFallbackImage(
            size, QImage::Format_ARGB32_Premultiplied);
        legacyFailureFallbackImage.fill(Qt::transparent);
        const bool legacyFailureFallbackRendered =
            compositeRasterWorkbenchSurfaces(
                &legacyFailureFallbackImage, objects, transform, size, 1.0,
                false, false, shading, std::nullopt, drawObject);
        const QPoint themeProbe(size.width() / 2, size.height() / 2);
        const QPoint uncoveredThemeProbe(8, size.height() / 2);
        const QVector3D expectedThemeDisplay = workbenchSceneLinearToDisplaySrgb(
            failureThemeBackground[static_cast<std::size_t>(
                uncoveredThemeProbe.y()) * size.width() +
                uncoveredThemeProbe.x()],
            shading.colorTransform, shading.colorExposure,
            shading.colorGamma, shading.colorLutData,
            shading.colorLutMetadata);
        const QColor actualThemeBackground =
            nativeFailureFallbackImage.pixelColor(uncoveredThemeProbe);
        const QString fallbackCapturePath = qEnvironmentVariable(
            "CLASSICAD_VIEWPORT_QPAINTER_FAILURE_FALLBACK_CAPTURE");
        const bool fallbackCaptureSaved = fallbackCapturePath.isEmpty() ||
            nativeFailureFallbackImage.save(fallbackCapturePath);
        passed &= check(nativeFailureFallbackRendered &&
                            legacyFailureFallbackRendered &&
                            fallbackCaptureSaved &&
                            actualThemeBackground.alpha() == 255 &&
                            std::abs(actualThemeBackground.redF() -
                                     expectedThemeDisplay.x()) < 0.01f &&
                            std::abs(actualThemeBackground.greenF() -
                                     expectedThemeDisplay.y()) < 0.01f &&
                            std::abs(actualThemeBackground.blueF() -
                                     expectedThemeDisplay.z()) < 0.01f &&
                            nativeFailureFallbackImage.pixelColor(themeProbe) !=
                                legacyFailureFallbackImage.pixelColor(themeProbe),
                        "native-renderer fallback composition must use the scene-linear Theme base, preserve its gradient outside geometry, and differ from the legacy display-space resolve");
        const QString capturePath = qEnvironmentVariable(
            "CLASSICAD_VIEWPORT_QPAINTER_TRANSPARENCY_CAPTURE");
        const bool saved = capturePath.isEmpty() || image.save(capturePath);
        const QString rearCapturePath = qEnvironmentVariable(
            "CLASSICAD_VIEWPORT_QPAINTER_TRANSPARENCY_REAR_CAPTURE");
        const QString frontCapturePath = qEnvironmentVariable(
            "CLASSICAD_VIEWPORT_QPAINTER_TRANSPARENCY_FRONT_CAPTURE");
        QImage rearImage(size, QImage::Format_ARGB32_Premultiplied);
        rearImage.fill(Qt::transparent);
        QImage frontImage(size, QImage::Format_ARGB32_Premultiplied);
        frontImage.fill(Qt::transparent);
        const bool rearRendered = compositeRasterWorkbenchSurfaces(
            &rearImage, {objects[0]}, transform, size, 1.0, false, true,
            shading, backgroundLinear, drawObject);
        const bool frontRendered = compositeRasterWorkbenchSurfaces(
            &frontImage, {objects[1]}, transform, size, 1.0, false, true,
            shading, backgroundLinear, drawObject);
        const bool isolatedSaved =
            (rearCapturePath.isEmpty() || rearImage.save(rearCapturePath)) &&
            (frontCapturePath.isEmpty() || frontImage.save(frontCapturePath));
        const QColor rearOnly = rearImage.pixelColor(180, 200);
        const QColor frontOnly = frontImage.pixelColor(460, 200);
        const QColor overlap = image.pixelColor(320, 240);
        passed &= check(rendered && rearRendered && frontRendered && saved &&
                            isolatedSaved &&
                            image.size() == size && rearOnly != background &&
                            frontOnly != background && overlap != rearOnly &&
                            overlap != frontOnly &&
                            rearImage.pixelColor(441, 200) != background &&
                            rearImage.pixelColor(200, 393) != background &&
                            rearImage.pixel(442, 200) == background.rgb() &&
                            rearImage.pixel(200, 394) == background.rgb(),
                        "headless QPainter TRANSPARENT_OVERLAP must render rear-only, front-only, and distinct overlap pixels");

        // This variant deliberately reverses object order while supplying
        // camera-relative depth in the prepared triangles. It isolates the
        // transparent resolver's far-to-near ordering from QPainter's
        // separate single-surface visibility path.
        ViewportRenderObject depthFront = objects[1];
        ViewportRenderObject depthRear = objects[0];
        auto offsetPreparedDepth = [](ViewportRenderObject *object,
                                      float offset) {
            auto geometry = QSharedPointer<ViewportDepthGeometry>::create(
                *object->preparedDepthGeometry);
            for (QVector3D &vertex : geometry->surfaceVertices) {
                vertex.setZ(vertex.z() + offset);
            }
            object->preparedDepthGeometry = geometry;
        };
        offsetPreparedDepth(&depthFront, 0.35f);
        offsetPreparedDepth(&depthRear, -0.35f);
        const QVector<ViewportRenderObject> depthOrderedObjects{
            depthFront, depthRear};
        QImage depthOrderedImage(size, QImage::Format_ARGB32_Premultiplied);
        depthOrderedImage.fill(Qt::transparent);
        const bool depthOrderedRendered = compositeRasterWorkbenchSurfaces(
            &depthOrderedImage, depthOrderedObjects, transform, size, 1.0,
            false, true, shading, backgroundLinear, drawObject);
        const QString depthCapturePath = qEnvironmentVariable(
            "CLASSICAD_VIEWPORT_QPAINTER_TRANSPARENCY_DEPTH_CAPTURE");
        const bool depthSaved = depthCapturePath.isEmpty() ||
                                depthOrderedImage.save(depthCapturePath);
        passed &= check(depthOrderedRendered && depthSaved &&
                            depthOrderedImage.pixelColor(180, 200) == rearOnly &&
                            depthOrderedImage.pixelColor(460, 200) == frontOnly &&
                            depthOrderedImage.pixelColor(320, 240) == overlap,
                        "weighted QPainter transparency must preserve pixels when object iteration order is reversed");

        ViewportRenderObject regularOpaque = objects[0];
        regularOpaque.workbenchMaterial.alpha = 1.0f;
        regularOpaque.meshMaterials = {regularOpaque.workbenchMaterial};
        ViewportRenderObject inFrontOpaque = objects[1];
        inFrontOpaque.workbenchMaterial.alpha = 1.0f;
        inFrontOpaque.meshMaterials = {inFrontOpaque.workbenchMaterial};
        inFrontOpaque.showInFront = true;
        inFrontOpaque.preparedGeometryOffset.z = -0.35;
        QImage opaqueDomainImage(size, QImage::Format_ARGB32_Premultiplied);
        opaqueDomainImage.fill(Qt::transparent);
        QImage isolatedInFrontOpaqueImage(
            size, QImage::Format_ARGB32_Premultiplied);
        isolatedInFrontOpaqueImage.fill(Qt::transparent);
        const bool opaqueDomainRendered = compositeRasterWorkbenchSurfaces(
            &opaqueDomainImage, {regularOpaque, inFrontOpaque}, transform,
            size, 1.0, false, true, shading, backgroundLinear, drawObject);
        const bool isolatedInFrontOpaqueRendered =
            compositeRasterWorkbenchSurfaces(
                &isolatedInFrontOpaqueImage, {inFrontOpaque}, transform,
                size, 1.0, false, true, shading, backgroundLinear, drawObject);

        ViewportRenderObject regularTransparent = objects[1];
        regularTransparent.preparedGeometryOffset.z = 0.35;
        ViewportRenderObject inFrontTransparent = objects[0];
        inFrontTransparent.showInFront = true;
        inFrontTransparent.preparedGeometryOffset.z = -0.35;
        inFrontTransparent.workbenchMaterial.alpha = 0.5f;
        inFrontTransparent.workbenchMaterial.baseColorSceneLinear =
            QVector3D(0.1f, 0.9f, 0.2f);
        inFrontTransparent.meshMaterials = {inFrontTransparent.workbenchMaterial};
        QImage regularOnlyTransparencyImage(
            size, QImage::Format_ARGB32_Premultiplied);
        regularOnlyTransparencyImage.fill(Qt::transparent);
        QImage inFrontTransparencyImage(
            size, QImage::Format_ARGB32_Premultiplied);
        inFrontTransparencyImage.fill(Qt::transparent);
        const bool regularOnlyTransparencyRendered =
            compositeRasterWorkbenchSurfaces(
                &regularOnlyTransparencyImage,
                {regularOpaque, regularTransparent}, transform, size, 1.0,
                false, true, shading, backgroundLinear, drawObject);
        const bool inFrontTransparencyRendered =
            compositeRasterWorkbenchSurfaces(
                &inFrontTransparencyImage,
                {regularOpaque, regularTransparent, inFrontTransparent},
                transform, size, 1.0, false, true, shading, backgroundLinear,
                drawObject);
        QImage sortedDomainImage(size, QImage::Format_ARGB32_Premultiplied);
        sortedDomainImage.fill(Qt::transparent);
        QImage sortedFrontOpaqueImage(
            size, QImage::Format_ARGB32_Premultiplied);
        sortedFrontOpaqueImage.fill(Qt::transparent);
        QImage sortedFrontTransparencyImage(
            size, QImage::Format_ARGB32_Premultiplied);
        sortedFrontTransparencyImage.fill(Qt::transparent);
        const bool sortedDomainRendered = compositeRasterWorkbenchSurfaces(
            &sortedDomainImage,
            {regularOpaque, regularTransparent, inFrontOpaque}, transform,
            size, 1.0, false, false, shading, backgroundLinear, drawObject);
        const bool sortedFrontOpaqueRendered =
            compositeRasterWorkbenchSurfaces(
                &sortedFrontOpaqueImage, {inFrontOpaque}, transform, size,
                1.0, false, false, shading, backgroundLinear, drawObject);
        inFrontTransparent.preparedGeometryOffset.z = 0.0;
        const bool sortedFrontTransparencyRendered =
            compositeRasterWorkbenchSurfaces(
                &sortedFrontTransparencyImage,
                {regularOpaque, regularTransparent, inFrontOpaque,
                 inFrontTransparent},
                transform, size, 1.0, false, false, shading,
                backgroundLinear, drawObject);
        passed &= check(
            opaqueDomainRendered && isolatedInFrontOpaqueRendered &&
                regularOnlyTransparencyRendered &&
                inFrontTransparencyRendered &&
                opaqueDomainImage.pixelColor(320, 240) ==
                    isolatedInFrontOpaqueImage.pixelColor(320, 240) &&
                inFrontTransparencyImage.pixelColor(320, 240) !=
                    regularOnlyTransparencyImage.pixelColor(320, 240) &&
                inFrontTransparencyImage.pixelColor(320, 240).green() >
                    inFrontTransparencyImage.pixelColor(320, 240).red() &&
                inFrontTransparencyImage.pixelColor(320, 240).alpha() > 0 &&
                sortedDomainRendered && sortedFrontOpaqueRendered &&
                sortedFrontTransparencyRendered &&
                sortedDomainImage.pixelColor(320, 240) ==
                    sortedFrontOpaqueImage.pixelColor(320, 240) &&
                sortedFrontTransparencyImage.pixelColor(320, 240) !=
                    sortedFrontOpaqueImage.pixelColor(320, 240) &&
                sortedFrontTransparencyImage.pixelColor(320, 240).green() >
                    sortedFrontOpaqueImage.pixelColor(320, 240).green() &&
                sortedFrontTransparencyImage.pixelColor(320, 240).alpha() > 0,
            "QPainter showInFront must use separate depth domains and compose regular transparent, front opaque, then front transparent in both resolvers");

        ViewportRenderObject halfAlphaObject = objects[0];
        halfAlphaObject.workbenchMaterial.alpha = 0.5f;
        halfAlphaObject.meshMaterials = {halfAlphaObject.workbenchMaterial};
        QImage halfAlphaImage(size, QImage::Format_ARGB32_Premultiplied);
        halfAlphaImage.fill(Qt::transparent);
        renderer.setShadingSettings(shading);
        const bool halfAlphaRendered = compositeRasterWorkbenchSurfaces(
            &halfAlphaImage, {halfAlphaObject}, transform, size, 1.0,
            false, true, shading, backgroundLinear, drawObject);

        ViewportRenderObject roundedTransparentFront = objects[1];
        roundedTransparentFront.preparedGeometryOffset.z = 2.0;
        roundedTransparentFront.workbenchMaterial.alpha = 254.0f / 255.0f;
        roundedTransparentFront.workbenchMaterial.baseColorSceneLinear =
            QVector3D(0.0f, 0.0f, 0.0f);
        roundedTransparentFront.meshMaterials = {
            roundedTransparentFront.workbenchMaterial};
        ViewportRenderObject opaqueRear = objects[0];
        opaqueRear.workbenchMaterial.alpha = 1.0f;
        opaqueRear.workbenchMaterial.baseColorSceneLinear =
            QVector3D(1.0f, 1.0f, 1.0f);
        opaqueRear.meshMaterials = {opaqueRear.workbenchMaterial};
        QImage packedAlphaTransparencyImage(size,
                                             QImage::Format_ARGB32_Premultiplied);
        packedAlphaTransparencyImage.fill(Qt::transparent);
        QImage packedAlphaFrontOnlyImage(size,
                                          QImage::Format_ARGB32_Premultiplied);
        packedAlphaFrontOnlyImage.fill(Qt::transparent);
        const QVector3D blackBackground(0.0f, 0.0f, 0.0f);
        const bool packedAlphaTransparencyRendered =
            compositeRasterWorkbenchSurfaces(
                &packedAlphaTransparencyImage,
                {roundedTransparentFront, opaqueRear}, transform, size,
                1.0, false, true, shading, blackBackground, drawObject);
        const bool packedAlphaFrontOnlyRendered =
            compositeRasterWorkbenchSurfaces(
                &packedAlphaFrontOnlyImage, {roundedTransparentFront},
                transform, size, 1.0, false, true, shading,
                blackBackground, drawObject);
        passed &= check(
            packedAlphaTransparencyRendered && packedAlphaFrontOnlyRendered &&
                workbenchPackUnitFloatToUnorm8(254.0f / 255.0f) < 1.0f &&
                packedAlphaTransparencyImage.pixelColor(320, 240).red() >
                    packedAlphaFrontOnlyImage.pixelColor(320, 240).red(),
            "QPainter must keep alpha packed to 254/255 transparent so a rear opaque surface contributes through it");

        ViewportShadingSettings xraySettings = shading;
        xraySettings.xray = true;
        xraySettings.xrayAlpha = 0.5;
        ViewportRenderObject xrayObject = objects[0];
        xrayObject.workbenchMaterial.alpha = 1.0f;
        xrayObject.meshMaterials = {xrayObject.workbenchMaterial};
        QImage xrayImage(size, QImage::Format_ARGB32_Premultiplied);
        xrayImage.fill(Qt::transparent);
        renderer.setShadingSettings(xraySettings);
        const bool xrayRendered = compositeRasterWorkbenchSurfaces(
            &xrayImage, {xrayObject}, transform, size, 1.0, false, true,
            xraySettings, backgroundLinear, drawObject);
        const QRgb halfAlphaPixel = halfAlphaImage.pixel(300, 220);
        const QRgb xrayPixel = xrayImage.pixel(300, 220);
        passed &= check(halfAlphaRendered && xrayRendered &&
                            std::abs(qRed(halfAlphaPixel) - qRed(xrayPixel)) <= 1 &&
                            std::abs(qGreen(halfAlphaPixel) - qGreen(xrayPixel)) <= 1 &&
                            std::abs(qBlue(halfAlphaPixel) - qBlue(xrayPixel)) <= 1 &&
                            std::abs(qAlpha(halfAlphaPixel) - qAlpha(xrayPixel)) <= 1,
                        "QPainter mesh X-Ray alpha must multiply source alpha before weighted transparency resolve");

        // Blender's transparent pass accumulates every depth-visible surface
        // with depth writes disabled. Keep both separated sheets of one scene
        // object, while the shared diagonal in each quad contributes only one
        // sample per pixel.
        ViewportRenderObject twoSheetObject = objects[0];
        twoSheetObject.workbenchMaterial.alpha = 0.5f;
        twoSheetObject.meshMaterials = {twoSheetObject.workbenchMaterial};
        const QVector<Point3D> firstSheetVertices =
            twoSheetObject.shape.polygonMesh.vertices;
        const int secondFaceFirstVertex =
            firstSheetVertices.size();
        for (Point3D point : firstSheetVertices) {
            point.z += 0.35;
            twoSheetObject.shape.polygonMesh.vertices.append(point);
        }
        twoSheetObject.shape.polygonMesh.faces.append(
            {secondFaceFirstVertex, secondFaceFirstVertex + 1,
             secondFaceFirstVertex + 2, secondFaceFirstVertex + 3});
        twoSheetObject.shape.polygonMesh.faceMaterialIndices.append(0);
        auto twoSheetGeometry = QSharedPointer<ViewportDepthGeometry>::create(
            *twoSheetObject.preparedDepthGeometry);
        const QVector<QVector3D> firstSheet = twoSheetGeometry->surfaceVertices;
        for (QVector3D vertex : firstSheet) {
            vertex.setZ(vertex.z() + 0.35f);
            twoSheetGeometry->surfaceVertices.append(vertex);
        }
        twoSheetObject.preparedDepthGeometry = twoSheetGeometry;
        QImage twoSheetImage(size, QImage::Format_ARGB32_Premultiplied);
        twoSheetImage.fill(Qt::transparent);
        QImage oneSheetImage(size, QImage::Format_ARGB32_Premultiplied);
        oneSheetImage.fill(Qt::transparent);
        renderer.setShadingSettings(shading);
        const bool twoSheetRendered = compositeRasterWorkbenchSurfaces(
            &twoSheetImage, {twoSheetObject}, transform, size, 1.0, false,
            true, shading, backgroundLinear, drawObject);
        const bool oneSheetRendered = compositeRasterWorkbenchSurfaces(
            &oneSheetImage, {halfAlphaObject}, transform, size, 1.0, false,
            true, shading, backgroundLinear, drawObject);
        passed &= check(twoSheetRendered && oneSheetRendered &&
                            twoSheetImage.pixelColor(300, 220) !=
                                oneSheetImage.pixelColor(300, 220),
                        "weighted QPainter OIT must accumulate both depth-separated transparent surfaces of one object");
        renderer.setShadingSettings(shading);

        ViewportRenderObject opaqueOutlineObject = objects[0];
        opaqueOutlineObject.workbenchMaterial.alpha = 1.0f;
        opaqueOutlineObject.meshMaterials = {
            opaqueOutlineObject.workbenchMaterial};
        ViewportShadingSettings standardOutlineSettings = shading;
        standardOutlineSettings.outline = true;
        standardOutlineSettings.outlineColor = QColor(220, 70, 40);
        standardOutlineSettings.colorTransform = 1;
        QImage standardOutlineImage(size,
                                    QImage::Format_ARGB32_Premultiplied);
        standardOutlineImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers standardOutlineLayers;
        renderer.setShadingSettings(standardOutlineSettings);
        const bool standardOutlineRendered = compositeRasterWorkbenchSurfaces(
            &standardOutlineImage, {opaqueOutlineObject}, transform, size,
            1.0, false, true, standardOutlineSettings, backgroundLinear,
            drawObject, &standardOutlineLayers);
        ViewportShadingSettings agxOutlineSettings = standardOutlineSettings;
        agxOutlineSettings.colorTransform = 0;
        QImage agxOutlineImage(size, QImage::Format_ARGB32_Premultiplied);
        agxOutlineImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers agxOutlineLayers;
        renderer.setShadingSettings(agxOutlineSettings);
        const bool agxOutlineRendered = compositeRasterWorkbenchSurfaces(
            &agxOutlineImage, {opaqueOutlineObject}, transform, size, 1.0,
            false, true, agxOutlineSettings, backgroundLinear, drawObject,
            &agxOutlineLayers);
        const QVector3D outlineLinear = workbenchSrgbToSceneLinear(
            QVector3D(standardOutlineSettings.outlineColor.redF(),
                      standardOutlineSettings.outlineColor.greenF(),
                      standardOutlineSettings.outlineColor.blueF()));
        bool outlineMatchesSceneLinearResolve = false;
        if (standardOutlineLayers.sceneLinearSize == size &&
            agxOutlineLayers.sceneLinearSize == size) {
            for (int y = 0; y < size.height() &&
                            !outlineMatchesSceneLinearResolve; ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    const int index = y * size.width() + x;
                    const RasterWorkbenchLinearPixel &base =
                        standardOutlineLayers.sceneLinearColor[
                            static_cast<std::size_t>(index)];
                    const RasterWorkbenchLinearPixel &agxBase =
                        agxOutlineLayers.sceneLinearColor[
                            static_cast<std::size_t>(index)];
                    if (std::abs(base.red - agxBase.red) > 1.0e-5f ||
                        std::abs(base.green - agxBase.green) > 1.0e-5f ||
                        std::abs(base.blue - agxBase.blue) > 1.0e-5f ||
                        std::abs(base.alpha - agxBase.alpha) > 1.0e-5f) {
                        continue;
                    }
                    const QColor standardActual =
                        standardOutlineImage.pixelColor(x, y);
                    const QColor agxActual = agxOutlineImage.pixelColor(x, y);
                    for (int coverageStep = 1; coverageStep <= 4;
                         ++coverageStep) {
                        const float coverage = coverageStep / 4.0f;
                        const QVector3D expectedLinear(
                            outlineLinear.x() * coverage + base.red *
                                (1.0f - coverage),
                            outlineLinear.y() * coverage + base.green *
                                (1.0f - coverage),
                            outlineLinear.z() * coverage + base.blue *
                                (1.0f - coverage));
                        const QVector3D expectedStandard =
                            workbenchSceneLinearToDisplaySrgb(
                                expectedLinear,
                                standardOutlineSettings.colorTransform,
                                standardOutlineSettings.colorExposure,
                                standardOutlineSettings.colorGamma,
                                standardOutlineSettings.colorLutData,
                                standardOutlineSettings.colorLutMetadata);
                        const QVector3D expectedAgx =
                            workbenchSceneLinearToDisplaySrgb(
                                expectedLinear,
                                agxOutlineSettings.colorTransform,
                                agxOutlineSettings.colorExposure,
                                agxOutlineSettings.colorGamma,
                                agxOutlineSettings.colorLutData,
                                agxOutlineSettings.colorLutMetadata);
                        const auto withinTwoCodes = [](int actual,
                                                       float expected) {
                            return std::abs(actual -
                                qRound(expected * 255.0f)) <= 2;
                        };
                        if (withinTwoCodes(standardActual.red(),
                                           expectedStandard.x()) &&
                            withinTwoCodes(standardActual.green(),
                                           expectedStandard.y()) &&
                            withinTwoCodes(standardActual.blue(),
                                           expectedStandard.z()) &&
                            withinTwoCodes(agxActual.red(), expectedAgx.x()) &&
                            withinTwoCodes(agxActual.green(), expectedAgx.y()) &&
                            withinTwoCodes(agxActual.blue(), expectedAgx.z()) &&
                            standardActual.alpha() == agxActual.alpha()) {
                            outlineMatchesSceneLinearResolve = true;
                            break;
                        }
                    }
                    if (outlineMatchesSceneLinearResolve) break;
                }
            }
        }
        QImage outlineEffectReplay = agxOutlineImage;
        applyRasterWorkbenchEffects(
            &outlineEffectReplay, {opaqueOutlineObject}, transform, size,
            1.0, agxOutlineSettings, &agxOutlineLayers);
        passed &= check(
            standardOutlineRendered && agxOutlineRendered &&
                standardOutlineLayers.outlineAppliedBeforeDisplay &&
                agxOutlineLayers.outlineAppliedBeforeDisplay &&
                outlineMatchesSceneLinearResolve &&
                outlineEffectReplay == agxOutlineImage,
            "opaque QPainter outlines must blend in scene-linear space before Standard or AgX display conversion");

        struct SortedOutlineRender {
            QImage image;
            RasterWorkbenchSurfaceLayers layers;
            bool rendered = false;
            bool replayMatches = false;
        };
        const auto renderSortedOutline = [&](int colorTransform,
                                             bool withOutline) {
            ViewportShadingSettings settings = standardOutlineSettings;
            settings.colorTransform = colorTransform;
            settings.outline = withOutline;
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            SortedOutlineRender result;
            renderer.setShadingSettings(settings);
            result.rendered = compositeRasterWorkbenchSurfaces(
                &image, {opaqueOutlineObject}, transform, size, 1.0, false,
                false, settings, backgroundLinear, drawObject, &result.layers);
            result.image = image;
            QImage replay = image;
            applyRasterWorkbenchEffects(
                &replay, {opaqueOutlineObject}, transform, size, 1.0,
                settings, &result.layers);
            result.replayMatches = replay == image;
            return result;
        };
        const SortedOutlineRender sortedStandardOutline =
            renderSortedOutline(1, true);
        const SortedOutlineRender sortedAgxOutline =
            renderSortedOutline(0, true);
        const SortedOutlineRender sortedNoOutline =
            renderSortedOutline(1, false);
        bool sortedOutlineDisplayMatches = true;
        bool sortedOutlineLinearBlendFound = false;
        const QVector3D sortedOutlineLinear = workbenchSrgbToSceneLinear(
            QVector3D(standardOutlineSettings.outlineColor.redF(),
                      standardOutlineSettings.outlineColor.greenF(),
                      standardOutlineSettings.outlineColor.blueF()));
        if (sortedStandardOutline.layers.sceneLinearSize == size &&
            sortedAgxOutline.layers.sceneLinearSize == size &&
            sortedNoOutline.layers.sceneLinearSize == size) {
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    const std::size_t index = static_cast<std::size_t>(
                        y * size.width() + x);
                    const RasterWorkbenchLinearPixel &standard =
                        sortedStandardOutline.layers.sceneLinearColor[index];
                    const RasterWorkbenchLinearPixel &agx =
                        sortedAgxOutline.layers.sceneLinearColor[index];
                    const RasterWorkbenchLinearPixel &base =
                        sortedNoOutline.layers.sceneLinearColor[index];
                    sortedOutlineDisplayMatches &=
                        std::abs(standard.red - agx.red) < 1.0e-5f &&
                        std::abs(standard.green - agx.green) < 1.0e-5f &&
                        std::abs(standard.blue - agx.blue) < 1.0e-5f &&
                        std::abs(standard.alpha - agx.alpha) < 1.0e-5f;
                    if (std::abs(standard.red - base.red) > 1.0e-5f ||
                        std::abs(standard.green - base.green) > 1.0e-5f ||
                        std::abs(standard.blue - base.blue) > 1.0e-5f) {
                        const float alpha = (standard.red - base.red) /
                            (sortedOutlineLinear.x() - base.red);
                        sortedOutlineLinearBlendFound |= alpha > 0.0f &&
                            alpha <= 1.0f &&
                            std::abs(standard.green -
                                (sortedOutlineLinear.y() * alpha +
                                 base.green * (1.0f - alpha))) < 1.0e-4f &&
                            std::abs(standard.blue -
                                (sortedOutlineLinear.z() * alpha +
                                 base.blue * (1.0f - alpha))) < 1.0e-4f;
                    }
                    for (const auto &[rendered, image, transformIndex] : {
                             std::tuple<bool, const QImage &, int>{
                                 sortedStandardOutline.rendered,
                                 sortedStandardOutline.image, 1},
                             std::tuple<bool, const QImage &, int>{
                                 sortedAgxOutline.rendered,
                                 sortedAgxOutline.image, 0}}) {
                        (void)rendered;
                        const RasterWorkbenchLinearPixel &pixel =
                            transformIndex == 1 ? standard : agx;
                        if (pixel.alpha <= 0.0f) {
                            sortedOutlineDisplayMatches &=
                                image.pixel(x, y) == qRgba(0, 0, 0, 0);
                            continue;
                        }
                        const QVector3D display =
                            workbenchSceneLinearToDisplaySrgb(
                                QVector3D(pixel.red / pixel.alpha,
                                          pixel.green / pixel.alpha,
                                          pixel.blue / pixel.alpha),
                                transformIndex, 0.0f, 1.0f, {}, {});
                        const QRgb expected = qPremultiply(qRgba(
                            qRound(display.x() * 255.0f),
                            qRound(display.y() * 255.0f),
                            qRound(display.z() * 255.0f),
                            qRound(pixel.alpha * 255.0f)));
                        sortedOutlineDisplayMatches &=
                            image.pixel(x, y) == expected;
                    }
                }
            }
        } else {
            sortedOutlineDisplayMatches = false;
        }
        renderer.setShadingSettings(standardOutlineSettings);
        passed &= check(
            sortedStandardOutline.rendered && sortedAgxOutline.rendered &&
                sortedNoOutline.rendered &&
                sortedStandardOutline.layers.outlineAppliedBeforeDisplay &&
                sortedAgxOutline.layers.outlineAppliedBeforeDisplay &&
                sortedStandardOutline.replayMatches &&
                sortedAgxOutline.replayMatches &&
                sortedOutlineDisplayMatches && sortedOutlineLinearBlendFound,
            "sorted QPainter outlines must blend into retained scene-linear color before Standard or AgX display conversion");
        ViewportShadingSettings transparentOutlineSettings =
            standardOutlineSettings;
        transparentOutlineSettings.colorTransform = 0;
        QImage transparentOutlineImage(
            size, QImage::Format_ARGB32_Premultiplied);
        transparentOutlineImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers transparentOutlineLayers;
        renderer.setShadingSettings(transparentOutlineSettings);
        const bool transparentOutlineRendered =
            compositeRasterWorkbenchSurfaces(
                &transparentOutlineImage, {objects[0]}, transform, size, 1.0,
                false, true, transparentOutlineSettings, backgroundLinear,
                drawObject, &transparentOutlineLayers);
        QImage transparentOutlineEffectReplay = transparentOutlineImage;
        applyRasterWorkbenchEffects(
            &transparentOutlineEffectReplay, {objects[0]}, transform, size,
            1.0, transparentOutlineSettings, &transparentOutlineLayers);
        passed &= check(
            transparentOutlineRendered &&
                transparentOutlineLayers.outlineAppliedBeforeDisplay &&
                transparentOutlineLayers.sceneLinearSize == size &&
                transparentOutlineEffectReplay == transparentOutlineImage,
            "QPainter transparent-material outlines must use the retained scene-linear resolve and avoid a duplicate display-space pass");

        struct XrayOutlineRender {
            QImage image;
            RasterWorkbenchSurfaceLayers layers;
            bool rendered = false;
            bool replayMatches = true;
        };
        const auto renderXrayOutline = [&](int colorTransform,
                                           bool withOutline,
                                           bool visibleOutline = true) {
            ViewportShadingSettings settings = standardOutlineSettings;
            settings.colorTransform = colorTransform;
            settings.xray = true;
            settings.xrayAlpha = 0.5;
            settings.shadows = false;
            settings.depthOfField = false;
            settings.cavity = false;
            settings.outline = withOutline;
            if (!visibleOutline)
                settings.outlineColor.setAlpha(0);
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            XrayOutlineRender result;
            renderer.setShadingSettings(settings);
            const QVector<ViewportRenderObject> xrayOutlineObjects{
                opaqueOutlineObject, objects[1]};
            result.rendered = compositeRasterWorkbenchSurfaces(
                &image, xrayOutlineObjects, transform, size, 1.0, false, true,
                settings, backgroundLinear, drawObject, &result.layers);
            result.image = std::move(image);
            if (withOutline) {
                QImage replay = result.image;
                applyRasterWorkbenchEffects(
                    &replay, xrayOutlineObjects, transform, size, 1.0, settings,
                    &result.layers);
                result.replayMatches = replay == result.image;
            }
            return result;
        };
        XrayOutlineRender standardXrayOutline =
            renderXrayOutline(1, true);
        XrayOutlineRender agxXrayOutline =
            renderXrayOutline(0, true);
        XrayOutlineRender noXrayOutline =
            renderXrayOutline(1, true, false);
        bool xrayLinearBasesMatch = true;
        bool xrayOutlineMatchesLinearResolve = false;
        const QVector3D xrayOutlineColorLinear =
            workbenchSrgbToSceneLinear(QVector3D(
                standardOutlineSettings.outlineColor.redF(),
                standardOutlineSettings.outlineColor.greenF(),
                standardOutlineSettings.outlineColor.blueF()));
        if (standardXrayOutline.layers.sceneLinearColor.size() ==
                static_cast<std::size_t>(size.width()) * size.height() &&
            agxXrayOutline.layers.sceneLinearColor.size() ==
                standardXrayOutline.layers.sceneLinearColor.size() &&
            noXrayOutline.layers.sceneLinearColor.size() ==
                standardXrayOutline.layers.sceneLinearColor.size()) {
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    const std::size_t index =
                        static_cast<std::size_t>(y) * size.width() + x;
                    const RasterWorkbenchLinearPixel &standard =
                        standardXrayOutline.layers.sceneLinearColor[index];
                    const RasterWorkbenchLinearPixel &agx =
                        agxXrayOutline.layers.sceneLinearColor[index];
                    const RasterWorkbenchLinearPixel &withoutOutline =
                        noXrayOutline.layers.sceneLinearColor[index];
                    xrayLinearBasesMatch &=
                        std::abs(standard.red - agx.red) <= 1.0e-5f &&
                        std::abs(standard.green - agx.green) <= 1.0e-5f &&
                        std::abs(standard.blue - agx.blue) <= 1.0e-5f &&
                        std::abs(standard.alpha - agx.alpha) <= 1.0e-5f;
                    const QColor standardActual =
                        standardXrayOutline.image.pixelColor(x, y);
                    const QColor agxActual = agxXrayOutline.image.pixelColor(x, y);
                    for (int coverageStep = 1; coverageStep <= 4;
                         ++coverageStep) {
                        const float coverage = coverageStep / 4.0f;
                        const float expectedAlpha = coverage +
                            withoutOutline.alpha * (1.0f - coverage);
                        const QVector3D expectedLinear(
                            xrayOutlineColorLinear.x() * coverage +
                                withoutOutline.red * (1.0f - coverage),
                            xrayOutlineColorLinear.y() * coverage +
                                withoutOutline.green * (1.0f - coverage),
                            xrayOutlineColorLinear.z() * coverage +
                                withoutOutline.blue * (1.0f - coverage));
                        const QVector3D expectedStandard =
                            workbenchSceneLinearToDisplaySrgb(
                                expectedLinear, 1,
                                standardOutlineSettings.colorExposure,
                                standardOutlineSettings.colorGamma,
                                standardOutlineSettings.colorLutData,
                                standardOutlineSettings.colorLutMetadata);
                        const QVector3D expectedAgx =
                            workbenchSceneLinearToDisplaySrgb(
                                expectedLinear, 0,
                                agxOutlineSettings.colorExposure,
                                agxOutlineSettings.colorGamma,
                                agxOutlineSettings.colorLutData,
                                agxOutlineSettings.colorLutMetadata);
                        const auto withinTwoCodes = [](int actual,
                                                       float expected) {
                            return std::abs(actual -
                                qRound(expected * 255.0f)) <= 2;
                        };
                        if (withinTwoCodes(standardActual.red(),
                                           expectedStandard.x()) &&
                            withinTwoCodes(standardActual.green(),
                                           expectedStandard.y()) &&
                            withinTwoCodes(standardActual.blue(),
                                           expectedStandard.z()) &&
                            withinTwoCodes(agxActual.red(), expectedAgx.x()) &&
                            withinTwoCodes(agxActual.green(), expectedAgx.y()) &&
                            withinTwoCodes(agxActual.blue(), expectedAgx.z()) &&
                            std::abs(standardActual.alpha() -
                                     qRound(expectedAlpha * 255.0f)) <= 2 &&
                            standardActual.alpha() == agxActual.alpha()) {
                            xrayOutlineMatchesLinearResolve = true;
                            break;
                        }
                    }
                }
            }
        }
        passed &= check(
            standardXrayOutline.rendered && agxXrayOutline.rendered &&
                noXrayOutline.rendered,
            "weighted QPainter X-Ray outline fixtures must render");
        passed &= check(
            standardXrayOutline.layers.outlineAppliedBeforeDisplay &&
                agxXrayOutline.layers.outlineAppliedBeforeDisplay &&
                !standardXrayOutline.layers.sceneLinearOutlinePending &&
                !agxXrayOutline.layers.sceneLinearOutlinePending,
            "weighted QPainter X-Ray outlines must resolve before display conversion without a deferred outline");
        passed &= check(xrayLinearBasesMatch,
            "weighted QPainter X-Ray outline must preserve one view-independent scene-linear OIT base");
        passed &= check(xrayOutlineMatchesLinearResolve,
            "weighted QPainter X-Ray outline output must match scene-linear blend color and alpha under Standard and AgX");
        passed &= check(
            standardXrayOutline.replayMatches && agxXrayOutline.replayMatches &&
                standardXrayOutline.image != agxXrayOutline.image,
            "weighted QPainter X-Ray outlines must not be applied twice and Standard/AgX output must differ");

        bool standardShadowRendered = false;
        bool standardShadowChangedLinearScene = false;
        bool standardShadowDisplayMatched = false;
        bool agxShadowRendered = false;
        bool agxShadowChangedLinearScene = false;
        bool agxShadowDisplayMatched = false;
        bool sortedStandardShadowRendered = false;
        bool sortedStandardShadowChangedLinearScene = false;
        bool sortedStandardShadowDisplayMatched = false;
        bool sortedAgxShadowRendered = false;
        bool sortedAgxShadowChangedLinearScene = false;
        bool sortedAgxShadowDisplayMatched = false;
        const auto renderShadowOutline = [&](int colorTransform,
                                             bool *renderedResult,
                                             bool *shadowChangedResult,
                                             bool *displayMatchedResult,
                                             bool useWeightedOit = true) {
            ViewportShadingSettings settings = standardOutlineSettings;
            settings.colorTransform = colorTransform;
            settings.shadows = true;
            settings.shadowIntensity = 0.8;
            settings.shadowFocus = 0.9;
            settings.depthOfField = false;
            settings.cavity = false;
            ViewportRenderObject shadowReceiver = opaqueOutlineObject;
            shadowReceiver.workbenchMaterial.alpha = 1.0f;
            shadowReceiver.meshMaterials = {shadowReceiver.workbenchMaterial};
            ViewportRenderObject shadowCaster = objects[1];
            shadowCaster.workbenchMaterial.alpha = 1.0f;
            shadowCaster.meshMaterials = {shadowCaster.workbenchMaterial};
            shadowCaster.preparedGeometryOffset.z += 2.0;
            const QVector<ViewportRenderObject> shadowOutlineObjects{
                shadowReceiver, shadowCaster};
            QImage image(size,
                         QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            RasterWorkbenchSurfaceLayers layers;
            renderer.setShadingSettings(settings);
            const bool rendered = compositeRasterWorkbenchSurfaces(
                &image, shadowOutlineObjects, transform, size, 1.0, false,
                useWeightedOit, settings, backgroundLinear, drawObject, &layers);
            *renderedResult = rendered && layers.sceneLinearOutlinePending &&
                              !layers.outlineAppliedBeforeDisplay;
            const std::vector<RasterWorkbenchLinearPixel> beforeEffects =
                layers.sceneLinearColor;
            applyRasterWorkbenchEffects(
                &image, shadowOutlineObjects, transform, size, 1.0, settings,
                &layers);
            bool shadowChangedLinearScene = false;
            bool matchesFinalLinearTransform = true;
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    const std::size_t index =
                        static_cast<std::size_t>(y) *
                            size.width() + x;
                    const RasterWorkbenchLinearPixel &pixel =
                        layers.sceneLinearColor[index];
                    if (std::abs(pixel.red - beforeEffects[index].red) > 1.0e-5f ||
                        std::abs(pixel.green - beforeEffects[index].green) > 1.0e-5f ||
                        std::abs(pixel.blue - beforeEffects[index].blue) > 1.0e-5f) {
                        shadowChangedLinearScene = true;
                    }
                    const QVector3D display = pixel.alpha > 0.0f
                        ? workbenchSceneLinearToDisplaySrgb(
                              QVector3D(pixel.red / pixel.alpha,
                                        pixel.green / pixel.alpha,
                                        pixel.blue / pixel.alpha),
                              settings.colorTransform, settings.colorExposure,
                              settings.colorGamma, settings.colorLutData,
                              settings.colorLutMetadata)
                        : QVector3D();
                    const QRgb expectedPixel = qPremultiply(qRgba(
                        std::clamp(qRound(display.x() * 255.0f), 0, 255),
                        std::clamp(qRound(display.y() * 255.0f), 0, 255),
                        std::clamp(qRound(display.z() * 255.0f), 0, 255),
                        qRound(pixel.alpha * 255.0f)));
                    const bool pixelMatches = image.pixel(x, y) == expectedPixel;
                    matchesFinalLinearTransform &= pixelMatches;
                }
            }
            *shadowChangedResult = shadowChangedLinearScene;
            *displayMatchedResult = matchesFinalLinearTransform &&
                layers.outlineAppliedBeforeDisplay &&
                !layers.sceneLinearOutlinePending;
            return rendered && layers.outlineAppliedBeforeDisplay &&
                   !layers.sceneLinearOutlinePending &&
                   shadowChangedLinearScene && matchesFinalLinearTransform;
        };
        const bool standardShadowOutline = renderShadowOutline(
            1, &standardShadowRendered, &standardShadowChangedLinearScene,
            &standardShadowDisplayMatched);
        const bool agxShadowOutline = renderShadowOutline(
            0, &agxShadowRendered, &agxShadowChangedLinearScene,
            &agxShadowDisplayMatched);
        const bool sortedStandardShadowOutline = renderShadowOutline(
            1, &sortedStandardShadowRendered,
            &sortedStandardShadowChangedLinearScene,
            &sortedStandardShadowDisplayMatched, false);
        const bool sortedAgxShadowOutline = renderShadowOutline(
            0, &sortedAgxShadowRendered, &sortedAgxShadowChangedLinearScene,
            &sortedAgxShadowDisplayMatched, false);
        passed &= check(standardShadowRendered && agxShadowRendered,
            "shadow-plus-outline weighted scenes must retain the scene-linear outline mask");
        passed &= check(standardShadowChangedLinearScene &&
                            agxShadowChangedLinearScene,
            "QPainter shadows must modify the retained opaque scene-linear contribution beneath outlines");
        passed &= check(standardShadowDisplayMatched &&
                            agxShadowDisplayMatched &&
                            standardShadowOutline && agxShadowOutline,
            "weighted QPainter shadow-plus-outline resolve must display-transform the final linear result once under Standard and AgX");
        passed &= check(sortedStandardShadowRendered &&
                            sortedAgxShadowRendered &&
                            sortedStandardShadowChangedLinearScene &&
                            sortedAgxShadowChangedLinearScene &&
                            sortedStandardShadowDisplayMatched &&
                            sortedAgxShadowDisplayMatched &&
                            sortedStandardShadowOutline &&
                            sortedAgxShadowOutline,
            "sorted QPainter shadow-plus-outline resolve must retain, shadow, outline, and display-transform scene-linear color once under Standard and AgX");
        renderer.setShadingSettings(shading);

        ViewportRenderObject cavityReceiver = objects[0];
        cavityReceiver.workbenchMaterial.alpha = 1.0f;
        cavityReceiver.meshMaterials = {cavityReceiver.workbenchMaterial};
        ViewportRenderObject cavityTransparent = objects[1];
        cavityTransparent.preparedGeometryOffset.z = 0.35;
        QVector<ViewportRenderObject> cavityObjects{
            cavityReceiver, cavityTransparent};
        QImage cavityOffImage(size, QImage::Format_ARGB32_Premultiplied);
        cavityOffImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers cavityOffLayers;
        renderer.setShadingSettings(shading);
        const bool cavityOffRendered = compositeRasterWorkbenchSurfaces(
            &cavityOffImage, cavityObjects, transform, size, 1.0, false, true,
            shading, backgroundLinear, drawObject, &cavityOffLayers);
        ViewportShadingSettings linearCavitySettings = shading;
        linearCavitySettings.cavity = true;
        linearCavitySettings.cavityType = ViewportCavityType::World;
        linearCavitySettings.cavityDistance = 0.35;
        linearCavitySettings.cavitySamples = 16;
        QImage cavityOnImage(size, QImage::Format_ARGB32_Premultiplied);
        cavityOnImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers cavityOnLayers;
        renderer.setShadingSettings(linearCavitySettings);
        const bool cavityOnRendered = compositeRasterWorkbenchSurfaces(
            &cavityOnImage, cavityObjects, transform, size, 1.0, false, true,
            linearCavitySettings, backgroundLinear, drawObject,
            &cavityOnLayers);
        QImage cavityPostEffectImage = cavityOnImage;
        applyRasterWorkbenchEffects(
            &cavityPostEffectImage, cavityObjects, transform, size, 1.0,
            linearCavitySettings, &cavityOnLayers);
        bool opaqueCavityChanged = false;
        if (cavityOffLayers.opaqueColor.size() == size &&
            cavityOnLayers.opaqueColor.size() == size) {
            for (int y = 0; y < size.height() && !opaqueCavityChanged; ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    if (cavityOffLayers.opaqueColor.pixel(x, y) !=
                        cavityOnLayers.opaqueColor.pixel(x, y)) {
                        opaqueCavityChanged = true;
                        break;
                    }
                }
            }
        }
        passed &= check(cavityOffRendered && cavityOnRendered &&
                            cavityOnLayers.cavityAppliedBeforeTransparency &&
                            opaqueCavityChanged &&
                            cavityOffLayers.transparentColor ==
                                cavityOnLayers.transparentColor &&
                            cavityOffImage != cavityOnImage &&
                            cavityPostEffectImage == cavityOnImage,
                        "QPainter cavity must shade the scene-linear opaque contribution before weighted transparency resolve");
        ViewportShadingSettings sortedCavitySettings =
            linearCavitySettings;
        sortedCavitySettings.colorTransform = 0; // Blender AgX.
        sortedCavitySettings.cavity = false;
        QImage sortedCavityOffImage(size,
                                    QImage::Format_ARGB32_Premultiplied);
        sortedCavityOffImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers sortedCavityOffLayers;
        renderer.setShadingSettings(sortedCavitySettings);
        const bool sortedCavityOffRendered = compositeRasterWorkbenchSurfaces(
            &sortedCavityOffImage, cavityObjects, transform, size, 1.0, true,
            false, sortedCavitySettings, backgroundLinear, drawObject,
            &sortedCavityOffLayers);
        sortedCavitySettings.cavity = true;
        QImage sortedCavityOnImage(size,
                                   QImage::Format_ARGB32_Premultiplied);
        sortedCavityOnImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers sortedCavityOnLayers;
        renderer.setShadingSettings(sortedCavitySettings);
        const bool sortedCavityOnRendered = compositeRasterWorkbenchSurfaces(
            &sortedCavityOnImage, cavityObjects, transform, size, 1.0, true,
            false, sortedCavitySettings, backgroundLinear, drawObject,
            &sortedCavityOnLayers);
        QImage sortedCavityPostEffectImage = sortedCavityOnImage;
        applyRasterWorkbenchEffects(
            &sortedCavityPostEffectImage, cavityObjects, transform, size,
            1.0, sortedCavitySettings, &sortedCavityOnLayers);
        passed &= check(
            sortedCavityOffRendered && sortedCavityOnRendered &&
                sortedCavityOnLayers.cavityAppliedBeforeTransparency &&
                sortedCavityOffLayers.transparentColor ==
                    sortedCavityOnLayers.transparentColor &&
                sortedCavityOffImage != sortedCavityOnImage &&
                sortedCavityPostEffectImage == sortedCavityOnImage,
            "QPainter sorted transparency must composite an AgX cavity-shaded opaque layer without tinting transparent color twice");
        ViewportShadingSettings sortedCavityStandardSettings =
            sortedCavitySettings;
        sortedCavityStandardSettings.colorTransform = 1;
        QImage sortedCavityStandardImage(
            size, QImage::Format_ARGB32_Premultiplied);
        sortedCavityStandardImage.fill(Qt::transparent);
        RasterWorkbenchSurfaceLayers sortedCavityStandardLayers;
        renderer.setShadingSettings(sortedCavityStandardSettings);
        const bool sortedCavityStandardRendered =
            compositeRasterWorkbenchSurfaces(
                &sortedCavityStandardImage, cavityObjects, transform, size,
                1.0, true, false, sortedCavityStandardSettings,
                backgroundLinear, drawObject, &sortedCavityStandardLayers);
        int maximumSortedDisplayError = 0;
        float maximumSortedLinearError = 0.0f;
        const auto &standardLinear =
            sortedCavityStandardLayers.sceneLinearColor;
        const auto &agxLinear = sortedCavityOnLayers.sceneLinearColor;
        const bool sortedLinearLayersMatch =
            standardLinear.size() == static_cast<std::size_t>(size.width()) *
                                         size.height() &&
            agxLinear.size() == standardLinear.size();
        if (sortedLinearLayersMatch) {
            for (int y = 0; y < size.height(); ++y) {
                for (int x = 0; x < size.width(); ++x) {
                    const std::size_t index =
                        static_cast<std::size_t>(y) * size.width() + x;
                    const RasterWorkbenchLinearPixel &standard =
                        standardLinear[index];
                    const RasterWorkbenchLinearPixel &agx = agxLinear[index];
                    maximumSortedLinearError = std::max({
                        maximumSortedLinearError,
                        std::abs(standard.red - agx.red),
                        std::abs(standard.green - agx.green),
                        std::abs(standard.blue - agx.blue),
                        std::abs(standard.alpha - agx.alpha)});
                    QVector3D expectedDisplay;
                    if (standard.alpha > 0.0f) {
                        expectedDisplay = workbenchSceneLinearToDisplaySrgb(
                            QVector3D(standard.red / standard.alpha,
                                      standard.green / standard.alpha,
                                      standard.blue / standard.alpha),
                            sortedCavitySettings.colorTransform,
                            sortedCavitySettings.colorExposure,
                            sortedCavitySettings.colorGamma,
                            sortedCavitySettings.colorLutData,
                            sortedCavitySettings.colorLutMetadata);
                    }
                    const QRgb expected = qPremultiply(qRgba(
                        std::clamp(qRound(expectedDisplay.x() * 255.0f), 0, 255),
                        std::clamp(qRound(expectedDisplay.y() * 255.0f), 0, 255),
                        std::clamp(qRound(expectedDisplay.z() * 255.0f), 0, 255),
                        std::clamp(qRound(standard.alpha * 255.0f), 0, 255)));
                    const QRgb actual = sortedCavityOnImage.pixel(x, y);
                    maximumSortedDisplayError = std::max({
                        maximumSortedDisplayError,
                        std::abs(qRed(actual) - qRed(expected)),
                        std::abs(qGreen(actual) - qGreen(expected)),
                        std::abs(qBlue(actual) - qBlue(expected)),
                        std::abs(qAlpha(actual) - qAlpha(expected))});
                }
            }
        }
        passed &= check(
            sortedCavityStandardRendered && sortedLinearLayersMatch &&
                maximumSortedLinearError <= 1.0e-6f &&
                maximumSortedDisplayError <= 1,
            "QPainter sorted cavity and transparency must resolve in scene-linear color before AgX display conversion");
        renderer.setShadingSettings(shading);

        const QVector3D themeHighSrgb(61.0f / 255.0f,
                                      61.0f / 255.0f,
                                      61.0f / 255.0f);
        const QVector3D themeLowSrgb(48.0f / 255.0f,
                                     48.0f / 255.0f,
                                     48.0f / 255.0f);
        const QVector3D themeHigh =
            workbenchSrgbToSceneLinear(themeHighSrgb);
        const QVector3D themeLow = workbenchSrgbToSceneLinear(themeLowSrgb);
        std::vector<QVector3D> themeBackgroundPixels;
        themeBackgroundPixels.reserve(size.width() * size.height());
        for (int y = 0; y < size.height(); ++y) {
            const float factor = (y + 0.5f) / size.height();
            const QVector3D rowColor = themeHigh * (1.0f - factor) +
                                       themeLow * factor;
            for (int x = 0; x < size.width(); ++x)
                themeBackgroundPixels.push_back(rowColor);
        }
        QImage themeImage(size, QImage::Format_ARGB32_Premultiplied);
        themeImage.fill(Qt::transparent);
        const bool themeRendered = compositeRasterWorkbenchSurfaces(
            &themeImage, objects, transform, size, 1.0, false, true, shading,
            std::nullopt, drawObject, nullptr, themeBackgroundPixels);
        const int themeProbeX = 10;
        const int themeProbeY = 10;
        const float themeProbeFactor = (themeProbeY + 0.5f) / size.height();
        const QVector3D themeProbeLinear = themeHigh * (1.0f - themeProbeFactor) +
                                           themeLow * themeProbeFactor;
        const QVector3D themeProbeDisplay = workbenchSceneLinearToDisplaySrgb(
            themeProbeLinear, shading.colorTransform, shading.colorExposure,
            shading.colorGamma, shading.colorLutData,
            shading.colorLutMetadata);
        const QColor themeProbeExpected = QColor::fromRgb(
            qRound(themeProbeDisplay.x() * 255.0f),
            qRound(themeProbeDisplay.y() * 255.0f),
            qRound(themeProbeDisplay.z() * 255.0f));
        passed &= check(themeRendered &&
                            themeImage.pixelColor(themeProbeX, themeProbeY) ==
                                themeProbeExpected,
                        "QPainter transparent surfaces must resolve against the per-pixel scene-linear Theme background");
    }

    ViewportTransform perspectiveDofTransform = orthographicDofTransform;
    perspectiveDofTransform.setPerspectiveEnabled(true);
    const Point3D perspectiveDofEye =
        perspectiveDofTransform.cameraPosition(orthographicDofSize);
    const auto makePerspectiveDofObject = [&](quint64 id, qreal depth,
                                               qreal left, qreal right) {
        ViewportRenderObject object;
        object.shape.geometryType = GeometryType::NurbsSurface;
        object.objectId = ObjectId::fromValue(id);
        auto geometry = QSharedPointer<ViewportDepthGeometry>::create();
        const auto worldAt = [&](qreal x, qreal y) {
            QPointF local;
            perspectiveDofTransform.screenToWorkPlaneUnclipped(
                QPointF(x, y), orthographicDofSize, WorkPlane::XY,
                perspectiveDofEye.z - depth, &local);
            return QVector3D(float(local.x()), float(local.y()),
                             float(perspectiveDofEye.z - depth));
        };
        const QVector3D a = worldAt(left, 8.0);
        const QVector3D b = worldAt(right, 8.0);
        const QVector3D c = worldAt(right, 120.0);
        const QVector3D d = worldAt(left, 120.0);
        geometry->surfaceVertices = {a, b, c, a, c, d};
        geometry->surfaceNormals =
            QVector<QVector3D>(6, QVector3D(0.0f, 0.0f, 1.0f));
        object.preparedDepthGeometry = geometry;
        return object;
    };
    const QVector<ViewportRenderObject> perspectiveDofObjects{
        makePerspectiveDofObject(3, 8.0, 8.0, 56.0),
        makePerspectiveDofObject(4, 16.0, 72.0, 120.0)};
    QImage perspectiveDofImage(orthographicDofSize,
                                QImage::Format_ARGB32_Premultiplied);
    perspectiveDofImage.fill(Qt::black);
    perspectiveDofImage.setPixelColor(32, 64, Qt::red);
    perspectiveDofImage.setPixelColor(96, 64, Qt::red);
    applyRasterWorkbenchEffects(
        &perspectiveDofImage, perspectiveDofObjects, perspectiveDofTransform,
        orthographicDofSize, 1.0, orthographicDofSettings);
    passed &= check(
        perspectiveDofImage.pixelColor(32, 64).red() == 255 &&
            perspectiveDofImage.pixelColor(96, 64).red() > 0 &&
            perspectiveDofImage.pixelColor(96, 64).red() < 240,
        "perspective scene-camera DOF CoC must retain the Blender aperture-radius factor while preserving focus");

    QImage dofBackgroundBleedImage(
        orthographicDofSize, QImage::Format_ARGB32_Premultiplied);
    dofBackgroundBleedImage.fill(Qt::black);
    for (int y = 8; y < 120; ++y) {
        for (int x = 8; x < 56; ++x)
            dofBackgroundBleedImage.setPixelColor(x, y, Qt::red);
    }
    const int dofBackgroundProbeX = 56;
    const int dofBackgroundProbeY = 64;
    const QColor sharpBackgroundProbe = dofBackgroundBleedImage.pixelColor(
        dofBackgroundProbeX, dofBackgroundProbeY);
    applyRasterWorkbenchEffects(
        &dofBackgroundBleedImage,
        {makeOrthographicDofObject(6, 16.0, 8.0, 56.0)},
        orthographicDofTransform, orthographicDofSize, 1.0,
        orthographicDofSettings);
    passed &= check(sharpBackgroundProbe.red() == 0 &&
                        dofBackgroundBleedImage.pixelColor(
                            dofBackgroundProbeX,
                            dofBackgroundProbeY).red() > 0,
                    "QPainter full-screen DOF resolve must spread a defocused foreground edge into far-plane background pixels");

    QImage cpuDitherInput(1, 1, QImage::Format_RGBA64);
    cpuDitherInput.setPixelColor(0, 0, QColor::fromRgbF(0.18, 0.18, 0.18,
                                                       0.37));
    QImage cpuDithered = cpuDitherInput;
    applyWorkbenchRenderDither(&cpuDithered, 1.0, 8);
    const QColor cpuDitherPixel = cpuDithered.convertToFormat(
        QImage::Format_RGBA8888).pixelColor(0, 0);
    QImage highDepthDithered = cpuDitherInput;
    applyWorkbenchRenderDither(&highDepthDithered, 1.0, 16);
    QImage premultipliedInput(1, 1, QImage::Format_RGBA64);
    auto *premultipliedChannels = reinterpret_cast<quint16 *>(
        premultipliedInput.scanLine(0));
    premultipliedChannels[0] = premultipliedChannels[1] =
        premultipliedChannels[2] = 5898;
    premultipliedChannels[3] = 32768;
    QImage premultipliedDithered = premultipliedInput;
    applyWorkbenchRenderDither(&premultipliedDithered, 1.0, 8, true);
    const auto *premultipliedDitheredChannels =
        reinterpret_cast<const quint16 *>(premultipliedDithered.constScanLine(0));
    passed &= check(cpuDitherPixel.red() == 45 &&
                        cpuDitherPixel.green() == 45 &&
                        cpuDitherPixel.blue() == 45 &&
                        cpuDitherPixel.alpha() ==
                            cpuDitherInput.pixelColor(0, 0).alpha() &&
                        highDepthDithered == cpuDitherInput &&
                        premultipliedDitheredChannels[0] <
                            premultipliedChannels[0] &&
                        premultipliedDitheredChannels[0] > 5700 &&
                        premultipliedDitheredChannels[3] ==
                            premultipliedChannels[3],
                    "CPU export dither must match Blender's bottom-left RGB byte, preserve alpha and premultiplication, and leave 16-bit output unchanged");

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
    WorkbenchStudioLighting monochromaticSpecularLighting;
    monochromaticSpecularLighting.ambientColor = QVector3D(1.0f, 1.0f, 1.0f);
    const QVector3D monochromaticSpecularResult = workbenchStudioShade(
        QVector3D(0.25f, 0.5f, 0.75f), QVector3D(0.0f, 0.0f, 1.0f),
        QVector3D(1.0f, 0.0f, 0.0f), 0.0f, 0.5f,
        monochromaticSpecularLighting);
    const float truncatedSpecularEnergy =
        QVector3D::dotProduct(QVector3D(1.0f, 1.0f, 1.0f),
                              QVector3D(0.33333f, 0.33333f, 0.33333f));
    const float expectedMonochromaticSpecularRed =
        1.0f + 0.125f * (1.0f - truncatedSpecularEnergy);
    passed &= check(
        std::abs(monochromaticSpecularResult.x() -
                 expectedMonochromaticSpecularRed) < 2.0e-7f &&
            monochromaticSpecularResult.x() > 1.0f,
        "QPainter Workbench Studio energy conservation must use Blender's 0.33333 monochromatic specular coefficient");
    const QVector3D roundTripColor = workbenchSceneLinearToSrgb(
        workbenchSrgbToSceneLinear(QVector3D(0.2f, 0.5f, 0.8f)));
    const ViewportThemeBackground pixelThemeBackground{
        ViewportThemeBackgroundStyle::Radial,
        QColor(200, 100, 50), QColor(20, 40, 60)};
    const auto paintThemeCapture = [&](ViewportThemeBackgroundStyle style) {
        QImage image(101, 101, QImage::Format_RGB32);
        image.fill(Qt::black);
        ViewportThemeBackground background = pixelThemeBackground;
        background.style = style;
        QPainter painter(&image);
        paintViewportThemeBackground(painter, image.rect(), background);
        painter.end();
        return image;
    };
    const QImage singleThemeImage = paintThemeCapture(
        ViewportThemeBackgroundStyle::SingleColor);
    const QImage linearThemeImage = paintThemeCapture(
        ViewportThemeBackgroundStyle::Linear);
    const QImage radialThemeImage = paintThemeCapture(
        ViewportThemeBackgroundStyle::Radial);
    const QColor singleThemePixel = singleThemeImage.pixelColor(50, 50);
    const QColor linearThemeTopPixel = linearThemeImage.pixelColor(50, 0);
    const QColor linearThemeBottomPixel = linearThemeImage.pixelColor(50, 100);
    const QColor radialThemeCenterPixel = radialThemeImage.pixelColor(50, 50);
    const QColor radialThemeCornerPixel = radialThemeImage.pixelColor(0, 0);
    passed &= check(singleThemePixel == pixelThemeBackground.lowColor &&
                        std::abs(linearThemeTopPixel.red() - 200) <= 2 &&
                        std::abs(linearThemeTopPixel.green() - 100) <= 2 &&
                        std::abs(linearThemeBottomPixel.red() - 20) <= 2 &&
                        std::abs(linearThemeBottomPixel.blue() - 60) <= 2 &&
                        std::abs(radialThemeCenterPixel.red() - 200) <= 2 &&
                        std::abs(radialThemeCenterPixel.blue() - 50) <= 2 &&
                        radialThemeCornerPixel.red() < 30 &&
                        radialThemeCornerPixel.blue() > 55,
                    "QPainter Theme backgrounds must render known Single, top-to-bottom Linear, and center-to-edge Radial pixels");
    const QColor hdrWorldFallback = workbenchWorldBackgroundDisplayColor(
        QVector3D(1.0f, 0.25f, 0.04f), 1, 2.0, 1.0);
    passed &= check(hdrWorldFallback.red() == 255 &&
                        hdrWorldFallback.green() == 255 &&
                        hdrWorldFallback.blue() >= 110 &&
                        hdrWorldFallback.blue() <= 112,
                    "Workbench World display transform helper must apply exposure and Standard conversion before QColor clamps HDR channels");
    const QColor viewportWorldColor = workbenchWorldBackgroundViewportColor(
        QVector3D(1.0f, 0.25f, 0.04f));
    passed &= check(viewportWorldColor.red() == 255 &&
                        std::abs(viewportWorldColor.green() - 137) <= 1 &&
                        std::abs(viewportWorldColor.blue() - 56) <= 1,
                    "QPainter viewport World background must preserve Blender's scene-linear color with only framebuffer sRGB encoding");
    const QVector3D defaultCubeFace = workbenchStudioShade(
        defaultMaterialColor, QVector3D(0.0f, 0.0f, 1.0f),
        QVector3D(0.0f, 0.0f, 1.0f));
    const QVector3D defaultCubeFaceAgx =
        workbenchSceneLinearToAgxSrgb(defaultCubeFace);
    const QVector3D agxExposureHdr = workbenchSceneLinearToDisplaySrgb(
        QVector3D(0.18f, 0.18f, 0.18f), 0, 4.0, 1.0);
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
                                .length() < 0.01f &&
                        std::abs(agxExposureHdr.x() - 0.8863138f) < 0.001f &&
                        std::abs(agxExposureHdr.y() - 0.8863138f) < 0.001f &&
                        std::abs(agxExposureHdr.z() - 0.8863138f) < 0.001f,
                    "default solid cube shading and exposure-driven HDR AgX must match Blender's Workbench transform");

    const QVector3D gammaZero = workbenchSceneLinearToDisplaySrgb(
        QVector3D(0.18f, 1.0f, 4.0f), 1, 0.0, 0.0);
    passed &= check(gammaZero.x() == 0.0f && gammaZero.y() == 1.0f &&
                        gammaZero.z() == 1.0f &&
                        std::isfinite(gammaZero.x()) &&
                        std::isfinite(gammaZero.y()) &&
                        std::isfinite(gammaZero.z()),
                    "Blender gamma zero must use the FLT_EPSILON exponent without overflowing HDR display values");

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

    WorkbenchRenderResult borderCropResult;
    borderCropResult.outputSize = QSize(8, 6);
    borderCropResult.combined = QImage(8, 6, QImage::Format_RGBA8888);
    borderCropResult.combined.fill(QColor(20, 40, 60, 255));
    borderCropResult.depth.fill(3.0f, 48);
    borderCropResult.sceneLinearSize = QSize(8, 6);
    borderCropResult.sceneLinearRgba.fill(0.25f, 48 * 4);
    const bool nonCropBorderApplied = applyWorkbenchRenderBorder(
        &borderCropResult, true, false, 0.25, 0.25, 0.75, 0.75);
    passed &= check(nonCropBorderApplied &&
                        borderCropResult.outputSize == QSize(8, 6) &&
                        borderCropResult.combined.pixelColor(3, 2).alpha() == 255 &&
                        borderCropResult.combined.pixelColor(0, 0).alpha() == 0 &&
                        borderCropResult.depth[0] == 1.0e10f &&
                        borderCropResult.depth[2 * 8 + 1] == 1.0e10f &&
                        borderCropResult.depth[2 * 8 + 6] == 1.0e10f &&
                        borderCropResult.depth[5 * 8 + 3] == 1.0e10f &&
                        borderCropResult.depth[2 * 8 + 3] == 3.0f &&
                        borderCropResult.sceneLinearRgba[0] == 0.0f &&
                        borderCropResult.sceneLinearRgba[(2 * 8 + 3) * 4] == 0.25f,
                    "non-cropped Blender render borders must retain the full frame and clear pixels outside the selected region");
    const auto makeFractionalBorderResult = []() {
        WorkbenchRenderResult result;
        result.outputSize = QSize(8, 6);
        result.combined = QImage(8, 6, QImage::Format_RGBA8888);
        result.depth.resize(48);
        result.sceneLinearSize = QSize(8, 6);
        result.sceneLinearRgba.resize(48 * 4);
        for (int y = 0; y < 6; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int index = y * 8 + x;
                result.combined.setPixel(
                    x, y, qRgba(20 + x, 40 + y, 60 + index, 255));
                result.depth[index] = static_cast<float>(index) + 0.5f;
                result.sceneLinearRgba[index * 4] = index + 0.25f;
                result.sceneLinearRgba[index * 4 + 1] = index + 0.5f;
                result.sceneLinearRgba[index * 4 + 2] = index + 0.75f;
                result.sceneLinearRgba[index * 4 + 3] = 1.0f;
            }
        }
        return result;
    };
    WorkbenchRenderResult fractionalBorderResult = makeFractionalBorderResult();
    WorkbenchRenderResult fractionalBorderCropResult = makeFractionalBorderResult();
    constexpr qreal borderMinX = 0.21;
    constexpr qreal borderMinY = 0.31;
    constexpr qreal borderMaxX = 0.82;
    constexpr qreal borderMaxY = 0.72;
    const bool fractionalBorderApplied = applyWorkbenchRenderBorder(
        &fractionalBorderResult, true, false,
        borderMinX, borderMinY, borderMaxX, borderMaxY);
    const bool fractionalBorderCropApplied = applyWorkbenchRenderBorder(
        &fractionalBorderCropResult, true, true,
        borderMinX, borderMinY, borderMaxX, borderMaxY);
    bool fractionalBorderPassesMatch = true;
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) {
            const int fullIndex = y * 8 + x;
            const bool inside = x >= 1 && x < 6 && y >= 2 && y < 5;
            if (!inside) {
                fractionalBorderPassesMatch &=
                    fractionalBorderResult.combined.pixelColor(x, y).alpha() == 0 &&
                    fractionalBorderResult.depth[fullIndex] == 1.0e10f;
                for (int channel = 0; channel < 4; ++channel) {
                    fractionalBorderPassesMatch &=
                        fractionalBorderResult.sceneLinearRgba[fullIndex * 4 +
                                                               channel] == 0.0f;
                }
                continue;
            }
            const int croppedX = x - 1;
            const int croppedY = y - 2;
            const int croppedIndex = croppedY * 5 + croppedX;
            fractionalBorderPassesMatch &=
                fractionalBorderCropResult.combined.pixel(x - 1, y - 2) ==
                    fractionalBorderResult.combined.pixel(x, y) &&
                fractionalBorderCropResult.depth[croppedIndex] ==
                    fractionalBorderResult.depth[fullIndex];
            for (int channel = 0; channel < 4; ++channel) {
                fractionalBorderPassesMatch &=
                    fractionalBorderCropResult.sceneLinearRgba[
                        croppedIndex * 4 + channel] ==
                    fractionalBorderResult.sceneLinearRgba[
                        fullIndex * 4 + channel];
            }
        }
    }
    passed &= check(fractionalBorderApplied && fractionalBorderCropApplied &&
                        fractionalBorderResult.outputSize == QSize(8, 6) &&
                        fractionalBorderCropResult.outputSize == QSize(5, 3) &&
                        fractionalBorderCropResult.combined.size() == QSize(5, 3) &&
                        fractionalBorderCropResult.depth.size() == 15 &&
                        fractionalBorderCropResult.sceneLinearSize == QSize(5, 3) &&
                        fractionalBorderCropResult.sceneLinearRgba.size() == 60 &&
                        fractionalBorderPassesMatch,
                    "fractional Blender render borders must truncate asymmetric bottom-left bounds to x=[1,6), y=[2,5), clear exterior Combined/depth/linear data, and crop every pass to the same region");
    WorkbenchRenderResult croppedBorderResult;
    croppedBorderResult.outputSize = QSize(8, 6);
    croppedBorderResult.combined = QImage(8, 6, QImage::Format_RGBA8888);
    croppedBorderResult.combined.fill(QColor(20, 40, 60, 255));
    croppedBorderResult.depth.fill(3.0f, 48);
    croppedBorderResult.sceneLinearSize = QSize(8, 6);
    croppedBorderResult.sceneLinearRgba.fill(0.25f, 48 * 4);
    const bool cropBorderApplied = applyWorkbenchRenderBorder(
        &croppedBorderResult, true, true, 0.25, 1.0 / 6.0, 0.75, 2.0 / 3.0);
    passed &= check(cropBorderApplied &&
                        croppedBorderResult.outputSize == QSize(4, 3) &&
                        croppedBorderResult.combined.size() == QSize(4, 3) &&
                        croppedBorderResult.combined.pixelColor(0, 0) ==
                            QColor(20, 40, 60, 255) &&
                        croppedBorderResult.depth.size() == 12 &&
                        croppedBorderResult.depth.first() == 3.0f &&
                        croppedBorderResult.sceneLinearSize == QSize(4, 3) &&
                        croppedBorderResult.sceneLinearRgba.size() == 48 &&
                        croppedBorderResult.sceneLinearRgba.first() == 0.25f,
                    "cropped Blender render borders must crop every pass without changing camera framing");
    passed &= check(!applyWorkbenchRenderBorder(
                        &croppedBorderResult, true, true, -0.1, 0.0, 0.5, 1.0),
                    "invalid normalized render-border coordinates must be rejected");

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
    int bronzeLayerWidth = 0;
    int bronzeLayerHeight = 0;
    const QByteArray bronzeLinearLayers = workbenchMatcapLinearHalfData(
        QStringLiteral("metal_bronze"), &bronzeLayerWidth, &bronzeLayerHeight);
    const float exactBronzeCenterResponse = workbenchMatcapShade(
        QStringLiteral("metal_bronze"), QVector3D(0.8f, 0.8f, 0.8f),
        QVector3D(0.0f, 0.0f, 1.0f),
        QVector3D(0.0f, 0.0f, 1.0f)).x();
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
    WorkbenchStudioLighting axisMappingSource;
    axisMappingSource.lights[0].enabled = true;
    axisMappingSource.lights[0].direction = QVector3D(0.2f, 0.3f, 0.4f);
    const WorkbenchStudioLighting axisMappedWorldLighting =
        workbenchStudioLightingForView(
            axisMappingSource, 0, true,
            QVector3D(1.0f, 0.0f, 0.0f),
            QVector3D(0.0f, 1.0f, 0.0f),
            QVector3D(0.0f, 0.0f, 1.0f));
    const WorkbenchStudioLighting rotatedAxisMappedWorldLighting =
        workbenchStudioLightingForView(
            axisMappingSource, 90, true,
            QVector3D(1.0f, 0.0f, 0.0f),
            QVector3D(0.0f, 1.0f, 0.0f),
            QVector3D(0.0f, 0.0f, 1.0f));
    const WorkbenchStudioLighting unchangedCameraLighting =
        workbenchStudioLightingForView(
            axisMappingSource, 0, false,
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
                        bronzeLayerWidth == 512 && bronzeLayerHeight == 512 &&
                        bronzeLinearLayers.size() == 512 * 512 * 3 * 2 * 2 &&
                        std::abs(exactBronzeCenterResponse - 0.25338745f) <
                            1.0e-5f &&
                        std::abs(cameraFollowingRotatedLighting.lights[1].direction.x() -
                                 workbenchLighting.lights[1].direction.x()) < 1.0e-6f &&
                        std::abs(worldRotatedLighting.lights[1].direction.x() -
                                 workbenchLighting.lights[1].direction.x()) > 0.1f,
                    "Workbench Studio Light presets and diffuse/specular MatCap layers must load, with rotation applied only to world-space lighting");
    passed &= check(
        std::abs(axisMappedWorldLighting.lights[0].direction.x() - 0.2f) <
                1.0e-6f &&
            std::abs(axisMappedWorldLighting.lights[0].direction.y() + 0.4f) <
                1.0e-6f &&
            std::abs(axisMappedWorldLighting.lights[0].direction.z() - 0.3f) <
                1.0e-6f &&
            std::abs(rotatedAxisMappedWorldLighting.lights[0].direction.x() +
                     0.4f) < 1.0e-6f &&
            std::abs(rotatedAxisMappedWorldLighting.lights[0].direction.y() +
                     0.2f) < 1.0e-6f &&
            std::abs(rotatedAxisMappedWorldLighting.lights[0].direction.z() -
                     0.3f) < 1.0e-6f &&
            unchangedCameraLighting.lights[0].direction ==
                axisMappingSource.lights[0].direction,
        "world-space Studio directions must apply Blender's (x, -z, y) axis remap while camera-following preset vectors remain unchanged");

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
    const ViewportThemeBackground defaultThemeBackground;
    ViewportThemeBackground changedThemeBackground = defaultThemeBackground;
    changedThemeBackground.style = ViewportThemeBackgroundStyle::Linear;
    changedThemeBackground.highColor = QColor(90, 110, 130);
    changedThemeBackground.lowColor = QColor(24, 32, 40);
    const bool themeBackgroundInvalidatesAa =
        viewportAaHistoryKey(aaHistoryFrame, aaHistoryShading, 8, 1.0, {},
                             std::nullopt, defaultThemeBackground) !=
        viewportAaHistoryKey(aaHistoryFrame, aaHistoryShading, 8, 1.0, {},
                             std::nullopt, changedThemeBackground);
    ViewportShadingSettings worldBackgroundShading = aaHistoryShading;
    worldBackgroundShading.backgroundMode = ViewportBackgroundMode::World;
    const bool unusedThemeBackgroundDoesNotInvalidateAa =
        viewportAaHistoryKey(aaHistoryFrame, worldBackgroundShading, 8, 1.0,
                             {}, std::nullopt, defaultThemeBackground) ==
        viewportAaHistoryKey(aaHistoryFrame, worldBackgroundShading, 8, 1.0,
                             {}, std::nullopt, changedThemeBackground);
    passed &= check(meshGeometryInvalidatesAa &&
                        meshSelectionInvalidatesAa &&
                        meshDepthModeInvalidatesAa &&
                        meshMaterialInvalidatesAa &&
                        meshColorInvalidatesAa &&
                        meshTextureInvalidatesAa &&
                        meshAddRemoveInvalidatesAa &&
                        nurbsTextureInvalidatesAa &&
                        worldBackgroundInvalidatesAa &&
                        themeBackgroundInvalidatesAa &&
                        unusedThemeBackgroundDoesNotInvalidateAa,
                    "temporal AA history keys must change for mesh edits, selection, visibility mode, materials, textures, scene membership, and active background colors");

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
        QColor leftSample;
        QColor rightSample;
        QVector3D meanColor;
        qreal meanAlpha = 0.0;
    };
    const auto renderedSurface = [&](const Shape &shape,
                                     const ViewportShadingSettings &settings,
                                     bool selected = false,
                                     const ViewportDepthGeometry *preparedGeometry =
                                         nullptr,
                                     std::optional<WorkbenchMaterialData> material =
                                         std::nullopt) {
        QImage image(surfaceViewportSize,
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surfaceRenderer.setShadingSettings(settings);
        QPainter painter(&image);
        surfaceRenderer.drawShape(painter, shape, surfaceViewportSize,
                                 false, selected, true, QColor(), QString(),
                                 0.0, ObjectId::invalid(), 0, preparedGeometry,
                                 Point3D{}, std::nullopt, material);
        painter.end();
        int coveredPixels = 0;
        QVector3D summedColor;
        qreal summedAlpha = 0.0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 0) {
                    ++coveredPixels;
                    summedColor += QVector3D(pixel.redF(), pixel.greenF(),
                                             pixel.blueF());
                    summedAlpha += pixel.alphaF();
                }
            }
        }
        return RenderedSurface{
            coveredPixels, image.pixelColor(80, 80), image.pixelColor(54, 100),
            image.pixelColor(106, 100),
            coveredPixels > 0 ? summedColor / float(coveredPixels)
                              : QVector3D{},
            coveredPixels > 0 ? summedAlpha / coveredPixels : 0.0};
    };
    const RenderedSurface wireframeSurface = renderedSurface(surfaceShape,
                                                               shading);
    ViewportShadingSettings solidShading;
    solidShading.mode = ViewportShadingMode::Solid;
    const RenderedSurface solidSurface = renderedSurface(surfaceShape,
                                                          solidShading);
    const RenderedSurface selectedSolidSurface = renderedSurface(
        surfaceShape, solidShading, true);
    ViewportShadingSettings matcapShading = solidShading;
    matcapShading.lightingMode = ViewportLightingMode::MatCap;
    matcapShading.matcapPreset = QStringLiteral("metal_bronze");
    const RenderedSurface matcapSurface = renderedSurface(surfaceShape,
                                                            matcapShading);
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

    NurbsSurface3D tiltedSurface = fillSurface;
    for (Point3D &point : tiltedSurface.controlPoints) {
        point.z = point.x * 0.7;
    }
    Shape tiltedNurbsShape;
    tiltedNurbsShape.geometryType = GeometryType::NurbsSurface;
    tiltedNurbsShape.nurbsSurface = tiltedSurface;
    ViewportShadingSettings matcapFlipSettings = matcapShading;
    matcapFlipSettings.matcapPreset = QStringLiteral("metal_bronze");
    matcapFlipSettings.specularLighting = true;
    const RenderedSurface tiltedNurbsNormal = renderedSurface(
        tiltedNurbsShape, matcapFlipSettings);
    matcapFlipSettings.matcapFlipX = true;
    const RenderedSurface tiltedNurbsFlipped = renderedSurface(
        tiltedNurbsShape, matcapFlipSettings);
    passed &= check(tiltedNurbsNormal.coveredPixels > 0 &&
                        tiltedNurbsFlipped.coveredPixels > 0 &&
                        (tiltedNurbsNormal.meanColor -
                         tiltedNurbsFlipped.meanColor).length() > 0.01,
                    "QPainter MatCap Flip X must mirror asymmetric MatCap shading for NURBS surfaces");
    Shape tiltedMeshShape;
    tiltedMeshShape.geometryType = GeometryType::PolygonMesh;
    tiltedMeshShape.polygonMesh.vertices = {
        {-1.0, -1.0, -0.7}, {1.0, -1.0, 0.7},
        {1.0, 1.0, 0.7}, {-1.0, 1.0, -0.7}};
    tiltedMeshShape.polygonMesh.faces = {{0, 1, 2, 3}};
    tiltedMeshShape.polygonMesh.faceMaterialIndices = {0};
    matcapFlipSettings.matcapFlipX = false;
    const RenderedSurface tiltedMeshNormal = renderedSurface(
        tiltedMeshShape, matcapFlipSettings);
    matcapFlipSettings.matcapFlipX = true;
    const RenderedSurface tiltedMeshFlipped = renderedSurface(
        tiltedMeshShape, matcapFlipSettings);
    passed &= check(tiltedMeshNormal.coveredPixels > 0 &&
                        tiltedMeshFlipped.coveredPixels > 0 &&
                        (tiltedMeshNormal.meanColor -
                         tiltedMeshFlipped.meanColor).length() > 0.01,
                    "QPainter MatCap Flip X must change asymmetric MatCap shading for polygon meshes");
    const QVector3D tiltedMeshNormalWorld = QVector3D(-2.8f, 0.0f, 4.0f).normalized();
    Shape opaqueFrontFacingMesh = tiltedMeshShape;
    opaqueFrontFacingMesh.polygonMesh.cornerNormals.fill(
        {tiltedMeshNormalWorld.x(), tiltedMeshNormalWorld.y(),
         tiltedMeshNormalWorld.z()}, 4);
    Shape opaqueBackFacingMesh = opaqueFrontFacingMesh;
    opaqueBackFacingMesh.polygonMesh.faces = {{3, 2, 1, 0}};
    opaqueBackFacingMesh.polygonMesh.cornerNormals.fill(
        {-tiltedMeshNormalWorld.x(), -tiltedMeshNormalWorld.y(),
         -tiltedMeshNormalWorld.z()}, 4);
    ViewportShadingSettings meshBackfaceSettings = solidShading;
    meshBackfaceSettings.lightingMode = ViewportLightingMode::Studio;
    const RenderedSurface opaqueMeshFront = renderedSurface(
        opaqueFrontFacingMesh, meshBackfaceSettings);
    const RenderedSurface opaqueMeshBack = renderedSurface(
        opaqueBackFacingMesh, meshBackfaceSettings);
    opaqueFrontFacingMesh.polygonMesh.materialSlots = {
        PolygonMeshMaterialSlot{}};
    opaqueBackFacingMesh.polygonMesh.materialSlots = {
        PolygonMeshMaterialSlot{}};
    opaqueFrontFacingMesh.polygonMesh.materialSlots[0].alpha = 0.5f;
    opaqueBackFacingMesh.polygonMesh.materialSlots[0].alpha = 0.5f;
    const RenderedSurface transparentMeshFront = renderedSurface(
        opaqueFrontFacingMesh, meshBackfaceSettings);
    const RenderedSurface transparentMeshBack = renderedSurface(
        opaqueBackFacingMesh, meshBackfaceSettings);
    const QVector3D opaqueMeshFrontRgb(
        opaqueMeshFront.centerColor.redF(), opaqueMeshFront.centerColor.greenF(),
        opaqueMeshFront.centerColor.blueF());
    const QVector3D opaqueMeshBackRgb(
        opaqueMeshBack.centerColor.redF(), opaqueMeshBack.centerColor.greenF(),
        opaqueMeshBack.centerColor.blueF());
    const QVector3D transparentMeshFrontRgb(
        transparentMeshFront.centerColor.redF(),
        transparentMeshFront.centerColor.greenF(),
        transparentMeshFront.centerColor.blueF());
    const QVector3D transparentMeshBackRgb(
        transparentMeshBack.centerColor.redF(),
        transparentMeshBack.centerColor.greenF(),
        transparentMeshBack.centerColor.blueF());
    passed &= check(
        opaqueMeshFront.coveredPixels > 0 && opaqueMeshBack.coveredPixels > 0,
        "Opaque QPainter mesh backface fixture must be covered");
    passed &= check(
        (opaqueMeshFrontRgb - opaqueMeshBackRgb).length() < 0.01f,
        "QPainter opaque mesh backface shading must flip the source normal");
    passed &= check(
        transparentMeshFront.coveredPixels > 0 &&
            transparentMeshBack.coveredPixels > 0,
        "Transparent QPainter mesh backface fixture must be covered");
    passed &= check(
        (transparentMeshFrontRgb - transparentMeshBackRgb).length() > 0.02f,
        "QPainter transparent mesh shading must preserve the source normal");
    passed &= check(
        std::abs(transparentMeshFront.meanAlpha -
                 transparentMeshBack.meanAlpha) < 0.01,
        "QPainter mesh backface normal handling must preserve transparent opacity");

    ViewportDepthGeometry smoothNurbsGeometry;
    const auto smoothNurbsPointAt = [&](const QPointF &screenPoint) {
        QPointF worldPoint;
        surfaceTransform.screenToWorkPlaneUnclipped(
            screenPoint, surfaceViewportSize, WorkPlane::XY, 0.0,
            &worldPoint);
        return QVector3D(float(worldPoint.x()), float(worldPoint.y()), 0.0f);
    };
    smoothNurbsGeometry.surfaceVertices = {
        smoothNurbsPointAt(QPointF(32.0, 128.0)),
        smoothNurbsPointAt(QPointF(128.0, 128.0)),
        smoothNurbsPointAt(QPointF(80.0, 32.0))};
    const QVector3D leftSmoothNormal(-0.8f, 0.0f, 0.6f);
    const QVector3D rightSmoothNormal(0.8f, 0.0f, 0.6f);
    const QVector3D topSmoothNormal(0.0f, 0.0f, 1.0f);
    smoothNurbsGeometry.surfaceNormals = {
        leftSmoothNormal, rightSmoothNormal, topSmoothNormal};
    ViewportDepthGeometry flatNurbsGeometry = smoothNurbsGeometry;
    flatNurbsGeometry.surfaceNormals =
        QVector<QVector3D>(3, QVector3D(0.0f, 0.0f, 1.0f));
    ViewportShadingSettings smoothNurbsSettings = solidShading;
    smoothNurbsSettings.lightingMode = ViewportLightingMode::Studio;
    const RenderedSurface smoothNurbsRender = renderedSurface(
        surfaceShape, smoothNurbsSettings, false, &smoothNurbsGeometry);
    const RenderedSurface flatNurbsRender = renderedSurface(
        surfaceShape, smoothNurbsSettings, false, &flatNurbsGeometry);
    const QVector3D smoothNurbsLeftRgb(
        smoothNurbsRender.leftSample.redF(),
        smoothNurbsRender.leftSample.greenF(),
        smoothNurbsRender.leftSample.blueF());
    const QVector3D smoothNurbsRightRgb(
        smoothNurbsRender.rightSample.redF(),
        smoothNurbsRender.rightSample.greenF(),
        smoothNurbsRender.rightSample.blueF());
    const QVector3D flatNurbsLeftRgb(
        flatNurbsRender.leftSample.redF(), flatNurbsRender.leftSample.greenF(),
        flatNurbsRender.leftSample.blueF());
    const QVector3D flatNurbsRightRgb(
        flatNurbsRender.rightSample.redF(),
        flatNurbsRender.rightSample.greenF(),
        flatNurbsRender.rightSample.blueF());
    passed &= check(
        smoothNurbsRender.coveredPixels > 0 &&
            (smoothNurbsLeftRgb - smoothNurbsRightRgb).length() > 0.02f &&
            (flatNurbsLeftRgb - flatNurbsRightRgb).length() < 0.01f,
        "QPainter prepared untextured NURBS triangles must interpolate smooth corner normals per sample");

    WorkbenchMaterialData smoothNurbsTextureMaterial;
    smoothNurbsTextureMaterial.textureImageSceneLinearFloatWidth = 1;
    smoothNurbsTextureMaterial.textureImageSceneLinearFloatHeight = 1;
    const std::array<float, 4> smoothNurbsTexturePixel{0.72f, 0.72f, 0.72f,
                                                       1.0f};
    smoothNurbsTextureMaterial.textureImageSceneLinearFloatDataBase64 =
        QString::fromLatin1(QByteArray(
            reinterpret_cast<const char *>(smoothNurbsTexturePixel.data()),
            qsizetype(sizeof(smoothNurbsTexturePixel))).toBase64());
    smoothNurbsTextureMaterial.textureImageInterpolation =
        QStringLiteral("Closest");
    ViewportShadingSettings texturedSmoothNurbsSettings = smoothNurbsSettings;
    texturedSmoothNurbsSettings.colorMode = ViewportColorMode::Texture;
    const RenderedSurface texturedSmoothNurbsRender = renderedSurface(
        surfaceShape, texturedSmoothNurbsSettings, false, &smoothNurbsGeometry,
        smoothNurbsTextureMaterial);
    const RenderedSurface texturedFlatNurbsRender = renderedSurface(
        surfaceShape, texturedSmoothNurbsSettings, false, &flatNurbsGeometry,
        smoothNurbsTextureMaterial);
    passed &= check(
        texturedSmoothNurbsRender.coveredPixels > 0 &&
            (QVector3D(texturedSmoothNurbsRender.leftSample.redF(),
                       texturedSmoothNurbsRender.leftSample.greenF(),
                       texturedSmoothNurbsRender.leftSample.blueF()) -
             QVector3D(texturedSmoothNurbsRender.rightSample.redF(),
                       texturedSmoothNurbsRender.rightSample.greenF(),
                       texturedSmoothNurbsRender.rightSample.blueF())).length() >
                0.02f &&
            (QVector3D(texturedFlatNurbsRender.leftSample.redF(),
                       texturedFlatNurbsRender.leftSample.greenF(),
                       texturedFlatNurbsRender.leftSample.blueF()) -
             QVector3D(texturedFlatNurbsRender.rightSample.redF(),
                       texturedFlatNurbsRender.rightSample.greenF(),
                       texturedFlatNurbsRender.rightSample.blueF())).length() <
                0.01f,
        "QPainter textured prepared NURBS triangles must apply interpolated smooth corner normals in Studio shading");

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

    MeshColorAttribute defaultCornerColors = cornerColors;
    defaultCornerColors.name = QStringLiteral("RenderColor");
    defaultCornerColors.values = {0.05f, 0.85f, 0.05f, 1.0f,
                                  0.05f, 0.85f, 0.05f, 1.0f,
                                  0.05f, 0.85f, 0.05f, 1.0f};
    colorSourceMesh.polygonMesh.colorAttributes.append(defaultCornerColors);
    colorSourceMesh.polygonMesh.activeColorAttributeName.clear();
    colorSourceMesh.polygonMesh.defaultColorAttributeName =
        defaultCornerColors.name;
    const QImage defaultAttributeImage = renderColorMesh(attributeSettings, {});
    const QColor defaultAttributePixel = defaultAttributeImage.pixelColor(
        qRound(nearRedCorner.x()), qRound(nearRedCorner.y()));
    passed &= check(defaultAttributePixel.green() >
                        defaultAttributePixel.red() + 30 &&
                        defaultAttributePixel.green() >
                        defaultAttributePixel.blue() + 30,
                    "QPainter Attribute mode must use Blender's default color attribute when no active attribute is set");

    Shape faceColorSourceMesh = colorSourceMesh;
    faceColorSourceMesh.polygonMesh.vertices = {
        {-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0},
        {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
    faceColorSourceMesh.polygonMesh.faces = {{0, 1, 2}, {0, 2, 3}};
    faceColorSourceMesh.polygonMesh.faceMaterialIndices = {0, 0};
    faceColorSourceMesh.polygonMesh.uvMaps.clear();
    faceColorSourceMesh.polygonMesh.activeUvMapName.clear();
    faceColorSourceMesh.polygonMesh.defaultUvMapName.clear();
    faceColorSourceMesh.polygonMesh.colorAttributes.clear();
    MeshColorAttribute faceColors;
    faceColors.name = QStringLiteral("FaceColor");
    faceColors.domain = MeshAttributeDomain::Face;
    faceColors.colorSpace = QStringLiteral("scene_linear");
    faceColors.componentCount = 4;
    faceColors.values = {0.9f, 0.05f, 0.05f, 1.0f,
                         0.05f, 0.9f, 0.05f, 1.0f};
    faceColorSourceMesh.polygonMesh.colorAttributes = {faceColors};
    faceColorSourceMesh.polygonMesh.activeColorAttributeName = faceColors.name;
    faceColorSourceMesh.polygonMesh.defaultColorAttributeName.clear();
    const Shape originalColorSourceMesh = colorSourceMesh;
    colorSourceMesh = faceColorSourceMesh;
    const QImage faceAttributeImage = renderColorMesh(attributeSettings, {});
    colorSourceMesh = originalColorSourceMesh;
    QPointF firstFaceProbe;
    QPointF secondFaceProbe;
    const bool projectedFaceSamples =
        surfaceTransform.worldPointToScreen({0.5, -0.5, 0.0},
                                            surfaceViewportSize,
                                            &firstFaceProbe) &&
        surfaceTransform.worldPointToScreen({-0.5, 0.5, 0.0},
                                            surfaceViewportSize,
                                            &secondFaceProbe);
    const QColor firstFaceAttribute = faceAttributeImage.pixelColor(
        qRound(firstFaceProbe.x()), qRound(firstFaceProbe.y()));
    const QColor secondFaceAttribute = faceAttributeImage.pixelColor(
        qRound(secondFaceProbe.x()), qRound(secondFaceProbe.y()));
    passed &= check(projectedFaceSamples &&
                        firstFaceAttribute.red() >
                            firstFaceAttribute.green() + 30 &&
                        secondFaceAttribute.green() >
                            secondFaceAttribute.red() + 30 &&
                        firstFaceAttribute.alpha() > 240 &&
                        secondFaceAttribute.alpha() > 240,
                    "QPainter Attribute mode must select the constant color stored on each mesh Face");

    QTemporaryDir colorTextureDirectory;
    QImage sourceTexture(1, 1, QImage::Format_RGBA8888);
    sourceTexture.fill(QColor(220, 30, 20, 255));
    const QString colorTexturePath =
        colorTextureDirectory.filePath(QStringLiteral("source.png"));
    const bool savedColorTexture = sourceTexture.save(colorTexturePath);
    colorSourceMesh.polygonMesh.uvMaps = {
        {QStringLiteral("OtherUV"), {QPointF(0.0, 0.0), QPointF(0.0, 0.0),
                                     QPointF(0.0, 0.0)}},
        {QStringLiteral("RenderUV"), {QPointF(0.5, 0.5), QPointF(0.5, 0.5),
                                      QPointF(0.5, 0.5)}}};
    colorSourceMesh.polygonMesh.activeUvMapName.clear();
    colorSourceMesh.polygonMesh.defaultUvMapName = QStringLiteral("RenderUV");
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

    QImage nurbsSurfaceTexture(2, 2, QImage::Format_RGBA8888);
    nurbsSurfaceTexture.setPixelColor(0, 0, QColor(240, 20, 30, 255));
    nurbsSurfaceTexture.setPixelColor(1, 0, QColor(20, 220, 40, 255));
    nurbsSurfaceTexture.setPixelColor(0, 1, QColor(30, 40, 230, 255));
    nurbsSurfaceTexture.setPixelColor(1, 1, QColor(230, 210, 20, 255));
    const QString nurbsSurfaceTexturePath = colorTextureDirectory.filePath(
        QStringLiteral("nurbs-surface-texture.png"));
    const bool savedNurbsSurfaceTexture =
        nurbsSurfaceTexture.save(nurbsSurfaceTexturePath);
    NurbsSurface3D texturedNurbsSurface = fillSurface;
    texturedNurbsSurface.knotsU = {2.0, 6.0};
    texturedNurbsSurface.knotsV = {-3.0, 5.0};
    Shape texturedNurbsShape;
    texturedNurbsShape.geometryType = GeometryType::NurbsSurface;
    texturedNurbsShape.nurbsSurface = texturedNurbsSurface;
    WorkbenchMaterialData nurbsTextureMaterial;
    nurbsTextureMaterial.textureImagePath = nurbsSurfaceTexturePath;
    nurbsTextureMaterial.textureImageSrgb = false;
    nurbsTextureMaterial.textureImageInterpolation = QStringLiteral("Closest");
    nurbsTextureMaterial.textureImageExtension = QStringLiteral("CLIP");
    const auto renderTexturedNurbsSurface = [&]() {
        QImage image(surfaceViewportSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surfaceRenderer.setShadingSettings(textureSettings);
        QPainter painter(&image);
        surfaceRenderer.drawShape(
            painter, texturedNurbsShape, surfaceViewportSize, false, false,
            true, QColor(), QString(), 0.0, ObjectId::invalid(), 0, nullptr,
            {}, std::nullopt, nurbsTextureMaterial);
        painter.end();
        return image;
    };
    const QImage nurbsSurfaceTextureImage = renderTexturedNurbsSurface();
    QPointF nurbsTextureLowerLeftPoint;
    QPointF nurbsTextureLowerRightPoint;
    QPointF nurbsTextureUpperLeftPoint;
    QPointF nurbsTextureUpperRightPoint;
    const bool projectedNurbsTextureSamples =
        surfaceTransform.worldPointToScreen({-0.5, -0.5, 0.0},
                                            surfaceViewportSize,
                                            &nurbsTextureLowerLeftPoint) &&
        surfaceTransform.worldPointToScreen({0.5, -0.5, 0.0},
                                            surfaceViewportSize,
                                            &nurbsTextureLowerRightPoint) &&
        surfaceTransform.worldPointToScreen({-0.5, 0.5, 0.0},
                                            surfaceViewportSize,
                                            &nurbsTextureUpperLeftPoint) &&
        surfaceTransform.worldPointToScreen({0.5, 0.5, 0.0},
                                            surfaceViewportSize,
                                            &nurbsTextureUpperRightPoint);
    const auto nurbsTexturePixelAt = [&](const QPointF &point) {
        return nurbsSurfaceTextureImage.pixelColor(qRound(point.x()),
                                                   qRound(point.y()));
    };
    const QColor nurbsTextureLowerLeft =
        nurbsTexturePixelAt(nurbsTextureLowerLeftPoint);
    const QColor nurbsTextureLowerRight =
        nurbsTexturePixelAt(nurbsTextureLowerRightPoint);
    const QColor nurbsTextureUpperLeft =
        nurbsTexturePixelAt(nurbsTextureUpperLeftPoint);
    const QColor nurbsTextureUpperRight =
        nurbsTexturePixelAt(nurbsTextureUpperRightPoint);
    passed &= check(
        savedNurbsSurfaceTexture && validateNurbsSurface(texturedNurbsSurface) &&
            projectedNurbsTextureSamples &&
            nurbsTextureLowerLeft.blue() > nurbsTextureLowerLeft.red() + 35 &&
            nurbsTextureLowerLeft.blue() > nurbsTextureLowerLeft.green() + 35 &&
            nurbsTextureLowerRight.red() > nurbsTextureLowerRight.blue() + 35 &&
            nurbsTextureLowerRight.green() > nurbsTextureLowerRight.blue() + 35 &&
            nurbsTextureUpperLeft.red() > nurbsTextureUpperLeft.green() + 35 &&
            nurbsTextureUpperLeft.red() > nurbsTextureUpperLeft.blue() + 35 &&
            nurbsTextureUpperRight.green() > nurbsTextureUpperRight.red() + 35 &&
            nurbsTextureUpperRight.green() > nurbsTextureUpperRight.blue() + 35,
        "QPainter NURBS Texture mode must map asymmetric image texels to U/V quadrants across non-unit knot domains");

    const QVector3D originalNurbsTextureBaseColor =
        nurbsTextureMaterial.baseColorSceneLinear;
    nurbsTextureMaterial.baseColorSceneLinear = QVector3D(0.02f, 0.9f, 0.02f);
    const QImage tintedNurbsSurfaceTextureImage = renderTexturedNurbsSurface();
    nurbsTextureMaterial.baseColorSceneLinear = originalNurbsTextureBaseColor;
    const auto sameNurbsTextureSamples = [&](const QPointF &point) {
        const QColor baseline = nurbsSurfaceTextureImage.pixelColor(
            qRound(point.x()), qRound(point.y()));
        const QColor tinted = tintedNurbsSurfaceTextureImage.pixelColor(
            qRound(point.x()), qRound(point.y()));
        return std::abs(baseline.red() - tinted.red()) <= 1 &&
               std::abs(baseline.green() - tinted.green()) <= 1 &&
               std::abs(baseline.blue() - tinted.blue()) <= 1;
    };
    passed &= check(
        sameNurbsTextureSamples(nurbsTextureLowerLeftPoint) &&
            sameNurbsTextureSamples(nurbsTextureLowerRightPoint) &&
            sameNurbsTextureSamples(nurbsTextureUpperLeftPoint) &&
            sameNurbsTextureSamples(nurbsTextureUpperRightPoint),
        "QPainter NURBS Texture mode must replace material RGB with the image sample");

    // A translucent, constant texture over a planar NURBS quad exercises the
    // production prepared-surface edge resolve. Shared tessellation edges must
    // contribute one shadow/compositor fragment per pixel, not one per face.
    QImage translucentNurbsTexture(2, 2, QImage::Format_RGBA8888);
    translucentNurbsTexture.fill(QColor(180, 90, 40, 128));
    const QString translucentNurbsTexturePath = colorTextureDirectory.filePath(
        QStringLiteral("nurbs-translucent.png"));
    const bool savedTranslucentNurbsTexture =
        translucentNurbsTexture.save(translucentNurbsTexturePath);
    WorkbenchMaterialData translucentNurbsMaterial = nurbsTextureMaterial;
    translucentNurbsMaterial.textureImagePath = translucentNurbsTexturePath;
    translucentNurbsMaterial.alpha = 0.5f;
    QImage translucentNurbsImage(surfaceViewportSize,
                                 QImage::Format_ARGB32_Premultiplied);
    translucentNurbsImage.fill(Qt::transparent);
    RasterWorkbenchFragmentSink translucentNurbsFragments;
    surfaceRenderer.setShadingSettings(textureSettings);
    QPainter translucentNurbsPainter(&translucentNurbsImage);
    surfaceRenderer.drawShape(
        translucentNurbsPainter, texturedNurbsShape, surfaceViewportSize,
        false, false, true, QColor(), QString(), 0.0,
        ObjectId::fromValue(1), 0,
        nullptr, {}, std::nullopt, translucentNurbsMaterial,
        QString(), QString(), {}, {}, &translucentNurbsFragments);
    translucentNurbsPainter.end();
    QSet<quint64> translucentNurbsFragmentPixels;
    bool translucentNurbsFragmentsUnique = true;
    float translucentNurbsDiagonalAlpha = 0.0f;
    int translucentNurbsDiagonalFragments = 0;
    for (const RasterWorkbenchLinearFragment &fragment :
         translucentNurbsFragments) {
        const quint64 key = static_cast<quint64>(fragment.y) *
                                static_cast<quint64>(surfaceViewportSize.width()) +
                            static_cast<quint64>(fragment.x);
        translucentNurbsFragmentsUnique &=
            !translucentNurbsFragmentPixels.contains(key);
        translucentNurbsFragmentPixels.insert(key);
        if (fragment.x == surfaceViewportSize.width() / 2 &&
            fragment.y == surfaceViewportSize.height() / 2) {
            translucentNurbsDiagonalAlpha = fragment.alpha;
            ++translucentNurbsDiagonalFragments;
        }
    }
    passed &= check(savedTranslucentNurbsTexture &&
                        !translucentNurbsFragments.empty() &&
                        translucentNurbsFragmentsUnique &&
                        static_cast<std::size_t>(
                            translucentNurbsFragmentPixels.size()) ==
                            translucentNurbsFragments.size() &&
                        translucentNurbsDiagonalFragments == 1 &&
                        std::abs(translucentNurbsDiagonalAlpha - 0.5f) < 0.02f &&
                        std::abs(translucentNurbsImage.pixelColor(
                                     surfaceViewportSize.width() / 2,
                                     surfaceViewportSize.height() / 2).alphaF() -
                                 0.5f) < 0.02f,
                    "QPainter textured planar NURBS tessellation must aggregate its shared fractional edge into one half-alpha fragment over a pixel");
    const QString translucentNurbsCapturePath = qEnvironmentVariable(
        "CLASSICAD_VIEWPORT_QPAINTER_NURBS_SHARED_EDGE_CAPTURE");
    if (!translucentNurbsCapturePath.isEmpty()) {
        passed &= check(
            translucentNurbsImage.size() == surfaceViewportSize &&
                translucentNurbsImage.save(translucentNurbsCapturePath),
            "opt-in QPainter translucent NURBS shared-edge capture must save");
    }
    const QString translucentMeshCapturePath = qEnvironmentVariable(
        "CLASSICAD_VIEWPORT_QPAINTER_MESH_SHARED_EDGE_CAPTURE");
    if (!translucentMeshCapturePath.isEmpty()) {
        QImage translucentMeshTexture(2, 2, QImage::Format_RGBA8888);
        translucentMeshTexture.fill(QColor(180, 90, 40, 255));
        const QString translucentMeshTexturePath =
            colorTextureDirectory.filePath(
                QStringLiteral("mesh-translucent-edge.png"));
        const bool savedTranslucentMeshTexture =
            translucentMeshTexture.save(translucentMeshTexturePath);
        Shape translucentMeshShape;
        translucentMeshShape.geometryType = GeometryType::PolygonMesh;
        translucentMeshShape.polygonMesh.vertices = {
            {-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0},
            {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
        translucentMeshShape.polygonMesh.faces = {{0, 1, 2}, {0, 2, 3}};
        translucentMeshShape.polygonMesh.faceMaterialIndices = {0, 0};
        translucentMeshShape.polygonMesh.materialSlotIds = {
            QStringLiteral("MeshTexture")};
        translucentMeshShape.polygonMesh.uvMaps = {
            {QStringLiteral("UVMap"),
             {QPointF(0.0, 0.0), QPointF(1.0, 0.0),
              QPointF(1.0, 1.0), QPointF(0.0, 0.0),
              QPointF(1.0, 1.0), QPointF(0.0, 1.0)}}};
        translucentMeshShape.polygonMesh.activeUvMapName =
            QStringLiteral("UVMap");
        translucentMeshShape.polygonMesh.defaultUvMapName =
            QStringLiteral("UVMap");
        WorkbenchMaterialData translucentMeshMaterial =
            translucentNurbsMaterial;
        translucentMeshMaterial.textureImagePath = translucentMeshTexturePath;
        translucentMeshMaterial.textureImageSrgb = false;
        translucentMeshMaterial.textureImageAlphaMode =
            QStringLiteral("STRAIGHT");
        translucentMeshMaterial.textureImageInterpolation =
            QStringLiteral("Linear");
        translucentMeshMaterial.textureImageExtension =
            QStringLiteral("CLIP");
        QImage translucentMeshImage(
            surfaceViewportSize, QImage::Format_ARGB32_Premultiplied);
        translucentMeshImage.fill(Qt::transparent);
        RasterWorkbenchFragmentSink translucentMeshFragments;
        surfaceRenderer.setWorkbenchRasterAntialiasingEnabled(false);
        surfaceRenderer.setShadingSettings(textureSettings);
        QPainter translucentMeshPainter(&translucentMeshImage);
        surfaceRenderer.drawShape(
            translucentMeshPainter, translucentMeshShape,
            surfaceViewportSize, false, false, true, QColor(), QString(), 0.0,
            ObjectId::fromValue(2), 0, nullptr, {}, std::nullopt,
            std::nullopt, QString(), QString(),
            identityMeshObjectTransform,
            {translucentMeshMaterial},
            &translucentMeshFragments);
        translucentMeshPainter.end();
        int meshDiagonalFragments = 0;
        float meshDiagonalAlpha = 0.0f;
        for (const RasterWorkbenchLinearFragment &fragment :
             translucentMeshFragments) {
            if (fragment.x == surfaceViewportSize.width() / 2 &&
                fragment.y == surfaceViewportSize.height() / 2) {
                ++meshDiagonalFragments;
                meshDiagonalAlpha = fragment.alpha;
            }
        }
        const QColor meshDiagonalPixel = translucentMeshImage.pixelColor(
            surfaceViewportSize.width() / 2,
            surfaceViewportSize.height() / 2);
        const bool savedTranslucentMeshCapture =
            savedTranslucentMeshTexture &&
            validatePolygonMesh(translucentMeshShape.polygonMesh) &&
            translucentMeshImage.save(translucentMeshCapturePath);
        passed &= check(
            savedTranslucentMeshCapture,
            "opt-in QPainter translucent mesh shared-edge capture must save");
        passed &= check(
            meshDiagonalFragments == 1 &&
                std::abs(meshDiagonalAlpha - 0.5f) < 0.02f &&
                std::abs(meshDiagonalPixel.alphaF() - 0.5f) < 0.02f,
            "QPainter translucent mesh shared diagonal must have one half-alpha contribution");
        surfaceRenderer.setWorkbenchRasterAntialiasingEnabled(true);
    }
    ViewportTransform perspectiveNurbsTransform = surfaceTransform;
    perspectiveNurbsTransform.zoom() = 17.28;
    perspectiveNurbsTransform.setViewDirection({4.0, -6.0, 4.0});
    perspectiveNurbsTransform.setPerspectiveEnabled(true);
    CurveHitTester perspectiveNurbsHitTester;
    ViewportRenderer perspectiveNurbsRenderer(perspectiveNurbsTransform,
                                               perspectiveNurbsHitTester);
    QImage perspectiveTranslucentNurbsImage(
        surfaceViewportSize, QImage::Format_ARGB32_Premultiplied);
    perspectiveTranslucentNurbsImage.fill(Qt::transparent);
    RasterWorkbenchFragmentSink perspectiveTranslucentNurbsFragments;
    perspectiveNurbsRenderer.setShadingSettings(textureSettings);
    QPainter perspectiveTranslucentNurbsPainter(
        &perspectiveTranslucentNurbsImage);
    perspectiveNurbsRenderer.drawShape(
        perspectiveTranslucentNurbsPainter, texturedNurbsShape,
        surfaceViewportSize, false, false, true, QColor(), QString(), 0.0,
        ObjectId::fromValue(1), 0, nullptr, {}, std::nullopt,
        translucentNurbsMaterial, QString(), QString(), {}, {},
        &perspectiveTranslucentNurbsFragments);
    perspectiveTranslucentNurbsPainter.end();
    QSet<quint64> perspectiveTranslucentNurbsFragmentPixels;
    bool perspectiveTranslucentNurbsFragmentsUnique = true;
    for (const RasterWorkbenchLinearFragment &fragment :
         perspectiveTranslucentNurbsFragments) {
        const quint64 key = static_cast<quint64>(fragment.y) *
                                static_cast<quint64>(surfaceViewportSize.width()) +
                            static_cast<quint64>(fragment.x);
        perspectiveTranslucentNurbsFragmentsUnique &=
            !perspectiveTranslucentNurbsFragmentPixels.contains(key);
        perspectiveTranslucentNurbsFragmentPixels.insert(key);
    }
    const QString perspectiveTranslucentNurbsCapturePath = qEnvironmentVariable(
        "CLASSICAD_VIEWPORT_QPAINTER_NURBS_PERSPECTIVE_CAPTURE");
    if (!perspectiveTranslucentNurbsCapturePath.isEmpty()) {
        passed &= check(
            perspectiveTranslucentNurbsImage.size() == surfaceViewportSize &&
                perspectiveTranslucentNurbsImage.save(
                    perspectiveTranslucentNurbsCapturePath),
            "opt-in QPainter perspective NURBS edge capture must save");
    }
    QPointF perspectiveNurbsOriginScreen;
    const bool perspectiveNurbsOriginProjected =
        perspectiveNurbsTransform.worldPointToScreen(
            {0.0, 0.0, 0.0}, surfaceViewportSize,
            &perspectiveNurbsOriginScreen);
    passed &= check(
        perspectiveNurbsOriginProjected &&
        !perspectiveTranslucentNurbsFragments.empty() &&
            perspectiveTranslucentNurbsFragmentsUnique &&
            perspectiveTranslucentNurbsFragmentPixels.size() ==
                static_cast<qsizetype>(
                    perspectiveTranslucentNurbsFragments.size()) &&
            std::abs(perspectiveTranslucentNurbsImage.pixelColor(
                         qRound(perspectiveNurbsOriginScreen.x()),
                         qRound(perspectiveNurbsOriginScreen.y())).alphaF() -
                     0.5f) < 0.02f,
        "QPainter perspective planar NURBS must aggregate fractional coverage across its shared diagonal");

    QImage interpolationTexture(2, 2, QImage::Format_RGBA8888);
    interpolationTexture.setPixelColor(0, 0, QColor(240, 20, 30, 255));
    interpolationTexture.setPixelColor(1, 0, QColor(20, 220, 40, 255));
    interpolationTexture.setPixelColor(0, 1, QColor(30, 40, 230, 255));
    interpolationTexture.setPixelColor(1, 1, QColor(230, 210, 20, 255));
    const QString interpolationTexturePath =
        colorTextureDirectory.filePath(QStringLiteral("interpolation.png"));
    const bool savedInterpolationTexture =
        interpolationTexture.save(interpolationTexturePath);
    colorSourceMesh.polygonMesh.uvMaps = {
        {QStringLiteral("RenderUV"), {QPointF(0.0, 0.0), QPointF(1.0, 0.0),
                                      QPointF(0.5, 1.0)}}};
    colorSourceMesh.polygonMesh.defaultUvMapName = QStringLiteral("RenderUV");
    WorkbenchMaterialData filteredTextureMaterial;
    filteredTextureMaterial.textureImagePath = interpolationTexturePath;
    filteredTextureMaterial.textureImageSrgb = false;
    filteredTextureMaterial.textureImageExtension = QStringLiteral("CLIP");
    filteredTextureMaterial.textureImageInterpolation = QStringLiteral("Linear");
    const QImage linearTextureImage = renderColorMesh(
        textureSettings, {filteredTextureMaterial});
    filteredTextureMaterial.textureImageInterpolation = QStringLiteral("Cubic");
    const QImage cubicTextureImage = renderColorMesh(
        textureSettings, {filteredTextureMaterial});
    filteredTextureMaterial.textureImageInterpolation = QStringLiteral("Smart");
    const QImage smartTextureImage = renderColorMesh(
        textureSettings, {filteredTextureMaterial});
    filteredTextureMaterial.textureImageInterpolation = QStringLiteral("Closest");
    const QImage closestTextureImage = renderColorMesh(
        textureSettings, {filteredTextureMaterial});
    const int closestProbeX = qRound(nearGreenCorner.x());
    const int closestProbeY = qRound(nearGreenCorner.y());
    const QColor closestTexturePixel = closestTextureImage.pixelColor(
        closestProbeX, closestProbeY);
    const QColor linearTexturePixel = linearTextureImage.pixelColor(
        closestProbeX, closestProbeY);
    QImage closestReferenceTexture(1, 1, QImage::Format_RGBA8888);
    closestReferenceTexture.fill(interpolationTexture.pixelColor(1, 1));
    const QString closestReferencePath =
        colorTextureDirectory.filePath(QStringLiteral("closest-reference.png"));
    const bool savedClosestReference =
        closestReferenceTexture.save(closestReferencePath);
    WorkbenchMaterialData closestReferenceMaterial = filteredTextureMaterial;
    closestReferenceMaterial.textureImagePath = closestReferencePath;
    const QImage closestReferenceImage = renderColorMesh(
        textureSettings, {closestReferenceMaterial});
    const QColor closestReferencePixel = closestReferenceImage.pixelColor(
        closestProbeX, closestProbeY);
    passed &= check(savedInterpolationTexture &&
                        savedClosestReference &&
                        !linearTextureImage.isNull() &&
                        linearTextureImage == cubicTextureImage &&
                        linearTextureImage == smartTextureImage &&
                        closestTexturePixel == closestReferencePixel &&
                        closestTexturePixel != linearTexturePixel,
                    "QPainter Workbench Texture mode must filter Linear, Cubic, and Smart alike while Closest selects only the expected yellow texel");

    const std::array<float, 8> floatTexels{
        1.5f, -0.25f, 0.5f, 0.4f,
        -0.5f, 0.75f, 1.5f, 0.8f};
    const QByteArray floatTexelBytes(
        reinterpret_cast<const char *>(floatTexels.data()),
        static_cast<qsizetype>(sizeof(floatTexels)));
    WorkbenchMaterialData floatTextureMaterial;
    floatTextureMaterial.alpha = 0.7;
    floatTextureMaterial.textureImageInterpolation = QStringLiteral("Linear");
    floatTextureMaterial.textureImageSceneLinearFloatWidth = 2;
    floatTextureMaterial.textureImageSceneLinearFloatHeight = 1;
    floatTextureMaterial.textureImageSceneLinearFloatDataBase64 =
        QString::fromLatin1(floatTexelBytes.toBase64());
    const QImage floatTextureImage = renderColorMesh(
        textureSettings, {floatTextureMaterial});
    const QColor sampledFloatTexture = floatTextureImage.pixelColor(80, 80);
    passed &= check(sampledFloatTexture.blue() > sampledFloatTexture.red() &&
                        sampledFloatTexture.red() > sampledFloatTexture.green() &&
                        std::abs(sampledFloatTexture.alpha() - 179) <= 2,
                    "QPainter Texture mode must sample signed/HDR float RGB and preserve alpha independently");

    const std::array<float, 8> lowAlphaFloatTexels{
        0.7f, 0.2f, 0.1f, 0.05f,
        0.2f, 0.7f, 0.1f, 0.05f};
    const QByteArray lowAlphaFloatBytes(
        reinterpret_cast<const char *>(lowAlphaFloatTexels.data()),
        static_cast<qsizetype>(sizeof(lowAlphaFloatTexels)));
    WorkbenchMaterialData lowAlphaFloatMaterial = floatTextureMaterial;
    lowAlphaFloatMaterial.textureImageSceneLinearFloatDataBase64 =
        QString::fromLatin1(lowAlphaFloatBytes.toBase64());
    lowAlphaFloatMaterial.textureImageAlphaMode = QStringLiteral("STRAIGHT");
    const QImage lowAlphaFloatCutout = renderColorMesh(
        textureSettings, {lowAlphaFloatMaterial});
    lowAlphaFloatMaterial.textureImageAlphaMode = QStringLiteral("IGNORE");
    const QImage lowAlphaFloatIgnored = renderColorMesh(
        textureSettings, {lowAlphaFloatMaterial});
    lowAlphaFloatMaterial.textureImageAlphaMode =
        QStringLiteral("CHANNEL_PACKED");
    const QImage lowAlphaFloatChannelPacked = renderColorMesh(
        textureSettings, {lowAlphaFloatMaterial});
    passed &= check(
        lowAlphaFloatCutout.pixelColor(80, 80).alpha() == 0 &&
            lowAlphaFloatIgnored.pixelColor(80, 80).alpha() > 0 &&
            lowAlphaFloatChannelPacked.pixelColor(80, 80).alpha() > 0,
        "QPainter float-texture cutoff must be bypassed for Ignore and Channel Packed alpha modes");

    lowAlphaFloatMaterial.textureImageAlphaMode = QStringLiteral("STRAIGHT");
    const std::array<float, 8> exactThresholdTexels{
        0.7f, 0.2f, 0.1f, 0.1f,
        0.2f, 0.7f, 0.1f, 0.1f};
    const QByteArray exactThresholdBytes(
        reinterpret_cast<const char *>(exactThresholdTexels.data()),
        static_cast<qsizetype>(sizeof(exactThresholdTexels)));
    lowAlphaFloatMaterial.textureImageSceneLinearFloatDataBase64 =
        QString::fromLatin1(exactThresholdBytes.toBase64());
    const QImage exactThresholdColor = renderColorMesh(
        textureSettings, {lowAlphaFloatMaterial});
    const std::array<float, 8> belowThresholdTexels{
        0.7f, 0.2f, 0.1f, std::nextafter(0.1f, 0.0f),
        0.2f, 0.7f, 0.1f, std::nextafter(0.1f, 0.0f)};
    const QByteArray belowThresholdBytes(
        reinterpret_cast<const char *>(belowThresholdTexels.data()),
        static_cast<qsizetype>(sizeof(belowThresholdTexels)));
    lowAlphaFloatMaterial.textureImageSceneLinearFloatDataBase64 =
        QString::fromLatin1(belowThresholdBytes.toBase64());
    const QImage belowThresholdColor = renderColorMesh(
        textureSettings, {lowAlphaFloatMaterial});
    passed &= check(exactThresholdColor.pixelColor(80, 80).alpha() > 0 &&
                        belowThresholdColor.pixelColor(80, 80).alpha() == 0,
        "QPainter color alpha must retain a sample exactly at 0.1 and discard the next float below it");
    if (application.arguments().contains(
            QStringLiteral("--texture-alpha-cutoff-only"))) {
        return passed ? 0 : 1;
    }

    const QSize annotationSize(128, 128);
    ViewportTransform annotationTransform;
    annotationTransform.zoom() = 32.0;
    annotationTransform.setViewPreset(ViewportViewPreset::Top);
    CurveHitTester annotationHitTester;
    ViewportRenderer annotationRenderer(annotationTransform,
                                         annotationHitTester);
    Shape renderDimension;
    renderDimension.geometryType = GeometryType::LinearDimension;
    renderDimension.points = {QPointF(-0.75, 0.0), QPointF(0.75, 0.0),
                              QPointF(0.0, 0.35)};
    QImage annotationLayer(annotationSize,
                           QImage::Format_RGBA64_Premultiplied);
    annotationLayer.fill(Qt::transparent);
    {
        QPainter painter(&annotationLayer);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        annotationRenderer.drawShape(
            painter, renderDimension, annotationSize, false, false, false,
            QColor(220, 40, 25), QString(), 0.0);
    }
    QImage linearDimensionLayer(annotationSize,
                                QImage::Format_RGBA64_Premultiplied);
    linearDimensionLayer.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    linearDimensionLayer.fill(Qt::transparent);
    {
        QPainter painter(&linearDimensionLayer);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        annotationRenderer.drawShape(
            painter, renderDimension, annotationSize, false, false, false,
            QColor(220, 40, 25), QString(), 0.0);
    }
    QImage annotationDisplay(annotationSize, QImage::Format_RGBA64);
    annotationDisplay.fill(QColor(64, 64, 64, 255));
    QVector<float> annotationSceneLinear(annotationSize.width() *
                                         annotationSize.height() * 4);
    for (qsizetype index = 0; index < annotationSceneLinear.size();
         index += 4) {
        annotationSceneLinear[index] = 0.1f;
        annotationSceneLinear[index + 1] = 0.1f;
        annotationSceneLinear[index + 2] = 0.1f;
        annotationSceneLinear[index + 3] = 1.0f;
    }
    QPoint annotationSample(-1, -1);
    bool dimensionRendererLinearizedStroke = false;
    for (int y = 0; y < annotationSize.height() &&
                    !dimensionRendererLinearizedStroke; ++y) {
        const auto *row = reinterpret_cast<const QRgba64 *>(
            annotationLayer.constScanLine(y));
        const auto *linearRow = reinterpret_cast<const QRgba64 *>(
            linearDimensionLayer.constScanLine(y));
        for (int x = 0; x < annotationSize.width(); ++x) {
            if (row[x].alpha() > 0 && row[x].red() > row[x].green() * 2 &&
                row[x].red() > row[x].blue() * 2) {
                if (annotationSample.x() < 0) annotationSample = QPoint(x, y);
                const float alpha = row[x].alpha() / 65535.0f;
                if (alpha > 0.1f && linearRow[x].alpha() > 0) {
                    const float srgbRed = std::min(
                        1.0f, row[x].red() / 65535.0f / alpha);
                    const float expectedLinearRed = srgbRed <= 0.04045f
                        ? srgbRed / 12.92f
                        : std::pow((srgbRed + 0.055f) / 1.055f, 2.4f);
                    const float actualLinearRed =
                        linearRow[x].red() / 65535.0f /
                        std::max(1.0e-6f,
                                 linearRow[x].alpha() / 65535.0f);
                    dimensionRendererLinearizedStroke =
                        std::abs(actualLinearRed - expectedLinearRed) < 0.02f;
                }
                if (dimensionRendererLinearizedStroke) break;
            }
        }
    }
    const bool annotationCompositeSucceeded =
        compositeWorkbenchRenderAnnotations(
            &annotationDisplay, annotationLayer, &annotationSceneLinear,
            annotationSize);
    bool annotationSceneLinearMatches = false;
    if (annotationSample.x() >= 0) {
        const auto *row = reinterpret_cast<const QRgba64 *>(
            annotationLayer.constScanLine(annotationSample.y()));
        const QRgba64 pixel = row[annotationSample.x()];
        const float alpha = pixel.alpha() / 65535.0f;
        const float redSrgb = std::min(
            1.0f, pixel.red() / 65535.0f / std::max(alpha, 1.0e-6f));
        const float expectedRed =
            (redSrgb <= 0.04045f
                 ? redSrgb / 12.92f
                 : std::pow((redSrgb + 0.055f) / 1.055f, 2.4f)) * alpha +
            0.1f * (1.0f - alpha);
        const qsizetype pixelBase =
            (qsizetype(annotationSample.y()) * annotationSize.width() +
             annotationSample.x()) * 4;
        annotationSceneLinearMatches =
            std::abs(annotationSceneLinear[pixelBase] - expectedRed) < 1.0e-4f &&
            std::abs(annotationSceneLinear[pixelBase] - 0.1f) > 1.0e-3f;
    }
    passed &= check(annotationCompositeSucceeded &&
                        annotationSample.x() >= 0 &&
                        annotationSceneLinearMatches &&
                        dimensionRendererLinearizedStroke &&
                        annotationDisplay.pixelColor(annotationSample).red() >
                            annotationDisplay.pixelColor(annotationSample).green(),
                    "committed dimension annotations must use linearized stroke colors for scene-linear Combined while preserving display colors");

    const QSize annotationOverlapSize(9, 9);
    const auto makeLinearAnnotationLayer = [&]() {
        QImage layer(annotationOverlapSize,
                     QImage::Format_RGBA64_Premultiplied);
        layer.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
        layer.fill(Qt::transparent);
        return layer;
    };
    QImage firstLinearStroke = makeLinearAnnotationLayer();
    QImage secondLinearStroke = makeLinearAnnotationLayer();
    QImage overlappingLinearStrokes = makeLinearAnnotationLayer();
    const auto drawCrossingStroke = [](QImage *layer, const QColor &color,
                                       const QLineF &line) {
        QPainter painter(layer);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor paintColor = color;
        if (layer->colorSpace() == QColorSpace(QColorSpace::SRgbLinear)) {
            const auto srgbToLinear = [](qreal value) {
                return value <= 0.04045
                    ? value / 12.92
                    : std::pow((value + 0.055) / 1.055, 2.4);
            };
            paintColor = QColor::fromRgbF(
                srgbToLinear(color.redF()), srgbToLinear(color.greenF()),
                srgbToLinear(color.blueF()), color.alphaF());
        }
        painter.setPen(QPen(paintColor, 3.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(line);
    };
    const QLineF firstAnnotationLine(QPointF(0.5, 0.5), QPointF(8.5, 8.5));
    const QLineF secondAnnotationLine(QPointF(0.5, 8.5), QPointF(8.5, 0.5));
    const QColor firstAnnotationColor(200, 50, 30, 180);
    const QColor secondAnnotationColor(25, 90, 220, 210);
    drawCrossingStroke(&firstLinearStroke, firstAnnotationColor,
                       firstAnnotationLine);
    drawCrossingStroke(&secondLinearStroke, secondAnnotationColor,
                       secondAnnotationLine);
    drawCrossingStroke(&overlappingLinearStrokes, firstAnnotationColor,
                       firstAnnotationLine);
    drawCrossingStroke(&overlappingLinearStrokes, secondAnnotationColor,
                       secondAnnotationLine);
    // The display image uses the existing sRGB annotation layer. Render its
    // expected result independently from the linear HDR layer.
    QImage srgbOverlapLayer(annotationOverlapSize,
                            QImage::Format_RGBA64_Premultiplied);
    srgbOverlapLayer.fill(Qt::transparent);
    drawCrossingStroke(&srgbOverlapLayer, firstAnnotationColor,
                       firstAnnotationLine);
    drawCrossingStroke(&srgbOverlapLayer, secondAnnotationColor,
                       secondAnnotationLine);
    QImage expectedSrgbOverlapDisplay(annotationOverlapSize,
                                      QImage::Format_RGBA64);
    expectedSrgbOverlapDisplay.fill(QColor(40, 45, 50, 255));
    {
        QPainter painter(&expectedSrgbOverlapDisplay);
        painter.drawImage(0, 0, srgbOverlapLayer);
    }
    QVector<float> overlapSceneLinear(annotationOverlapSize.width() *
                                      annotationOverlapSize.height() * 4);
    for (qsizetype index = 0; index < overlapSceneLinear.size(); index += 4) {
        overlapSceneLinear[index] = 5.0f;
        overlapSceneLinear[index + 1] = 1.0f;
        overlapSceneLinear[index + 2] = 0.5f;
        overlapSceneLinear[index + 3] = 0.4f;
    }
    QImage overlapDisplay(annotationOverlapSize, QImage::Format_RGBA64);
    overlapDisplay.fill(QColor(40, 45, 50, 255));
    const bool overlapCompositeSucceeded =
        compositeWorkbenchRenderAnnotations(
            &overlapDisplay, srgbOverlapLayer,
            &overlapSceneLinear, annotationOverlapSize,
            &overlappingLinearStrokes);
    qreal maxOverlapLinearError = 0.0;
    int overlapPixelsChecked = 0;
    bool linearTargetColorCorrect = false;
    for (int y = 0; y < annotationOverlapSize.height(); ++y) {
        const auto *firstRow = reinterpret_cast<const QRgba64 *>(
            firstLinearStroke.constScanLine(y));
        const auto *secondRow = reinterpret_cast<const QRgba64 *>(
            secondLinearStroke.constScanLine(y));
        const auto *combinedRow = reinterpret_cast<const QRgba64 *>(
            overlappingLinearStrokes.constScanLine(y));
        const auto *srgbSecondRow = reinterpret_cast<const QRgba64 *>(
            secondLinearStroke.constScanLine(y));
        for (int x = 0; x < annotationOverlapSize.width(); ++x) {
            const QRgba64 first = firstRow[x];
            const QRgba64 second = secondRow[x];
            const float firstAlpha = first.alpha() / 65535.0f;
            const float secondAlpha = second.alpha() / 65535.0f;
            if (firstAlpha > 0.0f && secondAlpha > 0.0f) ++overlapPixelsChecked;
            if (!linearTargetColorCorrect && firstAlpha > 0.25f &&
                srgbSecondRow[x].alpha() == 0) {
                const float red = first.red() / 65535.0f / firstAlpha;
                const float sourceRed = firstAnnotationColor.redF();
                const float expected = sourceRed <= 0.04045f
                    ? sourceRed / 12.92f
                    : std::pow((sourceRed + 0.055f) / 1.055f, 2.4f);
                linearTargetColorCorrect = std::abs(red - expected) < 0.02f;
            }
            const float expectedRed = second.red() / 65535.0f +
                first.red() / 65535.0f * (1.0f - secondAlpha);
            const float expectedGreen = second.green() / 65535.0f +
                first.green() / 65535.0f * (1.0f - secondAlpha);
            const float expectedBlue = second.blue() / 65535.0f +
                first.blue() / 65535.0f * (1.0f - secondAlpha);
            const float expectedAlpha = secondAlpha +
                firstAlpha * (1.0f - secondAlpha);
            const QRgba64 actual = combinedRow[x];
            maxOverlapLinearError = std::max({
                maxOverlapLinearError,
                std::abs(qreal(actual.red() / 65535.0f - expectedRed)),
                std::abs(qreal(actual.green() / 65535.0f - expectedGreen)),
                std::abs(qreal(actual.blue() / 65535.0f - expectedBlue)),
                std::abs(qreal(actual.alpha() / 65535.0f - expectedAlpha))});
        }
    }
    const qsizetype overlapCenterBase =
        (qsizetype(4) * annotationOverlapSize.width() + 4) * 4;
    const qsizetype untouchedHdrBase =
        qsizetype(4) * annotationOverlapSize.width() * 4;
    const auto *overlapCenter = reinterpret_cast<const QRgba64 *>(
        overlappingLinearStrokes.constScanLine(4));
    const float centerAlpha = overlapCenter[4].alpha() / 65535.0f;
    const float expectedHdrRed = overlapCenter[4].red() / 65535.0f +
        5.0f * (1.0f - centerAlpha);
    const float expectedHdrAlpha = centerAlpha + 0.4f * (1.0f - centerAlpha);
    passed &= check(overlapCompositeSucceeded,
                    "overlap annotation compositor must accept matching linear RGBA64 layer");
    passed &= check(overlapPixelsChecked > 0 && maxOverlapLinearError < 2.0e-4,
                    "overlapping linear annotation strokes must source-over within QPainter antialias tolerance");
    passed &= check(linearTargetColorCorrect,
                    "QPainter must convert CAD annotation QColor values into the linear target color space");
    passed &= check(overlapDisplay == expectedSrgbOverlapDisplay,
                    "HDR annotation composition must preserve existing display-space sRGB output");
    passed &= check(std::abs(overlapSceneLinear[overlapCenterBase] -
                             expectedHdrRed) < 1.0e-4f &&
                        std::abs(overlapSceneLinear[overlapCenterBase + 3] -
                                 expectedHdrAlpha) < 1.0e-4f &&
                        overlapSceneLinear[untouchedHdrBase] == 5.0f,
                    "HDR annotation composite must preserve scene-linear range and alpha");

    WorkbenchRenderResult annotationHdrResult;
    annotationHdrResult.combined = annotationDisplay;
    annotationHdrResult.sceneLinearRgba = annotationSceneLinear;
    annotationHdrResult.sceneLinearSize = annotationSize;
    annotationHdrResult.blenderImageFormat = QStringLiteral("HDR");
    QTemporaryDir annotationOutputDirectory;
    const QString annotationHdrPath = annotationOutputDirectory.filePath(
        QStringLiteral("dimension-combined.hdr"));
    QString annotationHdrError;
    const bool annotationHdrSaved =
        annotationHdrResult.saveCombinedImage(annotationHdrPath,
                                              &annotationHdrError);
    passed &= check(annotationOutputDirectory.isValid() &&
                        annotationHdrSaved &&
                        QFileInfo(annotationHdrPath).size() > 32,
                    "HDR Combined writer must accept the scene-linear buffer containing committed dimension annotations");

    const auto makePassResult = [](bool combinedAvailable,
                                   bool depthAvailable) {
        WorkbenchRenderResult result;
        result.outputSize = QSize(2, 2);
        result.combined = QImage(result.outputSize, QImage::Format_RGBA64);
        result.combined.fill(Qt::black);
        result.depth = QVector<float>(4, 3.0f);
        result.combinedPassAvailable = combinedAvailable;
        result.depthPassAvailable = depthAvailable;
        result.discardUnavailablePassPayloads();
        return result;
    };
    WorkbenchRenderResult combinedOnly = makePassResult(true, false);
    WorkbenchRenderResult depthOnly = makePassResult(false, true);
    WorkbenchRenderResult noPasses = makePassResult(false, false);
    QTemporaryDir passResultDirectory;
    const QString combinedOnlyDepthPath = passResultDirectory.filePath(
        QStringLiteral("combined-only.pfm"));
    const QString depthOnlyPath = passResultDirectory.filePath(
        QStringLiteral("depth-only.pfm"));
    QString passSaveError;
    const bool combinedOnlyRejectsDepth =
        !combinedOnly.saveDepthPfm(combinedOnlyDepthPath, &passSaveError);
    passSaveError.clear();
    const bool depthOnlySavesDepth =
        depthOnly.saveDepthPfm(depthOnlyPath, &passSaveError);
    QFile depthOnlyFile(depthOnlyPath);
    const bool depthOnlyHeaderValid = depthOnlyFile.open(QIODevice::ReadOnly) &&
        depthOnlyFile.readLine() == QByteArray("Pf\n") &&
        depthOnlyFile.readLine() == QByteArray("2 2\n");
    passSaveError.clear();
    const bool depthOnlyRejectsCombined = !depthOnly.saveCombinedImage(
        passResultDirectory.filePath(QStringLiteral("disabled.png")),
        &passSaveError);
    passed &= check(combinedOnly.isValid() && !combinedOnly.combined.isNull() &&
                        combinedOnly.depth.isEmpty() &&
                        combinedOnlyRejectsDepth && depthOnly.isValid() &&
                        depthOnly.combined.isNull() && depthOnly.hasDepthPass() &&
                        depthOnlySavesDepth && depthOnlyHeaderValid &&
                        depthOnlyRejectsCombined && !noPasses.isValid() &&
                        noPasses.combined.isNull() && noPasses.depth.isEmpty(),
                    "Workbench result assembly must expose and save only registered Combined/Z passes, and reject an empty result");
    if (WorkbenchRenderResult::openExrWriterAvailable()) {
        annotationHdrResult.blenderImageFormat = QStringLiteral("OPEN_EXR");
        const QString annotationExrPath = annotationOutputDirectory.filePath(
            QStringLiteral("dimension-combined.exr"));
        QString annotationExrError;
        passed &= check(annotationHdrResult.saveCombinedImage(
                            annotationExrPath, &annotationExrError) &&
                            QFileInfo(annotationExrPath).size() > 32,
                        "EXR Combined writer must accept the scene-linear buffer containing committed dimension annotations");
    }

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
            QImage gpuWorkbenchOutlineImage;
            QImage gpuWorkbenchOutlineBaselineImage;
            QImage gpuWorkbenchShadowImage;
            QImage gpuBottomImage;
            QImage gpuShadowImage;
            int gpuCavityChangedPixels = 0;
            int gpuShadowDarkenedPixels = 0;
            QImage gpuMatcapImage;
            bool gpuDrawSucceeded = false;
            bool gpuSceneLinearOutlineColorPreserved = false;
            bool gpuWorkbenchOutlineCaptureSucceeded = false;
            bool gpuWorkbenchShadowCaptureSucceeded = false;
            bool gpuUnselectedCaptureNonblank = true;
            bool gpuMatcapDrawSucceeded = false;
            bool gpuBottomDrawSucceeded = false;
            bool gpuShadowDrawSucceeded = false;
            bool gpuCavityDrawSucceeded = false;
            bool gpuNamedPrepassReportValid = false;
            bool gpuUnselectedBaselineDrawSucceeded = false;
            bool gpuSceneStrokeSuppressesPostOutline = false;
            bool gpuWireLinearBlendMatches = false;
            bool gpuVolumeDrawSucceeded = false;
            bool gpuVolumePassReported = false;
            int gpuVolumeChangedPixels = 0;
            bool gpuLowDensityVolumePreserved = false;
            bool gpuSignedVolumeScatteringPreserved = false;
            bool gpuRotatedVolumeSliceOpacityMatches = false;
            const QSize glSize(640, 480);
            ViewportTransform gpuTransform;
            gpuTransform.zoom() = 17.28;
            gpuTransform.setViewDirection({4.0, -6.0, 4.0});
            const QMatrix4x4 baselineViewProjection =
                viewportViewProjection(gpuTransform, glSize);
            const QVector4D projectedRightCorner =
                baselineViewProjection * QVector4D(1.0f, 1.0f, 1.0f, 1.0f);
            const QVector4D projectedLeftCorner =
                baselineViewProjection * QVector4D(-1.0f, -1.0f, -1.0f, 1.0f);
            const qreal rightCornerX =
                (projectedRightCorner.x() / projectedRightCorner.w() + 1.0) *
                glSize.width() * 0.5;
            const qreal leftCornerX =
                (projectedLeftCorner.x() / projectedLeftCorner.w() + 1.0) *
                glSize.width() * 0.5;
            passed &= check(
                std::abs(rightCornerX - 497.50404) < 0.001 &&
                    std::abs(leftCornerX - 142.49594) < 0.001,
                "orthographic cube corners must retain Blender's 640x480 projection with a distant depth eye");
            {
                QOpenGLFramebufferObjectFormat framebufferFormat;
                framebufferFormat.setAttachment(
                    QOpenGLFramebufferObject::CombinedDepthStencil);
                QOpenGLFramebufferObject framebuffer(glSize,
                                                     framebufferFormat);
                passed &= check(framebuffer.isValid(),
                                "NURBS solid display test must allocate a depth framebuffer");
                if (framebuffer.isValid()) {
                    BlenderGridRenderer themeBackgroundRenderer;
                    const auto renderThemeBackground = [&] (
                        ViewportThemeBackgroundStyle style,
                        qreal devicePixelRatio) {
                        ViewportThemeBackground background = pixelThemeBackground;
                        background.style = style;
                        framebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                            glSize.height());
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT |
                                          GL_STENCIL_BUFFER_BIT);
                        const bool rendered =
                            themeBackgroundRenderer
                                .renderBackgroundToCurrentFramebuffer(
                                    QSize(qRound(glSize.width() / devicePixelRatio),
                                          qRound(glSize.height() / devicePixelRatio)),
                                    devicePixelRatio, background, false);
                        functions.glFinish();
                        struct Pixels {
                            QImage image;
                            std::array<uchar, 4> bottomLeft{};
                            std::array<uchar, 4> topLeft{};
                            std::array<uchar, 4> center{};
                            std::array<uchar, 4> bottomRight{};
                            bool valid = false;
                        } pixels;
                        pixels.valid = rendered;
                        if (rendered) pixels.image = framebuffer.toImage();
                        const auto readPixel = [&](int x, int y,
                                                   std::array<uchar, 4> *out) {
                            functions.glReadPixels(x, y, 1, 1, GL_RGBA,
                                                   GL_UNSIGNED_BYTE,
                                                   out->data());
                        };
                        readPixel(0, 0, &pixels.bottomLeft);
                        readPixel(0, glSize.height() - 1, &pixels.topLeft);
                        readPixel(glSize.width() / 2, glSize.height() / 2,
                                  &pixels.center);
                        readPixel(glSize.width() - 1, 0,
                                  &pixels.bottomRight);
                        return pixels;
                    };
                    const auto singleGpuBackground = renderThemeBackground(
                        ViewportThemeBackgroundStyle::SingleColor, 1.0);
                    const auto linearGpuBackground = renderThemeBackground(
                        ViewportThemeBackgroundStyle::Linear, 1.0);
                    const auto radialGpuBackground = renderThemeBackground(
                        ViewportThemeBackgroundStyle::Radial, 1.0);
                    const auto nearColor = [](const std::array<uchar, 4> &pixel,
                                              const QColor &color,
                                              int tolerance) {
                        return std::abs(int(pixel[0]) - color.red()) <= tolerance &&
                               std::abs(int(pixel[1]) - color.green()) <= tolerance &&
                               std::abs(int(pixel[2]) - color.blue()) <= tolerance &&
                               pixel[3] == 255;
                    };
                    passed &= check(singleGpuBackground.valid &&
                                        linearGpuBackground.valid &&
                                        radialGpuBackground.valid &&
                                        nearColor(singleGpuBackground.center,
                                                  pixelThemeBackground.lowColor,
                                                  2) &&
                                        nearColor(linearGpuBackground.topLeft,
                                                  pixelThemeBackground.highColor,
                                                  2) &&
                                        nearColor(linearGpuBackground.bottomLeft,
                                                  pixelThemeBackground.lowColor,
                                                  2) &&
                                        nearColor(radialGpuBackground.center,
                                                  pixelThemeBackground.highColor,
                                                  2) &&
                                        nearColor(radialGpuBackground.bottomRight,
                                                  pixelThemeBackground.lowColor,
                                                  2),
                                    "GPU Theme background shader must render known Single, top-to-bottom Linear, and center-to-edge Radial pixels");

                    const auto paintThemeBackgroundAtSize = [&] (
                        ViewportThemeBackgroundStyle style,
                        qreal devicePixelRatio) {
                        QImage image(glSize, QImage::Format_RGB32);
                        image.setDevicePixelRatio(devicePixelRatio);
                        image.fill(Qt::black);
                        ViewportThemeBackground background = pixelThemeBackground;
                        background.style = style;
                        QPainter painter(&image);
                        const QRect logicalBounds(
                            0, 0,
                            qRound(glSize.width() / devicePixelRatio),
                            qRound(glSize.height() / devicePixelRatio));
                        paintViewportThemeBackground(painter, logicalBounds,
                                                     background);
                        painter.end();
                        return image;
                    };
                    const auto compareThemeImages = [&] (
                        const QImage &cpu, const QImage &gpu) {
                        struct Difference { int maximum = 0; double mean = 0.0; } result;
                        if (cpu.size() != gpu.size() || cpu.isNull() || gpu.isNull()) {
                            result.maximum = 256;
                            result.mean = 256.0;
                            return result;
                        }
                        qint64 total = 0;
                        for (int y = 0; y < cpu.height(); ++y) {
                            for (int x = 0; x < cpu.width(); ++x) {
                                const QRgb a = cpu.pixel(x, y);
                                const QRgb b = gpu.pixel(x, y);
                                for (int channel = 0; channel < 3; ++channel) {
                                    const int av = channel == 0 ? qRed(a) : channel == 1 ? qGreen(a) : qBlue(a);
                                    const int bv = channel == 0 ? qRed(b) : channel == 1 ? qGreen(b) : qBlue(b);
                                    const int difference = std::abs(av - bv);
                                    result.maximum = std::max(result.maximum, difference);
                                    total += difference;
                                }
                            }
                        }
                        result.mean = static_cast<double>(total) /
                            (cpu.width() * cpu.height() * 3);
                        return result;
                    };
                    const auto singleThemeDifference = compareThemeImages(
                        paintThemeBackgroundAtSize(ViewportThemeBackgroundStyle::SingleColor, 1.0),
                        singleGpuBackground.image);
                    const auto linearThemeDifference = compareThemeImages(
                        paintThemeBackgroundAtSize(ViewportThemeBackgroundStyle::Linear, 1.0),
                        linearGpuBackground.image);
                    const auto radialThemeDifference = compareThemeImages(
                        paintThemeBackgroundAtSize(ViewportThemeBackgroundStyle::Radial, 1.0),
                        radialGpuBackground.image);
                    const auto radialGpuBackgroundAt2x = renderThemeBackground(
                        ViewportThemeBackgroundStyle::Radial, 2.0);
                    const auto radialThemeDifferenceAt2x = compareThemeImages(
                        paintThemeBackgroundAtSize(ViewportThemeBackgroundStyle::Radial, 2.0),
                        radialGpuBackgroundAt2x.image);
                    // The direct-to-display shader adds a sub-LSB Bayer dither;
                    // allow two code values for that plus raster/interpolation rounding.
                    constexpr int themeImageTolerance = 2;
                    passed &= check(singleThemeDifference.maximum <= themeImageTolerance &&
                                        linearThemeDifference.maximum <= themeImageTolerance &&
                                        radialThemeDifference.maximum <= themeImageTolerance &&
                                        radialThemeDifferenceAt2x.maximum <= themeImageTolerance,
                                    "QPainter and GPU Theme backgrounds must match across full Single, Linear, and Radial images within two display code values");

                    const auto renderPresentedBackground = [&] (
                        bool worldBackground, bool drawBackground,
                        bool premultipliedAlpha, const char *historyKey) {
                        if (!framebuffer.bind()) return QColor();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        ViewportAaRenderer presenter;
                        presenter.setMode(static_cast<int>(
                            ViewportAntiAliasingMode::Off));
                        ViewportShadingSettings displaySettings;
                        presenter.setDisplayTransform(displaySettings, true,
                                                      premultipliedAlpha);
                        if (!presenter.beginSceneSample(
                                glSize, 1.0, gpuTransform,
                                QByteArray(historyKey), false, true)) {
                            return QColor();
                        }
                        bool backgroundRendered = true;
                        if (drawBackground) {
                            backgroundRendered = worldBackground
                                ? themeBackgroundRenderer
                                      .renderBackgroundToCurrentFramebuffer(
                                          glSize, 1.0,
                                          QVector3D(0.17f, 0.09f, 0.035f))
                                : themeBackgroundRenderer
                                      .renderBackgroundToCurrentFramebuffer(
                                          glSize, 1.0,
                                          QColor(75, 115, 165), true);
                        }
                        if (!backgroundRendered) return QColor();
                        presenter.finishSceneSample();
                        functions.glFinish();
                        return framebuffer.toImage().pixelColor(
                            glSize.width() / 2, glSize.height() / 2);
                    };
                    const QColor resolvedWorldBackground =
                        renderPresentedBackground(
                            true, true, false, "world-background-resolve");
                    const QColor resolvedViewportBackground =
                        renderPresentedBackground(
                            false, true, false,
                            "viewport-background-resolve");
                    const QColor resolvedMissingWorldBackground =
                        renderPresentedBackground(
                            true, false, true,
                            "missing-world-background-resolve");
                    const QColor resolvedTransparentFilm =
                        renderPresentedBackground(
                            false, false, true,
                            "transparent-film-background-resolve");
                    framebuffer.bind();
                    functions.glViewport(0, 0, glSize.width(),
                                         glSize.height());
                    ViewportAaRenderer premultipliedPresenter;
                    premultipliedPresenter.setMode(static_cast<int>(
                        ViewportAntiAliasingMode::Off));
                    ViewportShadingSettings agxResolveSettings;
                    premultipliedPresenter.setDisplayTransform(
                        agxResolveSettings, true, true);
                    std::array<uchar, 4> resolvedPremultipliedAgx{};
                    bool premultipliedAgxCaptureValid = false;
                    if (premultipliedPresenter.beginSceneSample(
                            glSize, 1.0, gpuTransform,
                            QByteArrayLiteral("premultiplied-agx-resolve"),
                            false, true)) {
                        const GLfloat premultipliedColor[4] = {
                            0.09f, 0.045f, 0.0225f, 0.5f};
                        functions.glClearBufferfv(GL_COLOR, 0,
                                                  premultipliedColor);
                        premultipliedPresenter.finishSceneSample();
                        functions.glFinish();
                        premultipliedAgxCaptureValid = framebuffer.bind();
                        functions.glReadPixels(
                            glSize.width() / 2, glSize.height() / 2, 1, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            resolvedPremultipliedAgx.data());
                    }
                    const QVector3D expectedWorldDisplay =
                        workbenchSceneLinearToAgxSrgb(
                            QVector3D(0.17f, 0.09f, 0.035f));
                    const QVector3D viewportEncoded(75.0f / 255.0f,
                                                    115.0f / 255.0f,
                                                    165.0f / 255.0f);
                    const QVector3D expectedViewportDisplay =
                        workbenchSceneLinearToAgxSrgb(
                            workbenchSrgbToSceneLinear(viewportEncoded));
                    const auto matchesDisplayColor = [](
                        const QColor &actual, const QVector3D &expected) {
                        return actual.isValid() &&
                               std::abs(actual.red() -
                                        qRound(expected.x() * 255.0f)) <= 2 &&
                               std::abs(actual.green() -
                                        qRound(expected.y() * 255.0f)) <= 2 &&
                               std::abs(actual.blue() -
                                        qRound(expected.z() * 255.0f)) <= 2 &&
                               actual.alpha() == 255;
                    };
                    const auto isTransparentBlack = [](const QColor &pixel) {
                        return pixel.isValid() && pixel.red() <= 1 &&
                               pixel.green() <= 1 && pixel.blue() <= 1 &&
                               pixel.alpha() == 0;
                    };
                    const QVector3D expectedPremultipliedAgx =
                        workbenchSceneLinearToAgxSrgb(
                            QVector3D(0.18f, 0.09f, 0.045f)) * 0.5f;
                    passed &= check(
                        matchesDisplayColor(resolvedWorldBackground,
                                            expectedWorldDisplay) &&
                            matchesDisplayColor(resolvedViewportBackground,
                                                expectedViewportDisplay) &&
                            isTransparentBlack(resolvedMissingWorldBackground) &&
                            isTransparentBlack(resolvedTransparentFilm),
                        "AA-off Workbench presentation must resolve scene-linear World and sRGB viewport backgrounds, while missing-World and transparent-film outputs retain transparent alpha");
                    passed &= check(
                            premultipliedAgxCaptureValid &&
                            std::abs(int(resolvedPremultipliedAgx[0]) -
                                     qRound(expectedPremultipliedAgx.x() *
                                            255.0f)) <= 2 &&
                            std::abs(int(resolvedPremultipliedAgx[1]) -
                                     qRound(expectedPremultipliedAgx.y() *
                                            255.0f)) <= 2 &&
                            std::abs(int(resolvedPremultipliedAgx[2]) -
                                     qRound(expectedPremultipliedAgx.z() *
                                            255.0f)) <= 2 &&
                            resolvedPremultipliedAgx[3] == 127,
                        "AgX presentation must unpremultiply scene-linear transparent color before the transform and restore its alpha afterward");

                    struct DitherCapture {
                        QImage image;
                        std::array<uchar, 4> bottomLeftPixel{};
                    };
                    const auto renderDitherContractImage = [&](
                        qreal intensity, const char *historyKey) {
                        framebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                            glSize.height());
                        ViewportAaRenderer presenter;
                        presenter.setMode(static_cast<int>(
                            ViewportAntiAliasingMode::Off));
                        ViewportShadingSettings displaySettings;
                        displaySettings.colorTransform = 1;
                        displaySettings.colorDitherIntensity = intensity;
                        presenter.setDisplayTransform(displaySettings, true,
                                                      true);
                        if (!presenter.beginSceneSample(
                                glSize, 1.0, gpuTransform,
                                QByteArray(historyKey), false)) {
                            return DitherCapture{};
                        }
                        const GLfloat premultipliedColor[4] = {
                            0.09f, 0.09f, 0.09f, 0.5f};
                        functions.glClearBufferfv(GL_COLOR, 0,
                                                  premultipliedColor);
                        presenter.finishSceneSample();
                        functions.glFinish();
                        DitherCapture capture;
                        capture.image = framebuffer.toImage();
                        framebuffer.bind();
                        functions.glReadPixels(0, 0, 1, 1, GL_RGBA,
                                               GL_UNSIGNED_BYTE,
                                               capture.bottomLeftPixel.data());
                        return capture;
                    };
                    const DitherCapture noDitherCapture =
                        renderDitherContractImage(0.0, "dither-zero");
                    const DitherCapture ditheredCapture =
                        renderDitherContractImage(1.0, "dither-one");
                    const DitherCapture repeatedDitheredCapture =
                        renderDitherContractImage(1.0, "dither-one-repeat");
                    const QImage &noDitherImage = noDitherCapture.image;
                    const QImage &ditheredImage = ditheredCapture.image;
                    int ditherChangedPixels = 0;
                    bool ditherPreservedAlpha =
                        !noDitherImage.isNull() &&
                        noDitherImage.size() == ditheredImage.size() &&
                        ditheredImage == repeatedDitheredCapture.image;
                    if (ditherPreservedAlpha) {
                        for (int y = 0; y < noDitherImage.height(); ++y) {
                            for (int x = 0; x < noDitherImage.width(); ++x) {
                                const QColor base =
                                    noDitherImage.pixelColor(x, y);
                                const QColor dithered =
                                    ditheredImage.pixelColor(x, y);
                                ditherChangedPixels +=
                                    base.red() != dithered.red() ||
                                    base.green() != dithered.green() ||
                                    base.blue() != dithered.blue();
                                ditherPreservedAlpha &=
                                    base.alpha() == dithered.alpha();
                            }
                        }
                    }
                    const auto expectedDitherByte = [](float linear,
                                                       float noise) {
                        const float display = linear <= 0.0031308f
                            ? linear * 12.92f
                            : 1.055f * std::pow(linear, 1.0f / 2.4f) -
                                  0.055f;
                        const float premultiplied =
                            (display + noise * 0.0033f) * 0.5f;
                        return int(std::lround(
                            std::clamp(premultiplied, 0.0f, 1.0f) * 255.0f));
                    };
                    const auto blenderDitherNoise = [](
                        std::uint32_t x, std::uint32_t y) {
                        const std::uint32_t qx = 1103515245u *
                            ((x >> 1u) ^ y);
                        const std::uint32_t qy = 1103515245u *
                            ((y >> 1u) ^ x);
                        const std::uint32_t n = 1103515245u *
                            (qx ^ (qy >> 3u));
                        float value = static_cast<float>(n) *
                            (1.0f / static_cast<float>(
                                std::numeric_limits<std::uint32_t>::max()));
                        value = value * 2.0f - 1.0f;
                        return std::copysign(
                            1.0f, value) *
                            (1.0f - std::sqrt(1.0f - std::fabs(value)));
                    };
                    // Blender's hash at bottom-left pixel (0,0) is zero;
                    // triangle remapping therefore returns exactly -1.
                    const float bottomLeftNoise = blenderDitherNoise(0, 0);
                    const int expectedDitheredRed = expectedDitherByte(
                        0.18f, bottomLeftNoise);
                    const int expectedUnditheredRed = expectedDitherByte(
                        0.18f, 0.0f);
                    const bool expectedDitherVectorMatches =
                        bottomLeftNoise == -1.0f &&
                        expectedDitheredRed == 58 &&
                        expectedUnditheredRed == 59 &&
                        ditheredCapture.bottomLeftPixel[0] ==
                            expectedDitheredRed &&
                        ditheredCapture.bottomLeftPixel[1] ==
                            expectedDitheredRed &&
                        ditheredCapture.bottomLeftPixel[2] ==
                            expectedDitheredRed &&
                        noDitherCapture.bottomLeftPixel[0] ==
                            expectedUnditheredRed &&
                        ditheredCapture.bottomLeftPixel[3] ==
                            noDitherCapture.bottomLeftPixel[3];
                    passed &= check(
                        ditherPreservedAlpha && ditherChangedPixels > 0 &&
                            expectedDitherVectorMatches,
                        "final display dither must match Blender's bottom-left (0,0) triangle-noise pixel, change RGB only, repeat exactly, and be disabled at zero intensity");

                    ViewportRenderObject surfaceObject;
                    surfaceObject.shape = cubeShape;
                    surfaceObject.objectId = ObjectId::fromValue(1);
                    surfaceObject.selected = true;
                    surfaceObject.geometryRevision = 1;
                    surfaceObject.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(surfaceObject.shape));
                    ViewportRenderObject polygonCubeObject = surfaceObject;
                    polygonCubeObject.selected = false;
                    polygonCubeObject.shape = Shape{};
                    polygonCubeObject.shape.geometryType =
                        GeometryType::PolygonMesh;
                    polygonCubeObject.shape.polygonMesh.vertices = {
                        {-1.0, -1.0, -1.0}, {-1.0, -1.0, 1.0},
                        {-1.0, 1.0, -1.0},  {-1.0, 1.0, 1.0},
                        {1.0, -1.0, -1.0},  {1.0, -1.0, 1.0},
                        {1.0, 1.0, -1.0},   {1.0, 1.0, 1.0}};
                    polygonCubeObject.shape.polygonMesh.faces = {
                        {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                        {4, 6, 7, 5}, {1, 3, 2, 0}, {2, 3, 7, 6}};
                    polygonCubeObject.shape.polygonMesh.faceMaterialIndices =
                        {0, 0, 0, 0, 0, 0};
                    polygonCubeObject.meshMaterials = {WorkbenchMaterialData{}};
                    polygonCubeObject.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(
                                polygonCubeObject.shape));
                    ViewportShadingSettings gpuSolidShading;
                    gpuSolidShading.mode = ViewportShadingMode::Solid;
                    ViewportSurfaceRenderer gpuSurfaceRenderer;

                    // NurbsSolid owns one object-level Workbench material.
                    // Exercise its derived-face UV stream through visible
                    // color, outline-ID alpha, and the no-stencil map path.
                    QImage transparentTexture(2, 2,
                                              QImage::Format_RGBA8888);
                    transparentTexture.fill(QColor(255, 20, 10, 0));
                    QByteArray packedTextureBytes;
                    QBuffer packedTextureBuffer(&packedTextureBytes);
                    const bool packedTextureReady =
                        packedTextureBuffer.open(QIODevice::WriteOnly) &&
                        transparentTexture.save(&packedTextureBuffer,
                                                "PNG");
                    ViewportRenderObject alphaReceiver = surfaceObject;
                    alphaReceiver.objectId = ObjectId::fromValue(801);
                    alphaReceiver.selected = false;
                    alphaReceiver.hasWorkbenchMaterial = false;
                    alphaReceiver.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(alphaReceiver.shape));

                    NurbsSurface3D raisedCasterBase = cubeBaseSurface;
                    for (Point3D &point : raisedCasterBase.controlPoints) {
                        point.x *= 0.4;
                        point.y *= 0.4;
                        point.z = 1.2;
                    }
                    Shape raisedCasterShape;
                    raisedCasterShape.geometryType = GeometryType::NurbsSolid;
                    const bool raisedCasterValid = makeNurbsExtrusionSolid(
                        raisedCasterBase, {0.0, 0.0, 0.8},
                        &raisedCasterShape.nurbsSolid);
                    ViewportRenderObject alphaCaster;
                    alphaCaster.shape = raisedCasterShape;
                    alphaCaster.objectId = ObjectId::fromValue(802);
                    alphaCaster.geometryRevision = 1;
                    alphaCaster.hasWorkbenchMaterial = true;
                    alphaCaster.workbenchMaterial.textureImagePackedDataBase64 =
                        QString::fromLatin1(
                            packedTextureBytes.toBase64());
                    alphaCaster.workbenchMaterial.textureImageSrgb = false;
                    alphaCaster.workbenchMaterial.textureImageAlphaMode =
                        QStringLiteral("STRAIGHT");
                    alphaCaster.workbenchMaterial.textureImageInterpolation =
                        QStringLiteral("Closest");
                    alphaCaster.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(alphaCaster.shape));
                    ViewportRenderObject floatAlphaCaster = alphaCaster;
                    floatAlphaCaster.workbenchMaterial.textureImagePackedDataBase64.clear();
                    const std::array<float, 16> transparentFloatTexels{
                        0.8f, 0.2f, 0.1f, 0.0f,
                        0.3f, 0.7f, 0.2f, 0.0f,
                        0.1f, 0.4f, 0.9f, 0.0f,
                        0.6f, 0.3f, 0.8f, 0.0f};
                    const QByteArray transparentFloatBytes(
                        reinterpret_cast<const char *>(transparentFloatTexels.data()),
                        static_cast<qsizetype>(sizeof(transparentFloatTexels)));
                    floatAlphaCaster.workbenchMaterial
                        .textureImageSceneLinearFloatDataBase64 =
                        QString::fromLatin1(transparentFloatBytes.toBase64());
                    floatAlphaCaster.workbenchMaterial
                        .textureImageSceneLinearFloatWidth = 2;
                    floatAlphaCaster.workbenchMaterial
                        .textureImageSceneLinearFloatHeight = 2;
                    ViewportRenderObject ignoredAlphaCaster = alphaCaster;
                    ignoredAlphaCaster.workbenchMaterial.textureImageAlphaMode =
                        QStringLiteral("NONE");

                    const auto renderCpuSolidAlphaScene = [&] (
                        const QVector<ViewportRenderObject> &objects) {
                        QImage image(surfaceViewportSize,
                                     QImage::Format_ARGB32_Premultiplied);
                        image.fill(Qt::transparent);
                        ViewportShadingSettings settings;
                        settings.mode = ViewportShadingMode::Solid;
                        settings.lightingMode = ViewportLightingMode::Flat;
                        settings.colorMode = ViewportColorMode::Texture;
                        settings.outline = true;
                        settings.specularLighting = false;
                        const bool composed =
                            compositeRasterWorkbenchSurfaces(
                                &image, objects, surfaceTransform,
                                surfaceViewportSize, 1.0, false, true,
                                settings, std::nullopt,
                                [&](QPainter &painter,
                                    const ViewportRenderObject &object,
                                    RasterWorkbenchFragmentSink *fragmentSink) {
                                    surfaceRenderer.setShadingSettings(settings);
                                    surfaceRenderer.drawShape(
                                        painter, object.shape,
                                        surfaceViewportSize, false,
                                        object.selected, true,
                                        object.layerColor,
                                        object.layerLineType,
                                        object.layerLineWeightMm,
                                        object.objectId,
                                        object.geometryRevision,
                                        object.preparedDepthGeometry.data(),
                                        object.preparedGeometryOffset,
                                        object.displayColorSceneLinear,
                                        object.workbenchMaterial,
                                        object.displayName,
                                        object.libraryPath,
                                        object.meshObjectTransform,
                                        object.meshMaterials,
                                        fragmentSink);
                                });
                        if (!composed) return QImage{};
                        applyRasterWorkbenchEffects(
                            &image, objects, surfaceTransform,
                            surfaceViewportSize, 1.0, settings);
                        return image;
                    };
                    const QImage cpuSolidAlphaReceiverOnly =
                        renderCpuSolidAlphaScene({alphaReceiver});
                    const QImage cpuSolidAlphaCutout =
                        renderCpuSolidAlphaScene({alphaReceiver, alphaCaster});
                    const QImage cpuSolidFloatAlphaCutout =
                        renderCpuSolidAlphaScene({alphaReceiver, floatAlphaCaster});
                    const QImage cpuSolidAlphaIgnored =
                        renderCpuSolidAlphaScene(
                            {alphaReceiver, ignoredAlphaCaster});
                    passed &= check(!cpuSolidAlphaReceiverOnly.isNull(),
                                    "QPainter NurbsSolid alpha baseline must render");
                    passed &= check(
                        cpuSolidAlphaCutout == cpuSolidAlphaReceiverOnly,
                        "QPainter NurbsSolid alpha cutout must match receiver-only color and outline");
                    passed &= check(
                        cpuSolidFloatAlphaCutout == cpuSolidAlphaReceiverOnly,
                        "QPainter NurbsSolid float-texture alpha must mask color and effect passes");
                    passed &= check(
                        cpuSolidAlphaIgnored != cpuSolidAlphaReceiverOnly,
                        "QPainter NurbsSolid NONE alpha mode must bypass the cutout");

                    QOpenGLFramebufferObjectFormat depthOnlyFormat;
                    depthOnlyFormat.setAttachment(
                        QOpenGLFramebufferObject::Depth);
                    QOpenGLFramebufferObject depthOnlyFramebuffer(
                        glSize, depthOnlyFormat);
                    GLint depthOnlyStencilBits = -1;
                    if (depthOnlyFramebuffer.isValid()) {
                        depthOnlyFramebuffer.bind();
                        functions.glGetIntegerv(GL_STENCIL_BITS,
                                                &depthOnlyStencilBits);
                    }
                    const auto renderSolidAlphaScene = [&] (
                        const QVector<ViewportRenderObject> &objects,
                        bool shadows, bool outline) {
                        if (!depthOnlyFramebuffer.isValid()) return QImage{};
                        depthOnlyFramebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        functions.glClearColor(0.02f, 0.02f, 0.02f, 0.0f);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        ViewportShadingSettings settings;
                        settings.mode = ViewportShadingMode::Solid;
                        settings.lightingMode = ViewportLightingMode::Flat;
                        settings.colorMode = ViewportColorMode::Texture;
                        settings.outline = outline;
                        settings.specularLighting = false;
                        settings.shadows = shadows;
                        settings.shadowIntensity = 0.8;
                        settings.colorDitherIntensity = 0.0;
                        if (!gpuSurfaceRenderer.draw(objects, gpuTransform,
                                                     glSize, 1.0, settings)) {
                            return QImage{};
                        }
                        functions.glFinish();
                        return depthOnlyFramebuffer.toImage();
                    };
                    const QImage solidAlphaReceiverOnly =
                        renderSolidAlphaScene({alphaReceiver}, false, false);
                    const QImage solidAlphaCutoutColor =
                        renderSolidAlphaScene({alphaReceiver, alphaCaster},
                                              false, false);
                    const QImage solidAlphaCutoutId =
                        renderSolidAlphaScene({alphaReceiver, alphaCaster},
                                              false, true);
                    const QImage solidAlphaReceiverOutline =
                        renderSolidAlphaScene({alphaReceiver}, false, true);
                    const QImage solidAlphaCutoutShadow =
                        renderSolidAlphaScene({alphaReceiver, alphaCaster},
                                              true, false);
                    const QImage solidAlphaReceiverShadow =
                        renderSolidAlphaScene({alphaReceiver}, true, false);
                    const QImage solidAlphaIgnoredTexture =
                        renderSolidAlphaScene(
                            {alphaReceiver, ignoredAlphaCaster}, false, false);
                    const QImage solidAlphaIgnoredShadow =
                        renderSolidAlphaScene(
                            {alphaReceiver, ignoredAlphaCaster}, true, false);
                    const auto renderXrayAlphaCenterDepth = [&] (
                        const ViewportRenderObject &caster) {
                        if (!depthOnlyFramebuffer.isValid()) {
                            return std::numeric_limits<float>::quiet_NaN();
                        }
                        depthOnlyFramebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        functions.glDisable(GL_SCISSOR_TEST);
                        functions.glDepthMask(GL_TRUE);
                        functions.glClearDepth(1.0);
                        functions.glClear(GL_DEPTH_BUFFER_BIT);
                        ViewportShadingSettings settings;
                        settings.mode = ViewportShadingMode::Solid;
                        settings.lightingMode = ViewportLightingMode::Flat;
                        settings.colorMode = ViewportColorMode::Texture;
                        settings.xray = true;
                        settings.xrayAlpha = 0.5;
                        settings.outline = false;
                        settings.shadows = false;
                        if (!gpuSurfaceRenderer.draw(
                                {caster}, gpuTransform, glSize, 1.0,
                                settings)) {
                            return std::numeric_limits<float>::quiet_NaN();
                        }
                        functions.glFinish();
                        QVector<float> depthValues(glSize.width() *
                                                   glSize.height());
                        functions.glReadPixels(
                            0, 0, glSize.width(), glSize.height(),
                            GL_DEPTH_COMPONENT, GL_FLOAT, depthValues.data());
                        return *std::min_element(depthValues.cbegin(),
                                                 depthValues.cend());
                    };
                    const float xrayTransparentCutoutDepth =
                        renderXrayAlphaCenterDepth(alphaCaster);
                    const float xrayIgnoredAlphaDepth =
                        renderXrayAlphaCenterDepth(ignoredAlphaCaster);
                    passed &= check(
                        packedTextureReady && raisedCasterValid &&
                            depthOnlyFramebuffer.isValid() &&
                            depthOnlyStencilBits == 0 &&
                            !solidAlphaReceiverOnly.isNull() &&
                            solidAlphaCutoutColor == solidAlphaReceiverOnly &&
                            solidAlphaCutoutId == solidAlphaReceiverOutline &&
                            solidAlphaCutoutShadow !=
                                solidAlphaIgnoredShadow &&
                            solidAlphaIgnoredTexture !=
                                solidAlphaReceiverOnly &&
                            solidAlphaIgnoredShadow !=
                                solidAlphaReceiverShadow &&
                            std::isfinite(xrayTransparentCutoutDepth) &&
                            std::isfinite(xrayIgnoredAlphaDepth) &&
                            xrayTransparentCutoutDepth > 0.999f &&
                            xrayIgnoredAlphaDepth < 0.999f,
                        "NurbsSolid texture alpha must mask color, object-ID outlines, depth-map shadows, and X-Ray depth consistently, while NONE bypasses the cutoff");

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
                    functions.glClearColor(34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuSurfaceRenderer.draw({surfaceObject}, gpuTransform,
                                            glSize, 1.0, gpuSolidShading);
                    functions.glFinish();
                    gpuUnselectedOutlineImage = framebuffer.toImage();
                    ViewportShadingSettings rawFaceShading = gpuSolidShading;
                    rawFaceShading.outline = false;
                    const QString qpainterAaCaptureDirectory =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_QPAINTER_AA_CAPTURE_DIR");
                    if (!qpainterAaCaptureDirectory.isEmpty()) {
                        const bool directoryReady = QDir().mkpath(
                            qpainterAaCaptureDirectory);
                        bool capturesSucceeded = directoryReady;
                        ViewportShadingSettings aaCaptureShading =
                            rawFaceShading;
                        aaCaptureShading.outline = false;
                        aaCaptureShading.shadows = false;
                        aaCaptureShading.depthOfField = false;
                        aaCaptureShading.cavity = false;
                        struct QPainterAaCaptureCase {
                            const char *name;
                            int mode;
                            int scale;
                        };
                        const QPainterAaCaptureCase captureCases[] = {
                            {"off", 0, 1}, {"single_pass", 1, 2},
                            {"samples5", 5, 2}, {"samples8", 8, 3},
                            {"samples11", 11, 3}, {"samples16", 16, 4},
                            {"samples32", 32, 6}};
                        for (const QPainterAaCaptureCase &captureCase :
                             captureCases) {
                            const qreal scale = captureCase.scale;
                            QImage supersampled(
                                glSize * captureCase.scale,
                                QImage::Format_ARGB32_Premultiplied);
                            supersampled.setDevicePixelRatio(scale);
                            supersampled.fill(Qt::transparent);
                            CurveHitTester qpainterAaHitTester;
                            ViewportRenderer qpainterAaRenderer(
                                gpuTransform, qpainterAaHitTester);
                            qpainterAaRenderer
                                .setWorkbenchRasterAntialiasingEnabled(
                                    captureCase.mode != 0);
                            qpainterAaRenderer.setShadingSettings(
                                aaCaptureShading);
                            const QVector<ViewportRenderObject> aaObjects{
                                polygonCubeObject};
                            const QVector3D backgroundSceneLinear =
                                workbenchSrgbToSceneLinear(QVector3D(
                                    34.0f / 255.0f, 34.0f / 255.0f,
                                    34.0f / 255.0f));
                            RasterWorkbenchSurfaceLayers surfaceLayers;
                            const bool composed =
                                compositeRasterWorkbenchSurfaces(
                                    &supersampled, aaObjects, gpuTransform,
                                    glSize, scale, captureCase.mode != 0,
                                    true, aaCaptureShading,
                                    backgroundSceneLinear,
                                    [&qpainterAaRenderer, &polygonCubeObject,
                                     &glSize](
                                        QPainter &surfacePainter,
                                        const ViewportRenderObject &object,
                                        RasterWorkbenchFragmentSink *sink) {
                                        qpainterAaRenderer.drawShape(
                                            surfacePainter, object.shape,
                                            glSize, false, object.selected,
                                            true, QColor(), QString(), 0.0,
                                            object.objectId,
                                            object.geometryRevision,
                                            object.preparedDepthGeometry.data(),
                                            {}, object.displayColorSceneLinear,
                                            object.workbenchMaterial,
                                            object.displayName,
                                            object.libraryPath,
                                            identityMeshObjectTransform,
                                            object.meshMaterials, sink);
                                    },
                                    &surfaceLayers);
                            if (!composed) {
                                supersampled.fill(Qt::transparent);
                                QPainter scenePainter(&supersampled);
                                scenePainter.setRenderHint(
                                    QPainter::Antialiasing,
                                    captureCase.mode != 0);
                                qpainterAaRenderer.drawShape(
                                    scenePainter, polygonCubeObject.shape,
                                    glSize, false, false, true, QColor(),
                                    QString(), 0.0,
                                    polygonCubeObject.objectId,
                                    polygonCubeObject.geometryRevision,
                                    polygonCubeObject.preparedDepthGeometry.data(),
                                    {}, polygonCubeObject.displayColorSceneLinear,
                                    std::nullopt,
                                    polygonCubeObject.displayName, QString(),
                                    identityMeshObjectTransform,
                                    polygonCubeObject.meshMaterials);
                                scenePainter.end();
                            } else {
                                applyRasterWorkbenchEffects(
                                    &supersampled, aaObjects, gpuTransform,
                                    glSize, scale, aaCaptureShading,
                                    &surfaceLayers);
                            }

                            QImage resolved(glSize,
                                            QImage::Format_ARGB32_Premultiplied);
                            resolved.fill(QColor(34, 34, 34));
                            QPainter resolvePainter(&resolved);
                            resolvePainter.drawImage(QPoint(0, 0),
                                                     supersampled);
                            resolvePainter.end();
                            const QString path = QDir(
                                qpainterAaCaptureDirectory).filePath(
                                    QString::fromLatin1(captureCase.name) +
                                    QStringLiteral(".png"));
                            capturesSucceeded &= !resolved.isNull() &&
                                                  resolved.save(path);
                        }
                        passed &= check(
                            capturesSucceeded,
                            "QPainter fallback AA matrix must capture the production mesh raster at each configured supersampling scale");
                    }
                    functions.glClearColor(34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuUnselectedBaselineDrawSucceeded =
                        gpuSurfaceRenderer.draw(
                            {surfaceObject}, gpuTransform, glSize, 1.0,
                            rawFaceShading);
                    functions.glFinish();
                    gpuUnselectedNoOutlineImage = framebuffer.toImage();

                    ViewportShadingSettings sceneLinearOutlineShading =
                        rawFaceShading;
                    sceneLinearOutlineShading.outlineColor =
                        QColor(128, 128, 128);
                    functions.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT |
                                      GL_DEPTH_BUFFER_BIT);
                    const bool sceneLinearOutlineBaselineDrawSucceeded =
                        gpuSurfaceRenderer.draw(
                            {surfaceObject}, gpuTransform, glSize, 1.0,
                            sceneLinearOutlineShading, false, true, false,
                            nullptr, nullptr, true);
                    const QImage sceneLinearOutlineBaseline =
                        framebuffer.toImage();
                    sceneLinearOutlineShading.outline = true;
                    functions.glClear(GL_COLOR_BUFFER_BIT |
                                      GL_DEPTH_BUFFER_BIT);
                    const bool sceneLinearOutlineDrawSucceeded =
                        gpuSurfaceRenderer.draw(
                            {surfaceObject}, gpuTransform, glSize, 1.0,
                            sceneLinearOutlineShading, false, true, false,
                            nullptr, nullptr, true);
                    const QImage sceneLinearOutline = framebuffer.toImage();
                    const QString sceneLinearOutlinePath = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_SCENE_LINEAR_OUTLINE_CAPTURE");
                    if (!sceneLinearOutlinePath.isEmpty()) {
                        sceneLinearOutline.save(sceneLinearOutlinePath);
                    }
                    const QString sceneLinearOutlineBaselinePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_SCENE_LINEAR_OUTLINE_BASELINE_CAPTURE");
                    if (!sceneLinearOutlineBaselinePath.isEmpty()) {
                        sceneLinearOutlineBaseline.save(
                            sceneLinearOutlineBaselinePath);
                    }
                    if (sceneLinearOutlineBaselineDrawSucceeded &&
                        sceneLinearOutlineDrawSucceeded &&
                        sceneLinearOutlineBaseline.size() == glSize &&
                        sceneLinearOutline.size() == glSize) {
                        for (int y = 0; y < glSize.height() &&
                                        !gpuSceneLinearOutlineColorPreserved;
                             ++y) {
                            for (int x = 0; x < glSize.width(); ++x) {
                                const QColor baseline =
                                    sceneLinearOutlineBaseline.pixelColor(x, y);
                                const QColor outlined =
                                    sceneLinearOutline.pixelColor(x, y);
                                if (baseline.red() <= 1 &&
                                    baseline.green() <= 1 &&
                                    baseline.blue() <= 1 &&
                                    std::abs(outlined.red() - 14) <= 2 &&
                                    std::abs(outlined.green() - 14) <= 2 &&
                                    std::abs(outlined.blue() - 14) <= 2) {
                                    gpuSceneLinearOutlineColorPreserved = true;
                                    break;
                                }
                            }
                        }
                    }

                    ViewportShadingSettings shadowShading = rawFaceShading;
                    shadowShading.shadows = true;
                    functions.glClearColor(34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           34.0f / 255.0f,
                                           1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    gpuShadowDrawSucceeded = gpuSurfaceRenderer.draw(
                        {surfaceObject}, gpuTransform, glSize, 1.0,
                        shadowShading);
                    functions.glFinish();
                    gpuShadowImage = framebuffer.toImage();

                    const auto renderCoordinatedScene = [&](
                        const ViewportShadingSettings &shading,
                        const QByteArray &historyKey, QImage *image,
                        const ViewportRenderObject *renderObject = nullptr,
                        const ViewportTransform *cameraOverride = nullptr,
                        const ViewportCameraPreferences *dofOptics = nullptr,
                        const QString &cavityDiagnosticPrefix = QString{},
                        QVector<float> *sceneLinearCapture = nullptr,
                        const QVector3D *backgroundSceneLinear = nullptr,
                        const QColor *viewportBackground = nullptr) {
                        if (image == nullptr || !framebuffer.bind()) {
                            return false;
                        }
                        const ViewportTransform &renderCamera =
                            cameraOverride == nullptr ? gpuTransform
                                                      : *cameraOverride;
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        ViewportAaRenderer sceneAaRenderer;
                        sceneAaRenderer.setMode(static_cast<int>(
                            ViewportAntiAliasingMode::Off));
                        sceneAaRenderer.setDisplayTransform(shading, true);
                        if (dofOptics != nullptr) {
                            sceneAaRenderer.setDepthOfField(
                                true, *dofOptics,
                                renderCamera.isPerspectiveEnabled(),
                                viewportProjectionClipRange(
                                    renderCamera, glSize).nearPlane,
                                viewportProjectionClipRange(
                                    renderCamera, glSize).farPlane);
                        }
                        if (!sceneAaRenderer.beginSceneSample(
                                glSize, 1.0, renderCamera, historyKey,
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
                        bool backgroundRendered = true;
                        if (backgroundSceneLinear != nullptr) {
                            BlenderGridRenderer backgroundRenderer;
                            backgroundRendered = backgroundRenderer
                                .renderBackgroundToCurrentFramebuffer(
                                    glSize, 1.0, *backgroundSceneLinear);
                        } else if (viewportBackground != nullptr) {
                            BlenderGridRenderer backgroundRenderer;
                            backgroundRendered = backgroundRenderer
                                .renderBackgroundToCurrentFramebuffer(
                                    glSize, 1.0, *viewportBackground, true);
                        } else {
                            const QVector3D background(
                                (low + high) * 0.5f,
                                (low + high) * 0.5f,
                                (low + high) * 0.5f);
                            const GLfloat linearBackground[4] = {
                                background.x(), background.y(),
                                background.z(), 1.0f};
                            functions.glClearBufferfv(GL_COLOR, 0,
                                                      linearBackground);
                        }
                        if (!backgroundRendered) {
                            sceneAaRenderer.finishSceneSample();
                            return false;
                        }
                        const QVector<ViewportRenderObject> renderObjects =
                            renderObject == nullptr
                                ? QVector<ViewportRenderObject>{surfaceObject}
                                : QVector<ViewportRenderObject>{*renderObject};
                        gpuSurfaceRenderer.setCavityDiagnosticOutput(
                            !cavityDiagnosticPrefix.isEmpty());
                        const auto result =
                            ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                                gpuSurfaceRenderer,
                                ViewportWorkbenchScenePassPacket::fromScene(
                                    renderObjects,
                                    sceneAaRenderer.sampleCamera(), glSize, 1.0,
                                    shading, &sceneAaRenderer.targets(), true));
                        gpuSurfaceRenderer.setCavityDiagnosticOutput(false);
                        if (result.rendered) {
                            sceneAaRenderer.markCurrentSampleAttachmentsValid(
                                result.report.populatedAttachments);
                        }
                        if (result.rendered && shading.cavity) {
                            const quint32 attachments =
                                result.report.populatedAttachments;
                            const bool opaquePrepassesCompleted =
                                result.completed(
                                    ViewportRenderPass::OpaqueDepthPrepass) &&
                                result.completed(
                                    ViewportRenderPass::OpaqueViewNormalPrepass) &&
                                result.completed(
                                    ViewportRenderPass::OpaqueObjectIdPrepass);
                            const quint32 opaqueAttachments =
                                ViewportRenderTargets::Depth |
                                ViewportRenderTargets::ViewSpaceNormal |
                                ViewportRenderTargets::ObjectId;
                            const bool opaqueAttachmentsPopulated =
                                (attachments & opaqueAttachments) ==
                                opaqueAttachments;
                        const quint32 frontAttachments =
                                ViewportRenderTargets::FrontDepth |
                                ViewportRenderTargets::FrontViewSpaceNormal |
                                ViewportRenderTargets::FrontObjectId |
                                ViewportRenderTargets::FrontOutlineId;
                        const bool noFrontDomainReported =
                            (attachments & frontAttachments) == 0 &&
                            !result.completed(
                                ViewportRenderPass::FrontDepthPrepass) &&
                            !result.completed(
                                ViewportRenderPass::FrontViewNormalPrepass) &&
                            !result.completed(
                                ViewportRenderPass::FrontObjectIdPrepass);
                            gpuNamedPrepassReportValid =
                                opaquePrepassesCompleted &&
                                opaqueAttachmentsPopulated &&
                            noFrontDomainReported;
                        }
                        if (result.rendered &&
                            !cavityDiagnosticPrefix.isEmpty()) {
                            const auto writePfm = [&](const QString &path,
                                                      const QVector<float> &values,
                                                      int channels) {
                                QFile file(path);
                                const QByteArray header =
                                    (channels == 1 ? QByteArrayLiteral("Pf\n")
                                                   : QByteArrayLiteral("PF\n")) +
                                    QByteArray::number(glSize.width()) +
                                    QByteArrayLiteral(" ") +
                                    QByteArray::number(glSize.height()) +
                                    QByteArrayLiteral("\n-1.0\n");
                                const qsizetype expected =
                                    qsizetype(glSize.width()) * glSize.height() *
                                    channels;
                                return values.size() == expected &&
                                    file.open(QIODevice::WriteOnly) &&
                                    file.write(header) == header.size() &&
                                    file.write(
                                        reinterpret_cast<const char *>(
                                            values.constData()),
                                        expected * qsizetype(sizeof(float))) ==
                                        expected * qsizetype(sizeof(float));
                            };
                            const auto readTextureRgb = [&](GLuint texture,
                                                            int firstChannel) {
                                QVector<float> rgba(glSize.width() *
                                                    glSize.height() * 4);
                                GLint oldActive = GL_TEXTURE0;
                                GLint oldBinding = 0;
                                functions.glGetIntegerv(GL_ACTIVE_TEXTURE,
                                                        &oldActive);
                                functions.glActiveTexture(GL_TEXTURE0);
                                functions.glGetIntegerv(GL_TEXTURE_BINDING_2D,
                                                        &oldBinding);
                                functions.glBindTexture(GL_TEXTURE_2D, texture);
                                functions.glGetTexImage(GL_TEXTURE_2D, 0,
                                                        GL_RGBA, GL_FLOAT,
                                                        rgba.data());
                                functions.glBindTexture(
                                    GL_TEXTURE_2D,
                                    static_cast<GLuint>(oldBinding));
                                functions.glActiveTexture(
                                    static_cast<GLenum>(oldActive));
                                QVector<float> rgb(glSize.width() *
                                                   glSize.height() * 3);
                                for (int pixel = 0;
                                     pixel < glSize.width() * glSize.height();
                                     ++pixel) {
                                    for (int channel = 0; channel < 3; ++channel) {
                                        rgb[pixel * 3 + channel] = rgba[
                                            pixel * 4 + firstChannel + channel];
                                    }
                                }
                                return rgb;
                            };
                            const ViewportRenderTargets &targets =
                                sceneAaRenderer.targets();
                            const bool amountsSaved = writePfm(
                                cavityDiagnosticPrefix +
                                    QStringLiteral("-amounts.pfm"),
                                readTextureRgb(
                                    targets.currentSampleColorTexture, 0), 3);
                            const bool normalsSaved = writePfm(
                                cavityDiagnosticPrefix +
                                    QStringLiteral("-normal-encoded.pfm"),
                                readTextureRgb(targets.viewSpaceNormalTexture,
                                               0), 3);
                            GLint oldReadFramebuffer = 0;
                            GLint oldReadBuffer = GL_NONE;
                            GLuint depthReadFramebuffer = 0;
                            functions.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,
                                                    &oldReadFramebuffer);
                            functions.glGetIntegerv(GL_READ_BUFFER,
                                                    &oldReadBuffer);
                            functions.glGenFramebuffers(1,
                                                        &depthReadFramebuffer);
                            functions.glBindFramebuffer(
                                GL_READ_FRAMEBUFFER, depthReadFramebuffer);
                            functions.glFramebufferTexture2D(
                                GL_READ_FRAMEBUFFER,
                                GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                targets.depthTexture, 0);
                            QVector<float> rawDepth(glSize.width() *
                                                    glSize.height());
                            const bool depthFramebufferReady =
                                functions.glCheckFramebufferStatus(
                                    GL_READ_FRAMEBUFFER) ==
                                GL_FRAMEBUFFER_COMPLETE;
                            if (depthFramebufferReady) {
                                functions.glReadBuffer(GL_NONE);
                                functions.glReadPixels(
                                    0, 0, glSize.width(), glSize.height(),
                                    GL_DEPTH_COMPONENT, GL_FLOAT,
                                    rawDepth.data());
                            }
                            functions.glBindFramebuffer(
                                GL_READ_FRAMEBUFFER,
                                static_cast<GLuint>(oldReadFramebuffer));
                            functions.glReadBuffer(
                                static_cast<GLenum>(oldReadBuffer));
                            functions.glDeleteFramebuffers(
                                1, &depthReadFramebuffer);
                            const bool depthSaved = depthFramebufferReady &&
                                writePfm(cavityDiagnosticPrefix +
                                             QStringLiteral("-depth-buffer.pfm"),
                                         rawDepth, 1);
                            if (!amountsSaved || !normalsSaved || !depthSaved) {
                                return false;
                            }
                        }
                        if (result.rendered && sceneLinearCapture != nullptr) {
                            const GLuint sourceTexture =
                                sceneAaRenderer.targets().currentSampleColorTexture;
                            QVector<float> rgba(glSize.width() * glSize.height() * 4);
                            GLint previousActive = GL_TEXTURE0;
                            GLint previousTexture = 0;
                            functions.glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
                            functions.glActiveTexture(GL_TEXTURE0);
                            functions.glGetIntegerv(GL_TEXTURE_BINDING_2D,
                                                    &previousTexture);
                            functions.glBindTexture(GL_TEXTURE_2D, sourceTexture);
                            functions.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA,
                                                    GL_FLOAT, rgba.data());
                            functions.glBindTexture(GL_TEXTURE_2D,
                                                    static_cast<GLuint>(previousTexture));
                            functions.glActiveTexture(
                                static_cast<GLenum>(previousActive));
                            sceneLinearCapture->resize(glSize.width() *
                                                       glSize.height() * 3);
                            for (int pixel = 0; pixel < glSize.width() * glSize.height();
                                 ++pixel) {
                                for (int channel = 0; channel < 3; ++channel) {
                                    (*sceneLinearCapture)[pixel * 3 + channel] =
                                        rgba[pixel * 4 + channel];
                                }
                            }
                        }
                        sceneAaRenderer.finishSceneSample();
                        if (!result.rendered) return false;
                        functions.glFinish();
                        *image = framebuffer.toImage();
                        return !image->isNull();
                    };
                    // A face-material split can make one imported mesh
                    // transparent as a whole even when its first draw range
                    // is opaque. Blender keeps its validated shared volume
                    // and forces depth-fail; its binary shadow must ignore
                    // the fully transparent image on the raised component.
                    ViewportRenderObject mixedAlphaShadowMesh =
                        polygonCubeObject;
                    mixedAlphaShadowMesh.objectId = ObjectId::fromValue(804);
                    const QVector<Point3D> receiverVertices =
                        mixedAlphaShadowMesh.shape.polygonMesh.vertices;
                    const QVector<QVector<int>> receiverFaces =
                        mixedAlphaShadowMesh.shape.polygonMesh.faces;
                    for (const Point3D &vertex : receiverVertices) {
                        mixedAlphaShadowMesh.shape.polygonMesh.vertices.append(
                            {vertex.x, vertex.y, vertex.z + 1.2});
                    }
                    for (const QVector<int> &face : receiverFaces) {
                        QVector<int> raisedFace;
                        raisedFace.reserve(face.size());
                        for (const int vertexIndex : face) {
                            raisedFace.append(vertexIndex +
                                receiverVertices.size());
                        }
                        mixedAlphaShadowMesh.shape.polygonMesh.faces.append(
                            raisedFace);
                    }
                    mixedAlphaShadowMesh.shape.polygonMesh.faceMaterialIndices =
                        QVector<int>(receiverFaces.size(), 0);
                    for (qsizetype faceIndex = 0;
                         faceIndex < receiverFaces.size(); ++faceIndex) {
                        mixedAlphaShadowMesh.shape.polygonMesh
                            .faceMaterialIndices.append(1);
                    }
                    QVector<QPointF> mixedShadowUvs;
                    for (const QVector<int> &face :
                         mixedAlphaShadowMesh.shape.polygonMesh.faces) {
                        for (qsizetype corner = 0; corner < face.size();
                             ++corner) {
                            mixedShadowUvs.append(QPointF(0.5, 0.5));
                        }
                    }
                    mixedAlphaShadowMesh.shape.polygonMesh.uvMaps = {
                        {QStringLiteral("ShadowAlphaUV"), mixedShadowUvs}};
                    mixedAlphaShadowMesh.shape.polygonMesh.activeUvMapName =
                        QStringLiteral("ShadowAlphaUV");
                    mixedAlphaShadowMesh.shape.polygonMesh.defaultUvMapName =
                        QStringLiteral("ShadowAlphaUV");
                    WorkbenchMaterialData transparentShadowMaterial;
                    transparentShadowMaterial.alpha = 0.5f;
                    transparentShadowMaterial.textureImagePackedDataBase64 =
                        QString::fromLatin1(packedTextureBytes.toBase64());
                    transparentShadowMaterial.textureImageSrgb = false;
                    transparentShadowMaterial.textureImageAlphaMode =
                        QStringLiteral("STRAIGHT");
                    transparentShadowMaterial.textureImageInterpolation =
                        QStringLiteral("Closest");
                    mixedAlphaShadowMesh.meshMaterials = {
                        WorkbenchMaterialData{}, transparentShadowMaterial};
                    mixedAlphaShadowMesh.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(
                                mixedAlphaShadowMesh.shape));
                    ViewportShadingSettings texturedShadowShading =
                        shadowShading;
                    texturedShadowShading.colorMode =
                        ViewportColorMode::Texture;
                    QImage opaqueReceiverShadowImage;
                    QImage transparentMeshShadowImage;
                    const bool opaqueReceiverShadowRendered =
                        renderCoordinatedScene(
                            texturedShadowShading,
                            QByteArrayLiteral("transparent-mesh-shadow-baseline"),
                            &opaqueReceiverShadowImage, &polygonCubeObject);
                    const bool transparentMeshShadowRendered =
                        renderCoordinatedScene(
                            texturedShadowShading,
                            QByteArrayLiteral("transparent-mesh-shadow-volume"),
                            &transparentMeshShadowImage,
                            &mixedAlphaShadowMesh);
                    int transparentMeshShadowChangedPixels = 0;
                    if (opaqueReceiverShadowRendered &&
                        transparentMeshShadowRendered &&
                        opaqueReceiverShadowImage.size() ==
                            transparentMeshShadowImage.size()) {
                        for (int y = 0;
                             y < opaqueReceiverShadowImage.height(); ++y) {
                            for (int x = 0;
                                 x < opaqueReceiverShadowImage.width(); ++x) {
                                const QColor baseline =
                                    opaqueReceiverShadowImage.pixelColor(x, y);
                                const QColor withCaster =
                                    transparentMeshShadowImage.pixelColor(x, y);
                                if (std::abs(baseline.red() - withCaster.red()) > 1 ||
                                    std::abs(baseline.green() - withCaster.green()) > 1 ||
                                    std::abs(baseline.blue() - withCaster.blue()) > 1) {
                                    ++transparentMeshShadowChangedPixels;
                                }
                            }
                        }
                    }
                    passed &= check(
                        packedTextureReady &&
                            opaqueReceiverShadowRendered &&
                            transparentMeshShadowRendered &&
                            transparentMeshShadowChangedPixels > 0,
                        "transparent polygon-mesh material ranges must retain binary stencil casting despite a fully transparent image-alpha texture");
                    // Exercise the production scene-color target, float
                    // texture readback, and HDR writer with a controlled
                    // scene-linear background above display range.
                    const QVector3D hdrBackgroundSceneLinear(4.0f, 0.75f,
                                                              0.25f);
                    QVector<float> hdrReadbackRgb;
                    QImage hdrReadbackDisplay;
                    const bool hdrSceneRendered = renderCoordinatedScene(
                        rawFaceShading,
                        QByteArrayLiteral("hdr-scene-linear-readback"),
                        &hdrReadbackDisplay, &polygonCubeObject, nullptr,
                        nullptr, QString{}, &hdrReadbackRgb,
                        &hdrBackgroundSceneLinear);
                    QVector3D hdrReadbackPixel;
                    bool hdrReadbackPixelFound = false;
                    if (hdrSceneRendered &&
                        hdrReadbackRgb.size() == glSize.width() *
                            glSize.height() * 3) {
                        for (int pixel = 0;
                             pixel < glSize.width() * glSize.height(); ++pixel) {
                            const QVector3D candidate(
                                hdrReadbackRgb[pixel * 3],
                                hdrReadbackRgb[pixel * 3 + 1],
                                hdrReadbackRgb[pixel * 3 + 2]);
                            if (candidate.x() > 1.0f) {
                                hdrReadbackPixel = candidate;
                                hdrReadbackPixelFound = true;
                                break;
                            }
                        }
                    }
                    WorkbenchRenderResult hdrReadbackResult;
                    hdrReadbackResult.combined =
                        QImage(2, 2, QImage::Format_RGBA64);
                    hdrReadbackResult.combined.fill(Qt::black);
                    hdrReadbackResult.sceneLinearSize = QSize(2, 2);
                    hdrReadbackResult.sceneLinearRgba.resize(16);
                    for (int pixel = 0; pixel < 4; ++pixel) {
                        hdrReadbackResult.sceneLinearRgba[pixel * 4] =
                            hdrReadbackPixel.x();
                        hdrReadbackResult.sceneLinearRgba[pixel * 4 + 1] =
                            hdrReadbackPixel.y();
                        hdrReadbackResult.sceneLinearRgba[pixel * 4 + 2] =
                            hdrReadbackPixel.z();
                        hdrReadbackResult.sceneLinearRgba[pixel * 4 + 3] = 1.0f;
                    }
                    hdrReadbackResult.blenderImageFormat =
                        QStringLiteral("HDR");
                    QTemporaryDir hdrReadbackDirectory;
                    const QString hdrReadbackPath =
                        hdrReadbackDirectory.filePath(
                            QStringLiteral("gpu-readback.hdr"));
                    QString hdrReadbackError;
                    const bool hdrReadbackWritten =
                        hdrReadbackPixelFound && hdrReadbackDirectory.isValid() &&
                        hdrReadbackResult.saveCombinedImage(
                            hdrReadbackPath, &hdrReadbackError);
                    QFile hdrReadbackFile(hdrReadbackPath);
                    bool hdrReadbackDecoded = hdrReadbackWritten &&
                        hdrReadbackFile.open(QIODevice::ReadOnly);
                    QByteArray hdrResolutionLine;
                    while (hdrReadbackDecoded) {
                        hdrResolutionLine = hdrReadbackFile.readLine();
                        if (hdrResolutionLine.isEmpty() ||
                            hdrResolutionLine.startsWith("-Y ")) {
                            break;
                        }
                    }
                    const QByteArray encodedHdrPixel =
                        hdrReadbackDecoded && hdrResolutionLine.startsWith("-Y ")
                        ? hdrReadbackFile.read(4) : QByteArray{};
                    if (encodedHdrPixel.size() == 4 &&
                        static_cast<unsigned char>(encodedHdrPixel[3]) != 0) {
                        const int exponent =
                            static_cast<unsigned char>(encodedHdrPixel[3]) - 128;
                        const float scale = std::ldexp(1.0f, exponent) / 256.0f;
                        const float decodedRed =
                            static_cast<unsigned char>(encodedHdrPixel[0]) * scale;
                        hdrReadbackDecoded = decodedRed > 1.0f &&
                            std::abs(decodedRed - hdrReadbackPixel.x()) < 0.04f;
                    } else {
                        hdrReadbackDecoded = false;
                    }
                    passed &= check(
                        hdrSceneRendered && hdrReadbackPixelFound &&
                            hdrReadbackPixel.x() > 1.0f &&
                            hdrReadbackResult.hasValidSceneLinearCombined() &&
                            hdrReadbackWritten && hdrReadbackDecoded,
                        "GPU RGBA16F scene resolve and float readback must preserve values above 1 through Radiance HDR output");
                    // Removing one cube face leaves a manifold sheet with one
                    // simple boundary loop. It must still contribute its
                    // boundary-closed forced-fail stencil volume.
                    ViewportRenderObject openPolygonMesh = polygonCubeObject;
                    openPolygonMesh.objectId = ObjectId::fromValue(803);
                    openPolygonMesh.shape.polygonMesh.faces.removeLast();
                    openPolygonMesh.shape.polygonMesh.faceMaterialIndices =
                        QVector<int>(openPolygonMesh.shape.polygonMesh.faces.size(), 0);
                    openPolygonMesh.preparedDepthGeometry =
                        QSharedPointer<ViewportDepthGeometry>::create(
                            buildViewportDepthGeometry(openPolygonMesh.shape));
                    QImage openMeshBaseline;
                    QImage openMeshShadow;
                    const bool openMeshBaselineRendered = renderCoordinatedScene(
                        rawFaceShading, QByteArrayLiteral("open-mesh-shadow-baseline"),
                        &openMeshBaseline, &openPolygonMesh);
                    const bool openMeshShadowRendered = renderCoordinatedScene(
                        shadowShading, QByteArrayLiteral("open-mesh-shadow"),
                        &openMeshShadow, &openPolygonMesh);
                    int openMeshShadowChangedPixels = 0;
                    if (openMeshBaselineRendered && openMeshShadowRendered &&
                        openMeshBaseline.size() == openMeshShadow.size()) {
                        for (int y = 0; y < openMeshShadow.height(); ++y) {
                            for (int x = 0; x < openMeshShadow.width(); ++x) {
                                const QColor base = openMeshBaseline.pixelColor(x, y);
                                const QColor shadow = openMeshShadow.pixelColor(x, y);
                                if (std::max({base.red() - shadow.red(),
                                              base.green() - shadow.green(),
                                              base.blue() - shadow.blue()}) >= 2) {
                                    ++openMeshShadowChangedPixels;
                                }
                            }
                        }
                    }
                    passed &= check(openMeshBaselineRendered &&
                                        openMeshShadowRendered &&
                                        openMeshShadowChangedPixels > 0,
                                    "open manifold polygon meshes must render a stencil shadow volume along their simple boundary loop");
                    const QString polygonCubeCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_POLYGON_CUBE_CAPTURE");
                    const QString polygonCubeLinearPath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_POLYGON_CUBE_LINEAR_CAPTURE");
                    if (!polygonCubeCapturePath.isEmpty() &&
                        !polygonCubeLinearPath.isEmpty()) {
                        QImage polygonCubeCapture;
                        QVector<float> polygonCubeLinear;
                        const bool rendered =
                            validatePolygonMesh(
                                polygonCubeObject.shape.polygonMesh) &&
                            renderCoordinatedScene(
                                rawFaceShading,
                                QByteArrayLiteral("polygon-cube-baseline"),
                                &polygonCubeCapture, &polygonCubeObject,
                                nullptr, nullptr, QString{}, &polygonCubeLinear);
                        QFile linearFile(polygonCubeLinearPath);
                        const QByteArray pfmHeader =
                            QByteArrayLiteral("PF\n640 480\n-1.0\n");
                        const bool linearSaved = rendered &&
                            linearFile.open(QIODevice::WriteOnly) &&
                            linearFile.write(pfmHeader) == pfmHeader.size() &&
                            linearFile.write(reinterpret_cast<const char *>(
                                polygonCubeLinear.constData()),
                                polygonCubeLinear.size() * qsizetype(sizeof(float))) ==
                                polygonCubeLinear.size() * qsizetype(sizeof(float));
                        const bool displaySaved = rendered &&
                            polygonCubeCapture.save(polygonCubeCapturePath);
                        bool displayNonblank = false;
                        if (rendered && !polygonCubeCapture.isNull()) {
                            for (int y = 0; y < polygonCubeCapture.height() &&
                                            !displayNonblank; ++y) {
                                for (int x = 0; x < polygonCubeCapture.width(); ++x) {
                                    if (polygonCubeCapture.pixelColor(x, y) !=
                                        QColor(0, 0, 0)) {
                                        displayNonblank = true;
                                        break;
                                    }
                                }
                            }
                        }
                        const bool linearNonblank = rendered &&
                            std::any_of(polygonCubeLinear.cbegin(),
                                        polygonCubeLinear.cend(),
                                [](float channel) { return channel > 1.0e-6f; });
                        passed &= check(
                            rendered && linearSaved && displaySaved &&
                                displayNonblank && linearNonblank,
                            "matched polygon cube capture must produce nonblank scene-linear and final display output");
                    }
                    const QString lightingCaptureDirectory = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR");
                    if (!lightingCaptureDirectory.isEmpty()) {
                        const bool captureSpecular =
                            qEnvironmentVariableIntValue(
                                "CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_NO_SPECULAR") == 0;
                        const bool directoryReady =
                            QDir().mkpath(lightingCaptureDirectory);
                        bool lightingCapturesSucceeded = directoryReady;
                        struct LightingCaptureCase {
                            const char *name;
                            ViewportLightingMode mode;
                            const char *preset;
                            bool worldSpaceLighting;
                            int rotationDegrees;
                        };
                        const LightingCaptureCase lightingCases[] = {
                            {"studio_default", ViewportLightingMode::Studio,
                             "Default", false, 0},
                            {"studio_paint_world_0",
                             ViewportLightingMode::Studio, "Paint", true, 0},
                            {"studio_paint_world_90",
                             ViewportLightingMode::Studio, "Paint", true, 90},
                            {"studio_paint_world_37",
                             ViewportLightingMode::Studio, "Paint", true, 37},
                            {"matcap_metal_bronze", ViewportLightingMode::MatCap,
                             "metal_bronze", false, 0},
                            {"matcap_check_normal_y",
                             ViewportLightingMode::MatCap, "check_normal+y",
                             false, 0},
                            {"matcap_ceramic_lightbulb",
                             ViewportLightingMode::MatCap,
                             "ceramic_lightbulb", false, 0},
                            {"flat", ViewportLightingMode::Flat, "Default",
                             false, 0}};
                        for (const LightingCaptureCase &captureCase :
                             lightingCases) {
                            ViewportShadingSettings lightingShading =
                                rawFaceShading;
                            lightingShading.lightingMode = captureCase.mode;
                            lightingShading.studioLightPreset =
                                QString::fromLatin1(captureCase.preset);
                            lightingShading.matcapPreset =
                                QString::fromLatin1(captureCase.preset);
                            lightingShading.worldSpaceLighting =
                                captureCase.worldSpaceLighting;
                            lightingShading.studioLightRotationDegrees =
                                captureCase.rotationDegrees;
                            lightingShading.specularLighting = captureSpecular;
                            QString stem = QString::fromLatin1(captureCase.name);
                            if (!captureSpecular) {
                                stem += QStringLiteral("_no_specular");
                            }
                            QImage display;
                            QVector<float> sceneLinear;
                            const bool rendered = renderCoordinatedScene(
                                lightingShading,
                                QByteArrayLiteral("lighting-match-") +
                                    stem.toUtf8(),
                                &display, &polygonCubeObject, nullptr, nullptr,
                                QString{}, &sceneLinear);
                            const QString displayPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    stem + QStringLiteral(".png"));
                            const QString linearPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    stem + QStringLiteral(".pfm"));
                            QFile linearFile(linearPath);
                            const QByteArray pfmHeader =
                                QByteArrayLiteral("PF\n640 480\n-1.0\n");
                            const qsizetype expectedValues = 640 * 480 * 3;
                            const bool linearSaved = rendered &&
                                sceneLinear.size() == expectedValues &&
                                linearFile.open(QIODevice::WriteOnly) &&
                                linearFile.write(pfmHeader) == pfmHeader.size() &&
                                linearFile.write(reinterpret_cast<const char *>(
                                    sceneLinear.constData()),
                                    sceneLinear.size() *
                                        qsizetype(sizeof(float))) ==
                                    sceneLinear.size() * qsizetype(sizeof(float));
                            const bool displaySaved = rendered &&
                                display.size() == QSize(640, 480) &&
                                display.save(displayPath);
                            lightingCapturesSucceeded &= linearSaved &&
                                                         displaySaved;
                            if (captureCase.mode ==
                                ViewportLightingMode::MatCap) {
                                CurveHitTester qpainterMatcapHitTester;
                                ViewportRenderer qpainterMatcapRenderer(
                                    gpuTransform, qpainterMatcapHitTester);
                                qpainterMatcapRenderer.setShadingSettings(
                                    lightingShading);
                                QImage qpainterDisplay(
                                    glSize,
                                    QImage::Format_ARGB32_Premultiplied);
                                qpainterDisplay.fill(QColor(34, 34, 34));
                                RasterWorkbenchFragmentSink qpainterFragments;
                                constexpr float qpainterBackgroundSceneLinear =
                                    0.0219421387f;
                                const QVector3D qpainterBackgroundLinear(
                                    qpainterBackgroundSceneLinear,
                                    qpainterBackgroundSceneLinear,
                                    qpainterBackgroundSceneLinear);
                                const QVector<ViewportRenderObject>
                                    qpainterMatcapObjects{
                                        polygonCubeObject};
                                const bool qpainterMatcapComposed =
                                    compositeRasterWorkbenchSurfaces(
                                        &qpainterDisplay,
                                        qpainterMatcapObjects, gpuTransform,
                                        glSize, 1.0, false, true,
                                        lightingShading,
                                        qpainterBackgroundLinear,
                                        [&qpainterMatcapRenderer,
                                         &qpainterFragments, &glSize](
                                            QPainter &surfacePainter,
                                            const ViewportRenderObject &object,
                                            RasterWorkbenchFragmentSink *sink) {
                                            qpainterMatcapRenderer.drawShape(
                                                surfacePainter, object.shape,
                                                glSize, false, object.selected,
                                                true, QColor(), QString(), 0.0,
                                                object.objectId,
                                                object.geometryRevision,
                                                object.preparedDepthGeometry.data(),
                                                {}, object.displayColorSceneLinear,
                                                object.workbenchMaterial,
                                                object.displayName,
                                                object.libraryPath,
                                                identityMeshObjectTransform,
                                                object.meshMaterials, sink);
                                            if (sink != nullptr) {
                                                qpainterFragments = *sink;
                                            }
                                        });
                                if (qpainterMatcapComposed) {
                                    applyRasterWorkbenchEffects(
                                        &qpainterDisplay,
                                        qpainterMatcapObjects, gpuTransform,
                                        glSize, 1.0, lightingShading);
                                } else {
                                    // The widget uses this same direct draw
                                    // path when its bounded surface compositor
                                    // declines the scene.
                                    QPainter qpainter(&qpainterDisplay);
                                    qpainterMatcapRenderer.drawShape(
                                        qpainter, polygonCubeObject.shape,
                                        glSize, false, false, true, QColor(),
                                        QString(), 0.0,
                                        polygonCubeObject.objectId,
                                        polygonCubeObject.geometryRevision,
                                        polygonCubeObject.preparedDepthGeometry.data(),
                                        {},
                                        polygonCubeObject.displayColorSceneLinear,
                                        std::nullopt,
                                        polygonCubeObject.displayName, QString(),
                                        identityMeshObjectTransform,
                                        polygonCubeObject.meshMaterials,
                                        &qpainterFragments);
                                    qpainter.end();
                                }
                                QVector<float> qpainterSceneLinear(
                                    expectedValues);
                                for (qsizetype pixel = 0;
                                     pixel + 2 < qpainterSceneLinear.size();
                                     pixel += 3) {
                                    qpainterSceneLinear[pixel] =
                                        qpainterBackgroundSceneLinear;
                                    qpainterSceneLinear[pixel + 1] =
                                        qpainterBackgroundSceneLinear;
                                    qpainterSceneLinear[pixel + 2] =
                                        qpainterBackgroundSceneLinear;
                                }
                                for (const RasterWorkbenchLinearFragment &fragment :
                                     qpainterFragments) {
                                    if (fragment.x < 0 ||
                                        fragment.x >= glSize.width() ||
                                        fragment.y < 0 ||
                                        fragment.y >= glSize.height()) {
                                        continue;
                                    }
                                    const qsizetype pixel =
                                        (qsizetype(fragment.y) * glSize.width() +
                                         fragment.x) * 3;
                                    qpainterSceneLinear[pixel] =
                                        fragment.sceneLinearRgb[0];
                                    qpainterSceneLinear[pixel + 1] =
                                        fragment.sceneLinearRgb[1];
                                    qpainterSceneLinear[pixel + 2] =
                                        fragment.sceneLinearRgb[2];
                                }
                                const QString qpainterStem = stem +
                                    QStringLiteral("_qpainter");
                                const QString qpainterDisplayPath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        qpainterStem + QStringLiteral(".png"));
                                const QString qpainterLinearPath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        qpainterStem + QStringLiteral(".pfm"));
                                QVector<float> qpainterSceneLinearPfm(
                                    expectedValues);
                                const qsizetype rowValues =
                                    qsizetype(glSize.width()) * 3;
                                for (int y = 0; y < glSize.height(); ++y) {
                                    std::copy_n(
                                        qpainterSceneLinear.constData() +
                                            qsizetype(y) * rowValues,
                                        rowValues,
                                        qpainterSceneLinearPfm.data() +
                                            qsizetype(glSize.height() - 1 - y) *
                                                rowValues);
                                }
                                QFile qpainterLinearFile(qpainterLinearPath);
                                const bool qpainterLinearSaved =
                                    qpainterSceneLinearPfm.size() == expectedValues &&
                                    qpainterLinearFile.open(QIODevice::WriteOnly) &&
                                    qpainterLinearFile.write(pfmHeader) ==
                                        pfmHeader.size() &&
                                    qpainterLinearFile.write(
                                        reinterpret_cast<const char *>(
                                            qpainterSceneLinearPfm.constData()),
                                        qpainterSceneLinearPfm.size() *
                                            qsizetype(sizeof(float))) ==
                                        qpainterSceneLinearPfm.size() *
                                            qsizetype(sizeof(float));
                                lightingCapturesSucceeded &=
                                    qpainterLinearSaved &&
                                    qpainterDisplay.save(qpainterDisplayPath);
                            }
                        }
                        struct ColorSourceCaptureCase {
                            const char *name;
                            ViewportColorMode colorMode;
                            MeshAttributeDomain attributeDomain =
                                MeshAttributeDomain::Corner;
                        };
                        const ColorSourceCaptureCase colorSourceCases[] = {
                            {"color_material", ViewportColorMode::Material},
                            {"color_object", ViewportColorMode::Object},
                            {"color_random", ViewportColorMode::Random},
                            {"color_custom", ViewportColorMode::Custom},
                            {"color_attribute", ViewportColorMode::Attribute},
                            {"color_attribute_point",
                             ViewportColorMode::Attribute,
                             MeshAttributeDomain::Point}};
                        for (const ColorSourceCaptureCase &captureCase :
                             colorSourceCases) {
                            ViewportRenderObject colorSourceObject =
                                polygonCubeObject;
                            colorSourceObject.displayName =
                                QStringLiteral("ReferenceCube");
                            colorSourceObject.displayColorSceneLinear =
                                QVector4D(0.12f, 0.62f, 0.18f, 1.0f);
                            colorSourceObject.meshMaterials[0].baseColorSceneLinear =
                                QVector3D(0.23f, 0.47f, 0.71f);
                            ViewportShadingSettings colorShading = rawFaceShading;
                            colorShading.lightingMode = ViewportLightingMode::Flat;
                            colorShading.colorMode = captureCase.colorMode;
                            if (captureCase.colorMode ==
                                ViewportColorMode::Custom) {
                                const QVector3D customLinear(0.31f, 0.18f,
                                                             0.72f);
                                const QVector3D customSrgb =
                                    workbenchSceneLinearToSrgb(customLinear);
                                colorShading.customColor = QColor::fromRgbF(
                                    customSrgb.x(), customSrgb.y(),
                                    customSrgb.z());
                            }
                            if (captureCase.colorMode ==
                                ViewportColorMode::Attribute) {
                                MeshColorAttribute attribute;
                                attribute.name = QStringLiteral("Color");
                                attribute.domain = captureCase.attributeDomain;
                                attribute.dataType =
                                    QStringLiteral("FLOAT_COLOR");
                                attribute.colorSpace =
                                    QStringLiteral("scene_linear");
                                attribute.componentCount = 4;
                                const PolygonMesh3D &mesh =
                                    colorSourceObject.shape.polygonMesh;
                                const auto appendPointColor =
                                    [&attribute](const Point3D &point) {
                                        const auto channel = [](double value) {
                                            return float(0.1 +
                                                0.35 * (value + 1.0));
                                        };
                                        attribute.values.append(channel(point.x));
                                        attribute.values.append(channel(point.y));
                                        attribute.values.append(channel(point.z));
                                        attribute.values.append(1.0f);
                                    };
                                if (attribute.domain ==
                                    MeshAttributeDomain::Point) {
                                    for (const Point3D &point : mesh.vertices)
                                        appendPointColor(point);
                                } else {
                                    for (const QVector<int> &face : mesh.faces) {
                                        for (int vertexIndex : face)
                                            appendPointColor(
                                                mesh.vertices[vertexIndex]);
                                    }
                                }
                                colorSourceObject.shape.polygonMesh
                                    .colorAttributes = {attribute};
                                colorSourceObject.shape.polygonMesh
                                    .activeColorAttributeName =
                                        attribute.name;
                            }
                            const QString stem = QString::fromLatin1(
                                captureCase.name);
                            QImage display;
                            QVector<float> sceneLinear;
                            const bool rendered = renderCoordinatedScene(
                                colorShading,
                                QByteArrayLiteral("color-source-match-") +
                                    stem.toUtf8(),
                                &display, &colorSourceObject, nullptr, nullptr,
                                QString{}, &sceneLinear);
                            const QString displayPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    stem + QStringLiteral(".png"));
                            const QString linearPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    stem + QStringLiteral(".pfm"));
                            QFile linearFile(linearPath);
                            const QByteArray pfmHeader =
                                QByteArrayLiteral("PF\n640 480\n-1.0\n");
                            const qsizetype expectedValues = 640 * 480 * 3;
                            const bool linearSaved = rendered &&
                                sceneLinear.size() == expectedValues &&
                                linearFile.open(QIODevice::WriteOnly) &&
                                linearFile.write(pfmHeader) == pfmHeader.size() &&
                                linearFile.write(reinterpret_cast<const char *>(
                                    sceneLinear.constData()),
                                    sceneLinear.size() *
                                        qsizetype(sizeof(float))) ==
                                    sceneLinear.size() * qsizetype(sizeof(float));
                            const bool displaySaved = rendered &&
                                display.size() == QSize(640, 480) &&
                                display.save(displayPath);
                            lightingCapturesSucceeded &= linearSaved &&
                                                         displaySaved;
                        }

                        ViewportRenderObject textureSourceObject =
                            polygonCubeObject;
                        textureSourceObject.displayName =
                            QStringLiteral("ReferenceCube");
                        textureSourceObject.displayColorSceneLinear =
                            QVector4D(0.12f, 0.62f, 0.18f, 1.0f);
                        QVector<QPointF> textureUvCorners;
                        for (const QVector<int> &face :
                             textureSourceObject.shape.polygonMesh.faces) {
                            for (qsizetype corner = 0;
                                 corner < face.size(); ++corner) {
                                textureUvCorners.append(QPointF(0.5, 0.5));
                            }
                        }
                        textureSourceObject.shape.polygonMesh.uvMaps = {
                            {QStringLiteral("WorkbenchTextureUV"),
                             textureUvCorners}};
                        textureSourceObject.shape.polygonMesh.activeUvMapName =
                            QStringLiteral("WorkbenchTextureUV");
                        textureSourceObject.shape.polygonMesh.defaultUvMapName =
                            QStringLiteral("WorkbenchTextureUV");
                        const QString textureAssetPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("workbench-texture.png"));
                        QImage textureAsset(1, 1, QImage::Format_RGBA8888);
                        textureAsset.fill(QColor(192, 128, 64, 255));
                        const bool textureAssetSaved =
                            textureAsset.save(textureAssetPath);
                        if (!textureSourceObject.meshMaterials.isEmpty()) {
                            WorkbenchMaterialData &textureMaterial =
                                textureSourceObject.meshMaterials[0];
                            textureMaterial.baseColorSceneLinear =
                                QVector3D(0.91f, 0.12f, 0.05f);
                            textureMaterial.textureImagePath = textureAssetPath;
                            textureMaterial.textureImageSrgb = true;
                            textureMaterial.textureImageAlphaMode =
                                QStringLiteral("STRAIGHT");
                            textureMaterial.textureImageInterpolation =
                                QStringLiteral("Closest");
                            textureMaterial.textureImageExtension =
                                QStringLiteral("REPEAT");
                        }
                        ViewportShadingSettings textureColorShading =
                            rawFaceShading;
                        textureColorShading.lightingMode =
                            ViewportLightingMode::Flat;
                        textureColorShading.colorMode =
                            ViewportColorMode::Texture;
                        QImage textureDisplay;
                        QVector<float> textureSceneLinear;
                        const bool textureRendered = renderCoordinatedScene(
                            textureColorShading,
                            QByteArrayLiteral("color-source-match-texture"),
                            &textureDisplay, &textureSourceObject, nullptr,
                            nullptr, QString{}, &textureSceneLinear);
                        const QString textureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture.png"));
                        const QString textureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture.pfm"));
                        QFile textureLinearFile(textureLinearPath);
                        const QByteArray texturePfmHeader =
                            QByteArrayLiteral("PF\n640 480\n-1.0\n");
                        const qsizetype textureExpectedValues = 640 * 480 * 3;
                        const bool textureLinearSaved = textureRendered &&
                            textureSceneLinear.size() == textureExpectedValues &&
                            textureLinearFile.open(QIODevice::WriteOnly) &&
                            textureLinearFile.write(texturePfmHeader) ==
                                texturePfmHeader.size() &&
                            textureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    textureSceneLinear.constData()),
                                textureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                textureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool textureDisplaySaved = textureRendered &&
                            textureDisplay.size() == QSize(640, 480) &&
                            textureDisplay.save(textureDisplayPath);
                        lightingCapturesSucceeded &= textureAssetSaved &&
                            textureLinearSaved && textureDisplaySaved;

                        ViewportRenderObject patternedTextureObject =
                            textureSourceObject;
                        QVector<QPointF> patternedUvCorners;
                        const PolygonMesh3D &patternedMesh =
                            patternedTextureObject.shape.polygonMesh;
                        for (const QVector<int> &face : patternedMesh.faces) {
                            for (int vertexIndex : face) {
                                const Point3D &point =
                                    patternedMesh.vertices[vertexIndex];
                                patternedUvCorners.append(QPointF(
                                    (point.x + 1.0) * 0.5,
                                    (point.z + 1.0) * 0.5));
                            }
                        }
                        patternedTextureObject.shape.polygonMesh.uvMaps[0]
                            .cornerValues = patternedUvCorners;
                        const QString patternedTexturePath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("workbench-texture-pattern.png"));
                        QImage patternedTexture(2, 2,
                                                QImage::Format_RGBA8888);
                        patternedTexture.setPixelColor(0, 0,
                                                       QColor(240, 32, 16));
                        patternedTexture.setPixelColor(1, 0,
                                                       QColor(24, 224, 48));
                        patternedTexture.setPixelColor(0, 1,
                                                       QColor(32, 64, 240));
                        patternedTexture.setPixelColor(1, 1,
                                                       QColor(240, 208, 24));
                        const bool patternedTextureSaved =
                            patternedTexture.save(patternedTexturePath);
                        const QString patternedTextureSecondaryPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "workbench-texture-pattern-secondary.png"));
                        QImage patternedTextureSecondary(
                            2, 2, QImage::Format_RGBA8888);
                        patternedTextureSecondary.setPixelColor(
                            0, 0, QColor(16, 224, 232));
                        patternedTextureSecondary.setPixelColor(
                            1, 0, QColor(232, 24, 200));
                        patternedTextureSecondary.setPixelColor(
                            0, 1, QColor(216, 208, 32));
                        patternedTextureSecondary.setPixelColor(
                            1, 1, QColor(32, 48, 216));
                        const bool patternedTextureSecondarySaved =
                            patternedTextureSecondary.save(
                                patternedTextureSecondaryPath);
                        if (!patternedTextureObject.meshMaterials.isEmpty()) {
                            WorkbenchMaterialData &patternedMaterial =
                                patternedTextureObject.meshMaterials[0];
                            patternedMaterial.textureImagePath =
                                patternedTexturePath;
                            patternedMaterial.textureImageInterpolation =
                                QStringLiteral("Linear");
                        }
                        QImage patternedTextureDisplay;
                        QVector<float> patternedTextureSceneLinear;
                        const bool patternedTextureRendered =
                            renderCoordinatedScene(
                                textureColorShading,
                                QByteArrayLiteral(
                                    "color-source-match-texture-uv"),
                                &patternedTextureDisplay,
                                &patternedTextureObject, nullptr, nullptr,
                                QString{}, &patternedTextureSceneLinear);
                        const QString patternedTextureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture_uv.png"));
                        const QString patternedTextureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture_uv.pfm"));
                        QFile patternedTextureLinearFile(
                            patternedTextureLinearPath);
                        const bool patternedTextureLinearSaved =
                            patternedTextureRendered &&
                            patternedTextureSceneLinear.size() ==
                                textureExpectedValues &&
                            patternedTextureLinearFile.open(
                                QIODevice::WriteOnly) &&
                            patternedTextureLinearFile.write(
                                texturePfmHeader) ==
                                texturePfmHeader.size() &&
                            patternedTextureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    patternedTextureSceneLinear.constData()),
                                patternedTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                patternedTextureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool patternedTextureDisplaySaved =
                            patternedTextureRendered &&
                            patternedTextureDisplay.size() == QSize(640, 480) &&
                            patternedTextureDisplay.save(
                                patternedTextureDisplayPath);
                        lightingCapturesSucceeded &=
                            patternedTextureSaved &&
                            patternedTextureLinearSaved &&
                            patternedTextureDisplaySaved;

                        // Exercise Image Texture filtering through the
                        // production GPU path for a planar NURBS patch. Its
                        // normalized UVs must remain stable when the stored
                        // knot domains are shifted and scaled.
                        ViewportRenderObject nurbsTextureObject;
                        nurbsTextureObject.shape.geometryType =
                            GeometryType::NurbsSurface;
                        nurbsTextureObject.shape.nurbsSurface = fillSurface;
                        nurbsTextureObject.shape.nurbsSurface.knotsU =
                            {2.0, 6.0};
                        nurbsTextureObject.shape.nurbsSurface.knotsV =
                            {-3.0, 5.0};
                        nurbsTextureObject.preparedDepthGeometry =
                            QSharedPointer<ViewportDepthGeometry>::create(
                                buildViewportDepthGeometry(
                                    nurbsTextureObject.shape));
                        nurbsTextureObject.objectId =
                            ObjectId::fromValue(9101);
                        nurbsTextureObject.displayName =
                            QStringLiteral("NurbsTexturePatch");
                        nurbsTextureObject.geometryRevision = 1;
                        nurbsTextureObject.hasWorkbenchMaterial = true;
                        nurbsTextureObject.workbenchMaterial
                            .baseColorSceneLinear = QVector3D(0.8f, 0.8f, 0.8f);
                        nurbsTextureObject.workbenchMaterial.textureImagePath =
                            patternedTexturePath;
                        nurbsTextureObject.workbenchMaterial.textureImageSrgb =
                            true;
                        nurbsTextureObject.workbenchMaterial
                            .textureImageAlphaMode = QStringLiteral("STRAIGHT");
                        nurbsTextureObject.workbenchMaterial
                            .textureImageInterpolation = QStringLiteral("Linear");
                        nurbsTextureObject.workbenchMaterial
                            .textureImageExtension = QStringLiteral("REPEAT");
                        QImage nurbsTextureDisplay;
                        QVector<float> nurbsTextureSceneLinear;
                        const QVector3D textureBackground(0.0219421387f,
                                                         0.0219421387f,
                                                         0.0219421387f);
                        const bool nurbsTextureRendered =
                            validateNurbsSurface(
                                nurbsTextureObject.shape.nurbsSurface) &&
                            renderCoordinatedScene(
                                textureColorShading,
                                QByteArrayLiteral(
                                    "color-source-match-nurbs-texture"),
                                &nurbsTextureDisplay, &nurbsTextureObject,
                                nullptr, nullptr, QString{},
                                &nurbsTextureSceneLinear,
                                &textureBackground);
                        int nurbsTextureForegroundPixels = 0;
                        QVector3D nurbsTextureMinimum(
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max());
                        QVector3D nurbsTextureMaximum(
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max());
                        for (qsizetype pixel = 0;
                             pixel + 2 < nurbsTextureSceneLinear.size();
                             pixel += 3) {
                            const QVector3D color(
                                nurbsTextureSceneLinear[pixel],
                                nurbsTextureSceneLinear[pixel + 1],
                                nurbsTextureSceneLinear[pixel + 2]);
                            if ((color - textureBackground).lengthSquared() >
                                1.0e-5f) {
                                ++nurbsTextureForegroundPixels;
                                nurbsTextureMinimum.setX(std::min(
                                    nurbsTextureMinimum.x(), color.x()));
                                nurbsTextureMinimum.setY(std::min(
                                    nurbsTextureMinimum.y(), color.y()));
                                nurbsTextureMinimum.setZ(std::min(
                                    nurbsTextureMinimum.z(), color.z()));
                                nurbsTextureMaximum.setX(std::max(
                                    nurbsTextureMaximum.x(), color.x()));
                                nurbsTextureMaximum.setY(std::max(
                                    nurbsTextureMaximum.y(), color.y()));
                                nurbsTextureMaximum.setZ(std::max(
                                    nurbsTextureMaximum.z(), color.z()));
                            }
                        }
                        passed &= check(
                            nurbsTextureRendered &&
                                nurbsTextureForegroundPixels > 5000 &&
                                (nurbsTextureMaximum -
                                 nurbsTextureMinimum).length() > 0.25f,
                            "GPU NURBS Image Texture capture must contain the full nonblank, varying-color patch");
                        const QString nurbsTextureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture_nurbs.pfm"));
                        const QString nurbsTextureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture_nurbs.png"));
                        QFile nurbsTextureLinearFile(nurbsTextureLinearPath);
                        const bool nurbsTextureLinearSaved =
                            nurbsTextureRendered &&
                            nurbsTextureSceneLinear.size() ==
                                textureExpectedValues &&
                            nurbsTextureLinearFile.open(QIODevice::WriteOnly) &&
                            nurbsTextureLinearFile.write(texturePfmHeader) ==
                                texturePfmHeader.size() &&
                            nurbsTextureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    nurbsTextureSceneLinear.constData()),
                                nurbsTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                nurbsTextureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool nurbsTextureDisplaySaved =
                            nurbsTextureRendered &&
                            nurbsTextureDisplay.size() == QSize(640, 480) &&
                            nurbsTextureDisplay.save(nurbsTextureDisplayPath);
                        lightingCapturesSucceeded &=
                            nurbsTextureRendered && nurbsTextureLinearSaved &&
                            nurbsTextureDisplaySaved;

                        CurveHitTester qpainterTextureHitTester;
                        ViewportRenderer qpainterTextureRenderer(
                            gpuTransform, qpainterTextureHitTester);
                        qpainterTextureRenderer.setShadingSettings(
                            textureColorShading);
                        QImage qpainterNurbsTextureDisplay(
                            glSize, QImage::Format_ARGB32_Premultiplied);
                        qpainterNurbsTextureDisplay.fill(
                            nurbsTextureDisplay.pixelColor(0, 0));
                        RasterWorkbenchFragmentSink
                            qpainterNurbsTextureFragments;
                        QPainter qpainterTexturePainter(
                            &qpainterNurbsTextureDisplay);
                        qpainterTextureRenderer.drawShape(
                            qpainterTexturePainter,
                            nurbsTextureObject.shape, glSize, false, false,
                            true, QColor(), QString(), 0.0,
                            nurbsTextureObject.objectId,
                            nurbsTextureObject.geometryRevision,
                            nurbsTextureObject.preparedDepthGeometry.data(),
                            {}, std::nullopt,
                            nurbsTextureObject.workbenchMaterial,
                            nurbsTextureObject.displayName, QString(), {}, {},
                            &qpainterNurbsTextureFragments);
                        qpainterTexturePainter.end();
                        int qpainterNurbsTextureForegroundPixels = 0;
                        for (int y = 0; y < glSize.height(); ++y) {
                            for (int x = 0; x < glSize.width(); ++x) {
                                if (QColor::fromRgba(
                                        qpainterNurbsTextureDisplay.pixel(x, y)) !=
                                    nurbsTextureDisplay.pixelColor(0, 0)) {
                                    ++qpainterNurbsTextureForegroundPixels;
                                }
                            }
                        }
                        const QString qpainterNurbsTextureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_nurbs_qpainter.png"));
                        QVector<float> qpainterNurbsTextureSceneLinear(
                            textureExpectedValues);
                        for (qsizetype pixel = 0;
                             pixel + 2 < qpainterNurbsTextureSceneLinear.size();
                             pixel += 3) {
                            qpainterNurbsTextureSceneLinear[pixel] =
                                textureBackground.x();
                            qpainterNurbsTextureSceneLinear[pixel + 1] =
                                textureBackground.y();
                            qpainterNurbsTextureSceneLinear[pixel + 2] =
                                textureBackground.z();
                        }
                        for (const RasterWorkbenchLinearFragment &fragment :
                             qpainterNurbsTextureFragments) {
                            if (fragment.x < 0 || fragment.x >= glSize.width() ||
                                fragment.y < 0 || fragment.y >= glSize.height()) {
                                continue;
                            }
                            const qsizetype pixel =
                                (qsizetype(fragment.y) * glSize.width() +
                                 fragment.x) * 3;
                            qpainterNurbsTextureSceneLinear[pixel] =
                                fragment.sceneLinearRgb[0];
                            qpainterNurbsTextureSceneLinear[pixel + 1] =
                                fragment.sceneLinearRgb[1];
                            qpainterNurbsTextureSceneLinear[pixel + 2] =
                                fragment.sceneLinearRgb[2];
                        }
                        const QString qpainterNurbsTextureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_nurbs_qpainter.pfm"));
                        QFile qpainterNurbsTextureLinearFile(
                            qpainterNurbsTextureLinearPath);
                        const bool qpainterNurbsTextureLinearSaved =
                            qpainterNurbsTextureFragments.size() > 5000 &&
                            qpainterNurbsTextureLinearFile.open(
                                QIODevice::WriteOnly) &&
                            qpainterNurbsTextureLinearFile.write(
                                texturePfmHeader) == texturePfmHeader.size() &&
                            qpainterNurbsTextureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    qpainterNurbsTextureSceneLinear.constData()),
                                qpainterNurbsTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                qpainterNurbsTextureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool qpainterNurbsTextureSaved =
                            nurbsTextureRendered &&
                            qpainterNurbsTextureForegroundPixels > 5000 &&
                            qpainterNurbsTextureDisplay.size() == glSize &&
                            qpainterNurbsTextureLinearSaved &&
                            qpainterNurbsTextureDisplay.save(
                                qpainterNurbsTextureDisplayPath);
                        passed &= check(
                            qpainterNurbsTextureSaved,
                            "QPainter NURBS Image Texture capture must render a nonblank full-frame patch");
                        lightingCapturesSucceeded &=
                            qpainterNurbsTextureSaved;

                        ViewportRenderObject qpainterMeshTextureObject;
                        qpainterMeshTextureObject.shape.geometryType =
                            GeometryType::PolygonMesh;
                        PolygonMesh3D &qpainterMeshTextureMesh =
                            qpainterMeshTextureObject.shape.polygonMesh;
                        qpainterMeshTextureMesh.vertices = {
                            {-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0},
                            {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
                        qpainterMeshTextureMesh.faces = {{0, 1, 2}, {0, 2, 3}};
                        qpainterMeshTextureMesh.faceMaterialIndices = {0, 1};
                        qpainterMeshTextureMesh.materialSlotIds = {
                            QStringLiteral("TextureSlotA"),
                            QStringLiteral("TextureSlotB")};
                        qpainterMeshTextureMesh.uvMaps = {
                            {QStringLiteral("UVMap"),
                             {QPointF(0.0, 0.0), QPointF(1.0, 0.0),
                              QPointF(1.0, 1.0), QPointF(0.0, 0.0),
                              QPointF(1.0, 1.0), QPointF(0.0, 1.0)}}};
                        qpainterMeshTextureMesh.activeUvMapName =
                            QStringLiteral("UVMap");
                        qpainterMeshTextureMesh.defaultUvMapName =
                            QStringLiteral("UVMap");
                        WorkbenchMaterialData qpainterMeshTextureMaterial;
                        qpainterMeshTextureMaterial.baseColorSceneLinear =
                            QVector3D(0.8f, 0.8f, 0.8f);
                        qpainterMeshTextureMaterial.textureImagePath =
                            patternedTexturePath;
                        qpainterMeshTextureMaterial.textureImageSrgb = true;
                        qpainterMeshTextureMaterial.textureImageAlphaMode =
                            QStringLiteral("STRAIGHT");
                        qpainterMeshTextureMaterial
                            .textureImageInterpolation =
                                QStringLiteral("Linear");
                        qpainterMeshTextureMaterial.textureImageExtension =
                            QStringLiteral("REPEAT");
                        WorkbenchMaterialData qpainterMeshTextureSecondaryMaterial;
                        qpainterMeshTextureSecondaryMaterial.baseColorSceneLinear =
                            QVector3D(0.8f, 0.8f, 0.8f);
                        qpainterMeshTextureSecondaryMaterial.textureImagePath =
                            patternedTextureSecondaryPath;
                        qpainterMeshTextureSecondaryMaterial.textureImageSrgb =
                            true;
                        qpainterMeshTextureSecondaryMaterial
                            .textureImageAlphaMode = QStringLiteral("STRAIGHT");
                        qpainterMeshTextureSecondaryMaterial
                            .textureImageInterpolation =
                                QStringLiteral("Linear");
                        qpainterMeshTextureSecondaryMaterial
                            .textureImageExtension = QStringLiteral("REPEAT");
                        const QVector<WorkbenchMaterialData>
                            qpainterMeshTextureMaterials{
                                qpainterMeshTextureMaterial,
                                qpainterMeshTextureSecondaryMaterial};
                        qpainterMeshTextureObject.meshMaterials =
                            qpainterMeshTextureMaterials;
                        qpainterMeshTextureObject.objectId =
                            ObjectId::fromValue(9102);
                        qpainterMeshTextureObject.displayName =
                            QStringLiteral("MeshTexturePatch");
                        qpainterMeshTextureObject.geometryRevision = 1;
                        QImage qpainterMeshTextureDisplay(
                            glSize, QImage::Format_ARGB32_Premultiplied);
                        qpainterMeshTextureDisplay.fill(
                            nurbsTextureDisplay.pixelColor(0, 0));
                        RasterWorkbenchFragmentSink
                            qpainterMeshTextureFragments;
                        QPainter qpainterMeshTexturePainter(
                            &qpainterMeshTextureDisplay);
                        qpainterTextureRenderer.drawShape(
                            qpainterMeshTexturePainter,
                            qpainterMeshTextureObject.shape, glSize, false,
                            false, true, QColor(), QString(), 0.0,
                            qpainterMeshTextureObject.objectId,
                            qpainterMeshTextureObject.geometryRevision,
                            nullptr, {}, std::nullopt, std::nullopt,
                            qpainterMeshTextureObject.displayName, QString(),
                            identityMeshObjectTransform,
                            qpainterMeshTextureMaterials,
                            &qpainterMeshTextureFragments);
                        qpainterMeshTexturePainter.end();
                        QVector<float> qpainterMeshTextureSceneLinear(
                            textureExpectedValues);
                        for (qsizetype pixel = 0;
                             pixel + 2 < qpainterMeshTextureSceneLinear.size();
                             pixel += 3) {
                            qpainterMeshTextureSceneLinear[pixel] =
                                textureBackground.x();
                            qpainterMeshTextureSceneLinear[pixel + 1] =
                                textureBackground.y();
                            qpainterMeshTextureSceneLinear[pixel + 2] =
                                textureBackground.z();
                        }
                        for (const RasterWorkbenchLinearFragment &fragment :
                             qpainterMeshTextureFragments) {
                            if (fragment.x < 0 || fragment.x >= glSize.width() ||
                                fragment.y < 0 || fragment.y >= glSize.height()) {
                                continue;
                            }
                            const qsizetype pixel =
                                (qsizetype(fragment.y) * glSize.width() +
                                 fragment.x) * 3;
                            qpainterMeshTextureSceneLinear[pixel] =
                                fragment.sceneLinearRgb[0];
                            qpainterMeshTextureSceneLinear[pixel + 1] =
                                fragment.sceneLinearRgb[1];
                            qpainterMeshTextureSceneLinear[pixel + 2] =
                                fragment.sceneLinearRgb[2];
                        }
                        const QString qpainterMeshTextureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_mesh_qpainter.png"));
                        const QString qpainterMeshTextureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_mesh_qpainter.pfm"));
                        QFile qpainterMeshTextureLinearFile(
                            qpainterMeshTextureLinearPath);
                        const bool qpainterMeshTextureLinearSaved =
                            qpainterMeshTextureFragments.size() > 5000 &&
                            qpainterMeshTextureLinearFile.open(
                                QIODevice::WriteOnly) &&
                            qpainterMeshTextureLinearFile.write(
                                texturePfmHeader) == texturePfmHeader.size() &&
                            qpainterMeshTextureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    qpainterMeshTextureSceneLinear.constData()),
                                qpainterMeshTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                            qpainterMeshTextureSceneLinear.size() *
                                qsizetype(sizeof(float));
                        const bool qpainterMeshTextureSaved =
                            patternedTextureSaved &&
                            patternedTextureSecondarySaved &&
                            validatePolygonMesh(qpainterMeshTextureMesh) &&
                            qpainterMeshTextureLinearSaved &&
                            qpainterMeshTextureDisplay.save(
                                qpainterMeshTextureDisplayPath);
                        passed &= check(
                            qpainterMeshTextureSaved,
                            "QPainter polygon-mesh Image Texture capture must render the matched two-triangle patch");
                        lightingCapturesSucceeded &=
                            qpainterMeshTextureSaved;

                        const QString meshAlphaStraightPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "workbench-texture-alpha-straight.png"));
                        const QString meshAlphaPremultipliedPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "workbench-texture-alpha-premultiplied.png"));
                        QImage meshAlphaStraight(2, 2,
                                                 QImage::Format_RGBA8888);
                        const std::array<QColor, 4> meshAlphaColors{
                            QColor(244, 24, 16, 0),
                            QColor(20, 224, 40, 13),
                            QColor(32, 56, 240, 26),
                            QColor(240, 200, 24, 128)};
                        for (int index = 0; index < 4; ++index) {
                            meshAlphaStraight.setPixelColor(index % 2,
                                                           index / 2,
                                                           meshAlphaColors[index]);
                        }
                        QImage meshAlphaPremultiplied(
                            2, 2, QImage::Format_RGBA8888);
                        for (int index = 0; index < 4; ++index) {
                            const QColor source = meshAlphaColors[index];
                            const qreal alpha = source.alphaF();
                            meshAlphaPremultiplied.setPixelColor(
                                index % 2, index / 2,
                                QColor(qRound(source.red() * alpha),
                                       qRound(source.green() * alpha),
                                       qRound(source.blue() * alpha),
                                       source.alpha()));
                        }
                        const bool meshAlphaTexturesSaved =
                            meshAlphaStraight.save(meshAlphaStraightPath) &&
                            meshAlphaPremultiplied.save(
                                meshAlphaPremultipliedPath);
                        const std::array<QString, 4> meshAlphaModes{
                            QStringLiteral("STRAIGHT"),
                            QStringLiteral("PREMUL"),
                            QStringLiteral("CHANNEL_PACKED"),
                            QStringLiteral("NONE")};
                        for (int modeIndex = 0;
                             modeIndex < int(meshAlphaModes.size());
                             ++modeIndex) {
                            const QString &alphaMode =
                                meshAlphaModes[modeIndex];
                            ViewportRenderObject alphaMeshObject;
                            alphaMeshObject.shape.geometryType =
                                GeometryType::PolygonMesh;
                            PolygonMesh3D &alphaMesh =
                                alphaMeshObject.shape.polygonMesh;
                            alphaMesh.vertices = {
                                {-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0},
                                {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
                            alphaMesh.faces = {{0, 1, 2}, {0, 2, 3}};
                            alphaMesh.faceMaterialIndices = {0, 0};
                            alphaMesh.materialSlotIds = {
                                QStringLiteral("AlphaTextureSlot")};
                            alphaMesh.uvMaps = {
                                {QStringLiteral("UVMap"),
                                 {QPointF(0.0, 0.0), QPointF(1.0, 0.0),
                                  QPointF(1.0, 1.0), QPointF(0.0, 0.0),
                                  QPointF(1.0, 1.0), QPointF(0.0, 1.0)}}};
                            alphaMesh.activeUvMapName =
                                QStringLiteral("UVMap");
                            alphaMesh.defaultUvMapName =
                                QStringLiteral("UVMap");
                            WorkbenchMaterialData alphaMeshMaterial;
                            alphaMeshMaterial.baseColorSceneLinear =
                                QVector3D(0.8f, 0.8f, 0.8f);
                            alphaMeshMaterial.textureImagePath =
                                alphaMode == QStringLiteral("PREMUL")
                                    ? meshAlphaPremultipliedPath
                                    : meshAlphaStraightPath;
                            alphaMeshMaterial.textureImageSrgb = true;
                            alphaMeshMaterial.textureImageAlphaMode =
                                alphaMode;
                            alphaMeshMaterial.textureImageInterpolation =
                                QStringLiteral("Linear");
                            alphaMeshMaterial.textureImageExtension =
                                QStringLiteral("REPEAT");
                            const QVector<WorkbenchMaterialData> alphaMaterials{
                                alphaMeshMaterial};
                            alphaMeshObject.meshMaterials = alphaMaterials;
                            alphaMeshObject.objectId =
                                ObjectId::fromValue(9110 + modeIndex);
                            alphaMeshObject.displayName =
                                QStringLiteral("MeshAlphaTexturePatch");
                            alphaMeshObject.geometryRevision = 1;
                            QImage alphaMeshDisplay(
                                glSize, QImage::Format_ARGB32_Premultiplied);
                            alphaMeshDisplay.fill(
                                nurbsTextureDisplay.pixelColor(0, 0));
                            RasterWorkbenchFragmentSink alphaMeshFragments;
                            QPainter alphaMeshPainter(&alphaMeshDisplay);
                            qpainterTextureRenderer.drawShape(
                                alphaMeshPainter, alphaMeshObject.shape,
                                glSize, false, false, true, QColor(),
                                QString(), 0.0, alphaMeshObject.objectId,
                                alphaMeshObject.geometryRevision, nullptr, {},
                                std::nullopt, std::nullopt,
                                alphaMeshObject.displayName, QString(),
                                identityMeshObjectTransform, alphaMaterials,
                                &alphaMeshFragments);
                            alphaMeshPainter.end();
                            const QString modeFileName = alphaMode.toLower();
                            const QString alphaMeshDisplayPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    QStringLiteral("color_texture_mesh_alpha_") +
                                    modeFileName + QStringLiteral(".png"));
                            QVector<float> alphaMeshSceneLinear(
                                textureExpectedValues);
                            for (qsizetype pixel = 0;
                                 pixel + 2 < alphaMeshSceneLinear.size();
                                 pixel += 3) {
                                alphaMeshSceneLinear[pixel] =
                                    textureBackground.x();
                                alphaMeshSceneLinear[pixel + 1] =
                                    textureBackground.y();
                                alphaMeshSceneLinear[pixel + 2] =
                                    textureBackground.z();
                            }
                            for (const RasterWorkbenchLinearFragment &fragment :
                                 alphaMeshFragments) {
                                if (fragment.x < 0 ||
                                    fragment.x >= glSize.width() ||
                                    fragment.y < 0 ||
                                    fragment.y >= glSize.height()) {
                                    continue;
                                }
                                const qsizetype pixel =
                                    (qsizetype(fragment.y) * glSize.width() +
                                     fragment.x) * 3;
                                alphaMeshSceneLinear[pixel] =
                                    fragment.sceneLinearRgb[0];
                                alphaMeshSceneLinear[pixel + 1] =
                                    fragment.sceneLinearRgb[1];
                                alphaMeshSceneLinear[pixel + 2] =
                                    fragment.sceneLinearRgb[2];
                            }
                            QFile alphaMeshLinearFile(QDir(
                                lightingCaptureDirectory).filePath(
                                    QStringLiteral("color_texture_mesh_alpha_") +
                                    modeFileName + QStringLiteral(".pfm")));
                            const bool alphaMeshLinearSaved =
                                alphaMeshLinearFile.open(QIODevice::WriteOnly) &&
                                alphaMeshLinearFile.write(texturePfmHeader) ==
                                    texturePfmHeader.size() &&
                                alphaMeshLinearFile.write(
                                    reinterpret_cast<const char *>(
                                        alphaMeshSceneLinear.constData()),
                                    alphaMeshSceneLinear.size() *
                                        qsizetype(sizeof(float))) ==
                                    alphaMeshSceneLinear.size() *
                                        qsizetype(sizeof(float));
                            const bool alphaMeshCaptureSaved =
                                meshAlphaTexturesSaved &&
                                validatePolygonMesh(alphaMesh) &&
                                alphaMeshFragments.size() > 1000 &&
                                alphaMeshLinearSaved &&
                                alphaMeshDisplay.save(alphaMeshDisplayPath);
                            passed &= check(
                                alphaMeshCaptureSaved,
                                "QPainter mesh texture alpha-mode capture must render a nonblank cutoff fixture");
                            lightingCapturesSucceeded &=
                                alphaMeshCaptureSaved;
                        }

                        const QString udimTile1001Path = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("workbench-udim.1001.png"));
                        const QString udimTile1002Path = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("workbench-udim.1002.png"));
                        QImage udimTile1001(2, 2, QImage::Format_RGBA8888);
                        udimTile1001.fill(QColor(240, 48, 24, 255));
                        QImage udimTile1002(2, 2, QImage::Format_RGBA8888);
                        udimTile1002.fill(QColor(24, 216, 72, 255));
                        udimTile1002.setPixelColor(0, 0,
                                                  QColor(24, 216, 72, 0));
                        const bool udimTilesSaved =
                            udimTile1001.save(udimTile1001Path) &&
                            udimTile1002.save(udimTile1002Path);
                        ViewportRenderObject udimMeshTextureObject;
                        udimMeshTextureObject.shape.geometryType =
                            GeometryType::PolygonMesh;
                        PolygonMesh3D &udimMesh =
                            udimMeshTextureObject.shape.polygonMesh;
                        udimMesh.vertices = {
                            {-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0},
                            {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
                        udimMesh.faces = {{0, 1, 2}, {0, 2, 3}};
                        udimMesh.faceMaterialIndices = {0, 0};
                        udimMesh.materialSlotIds = {
                            QStringLiteral("UdimTextureSlot")};
                        udimMesh.uvMaps = {
                            {QStringLiteral("UVMap"),
                             {QPointF(0.0, 0.05), QPointF(2.0, 0.05),
                              QPointF(2.0, 0.95), QPointF(0.0, 0.05),
                              QPointF(2.0, 0.95), QPointF(0.0, 0.95)}}};
                        udimMesh.activeUvMapName = QStringLiteral("UVMap");
                        udimMesh.defaultUvMapName = QStringLiteral("UVMap");
                        WorkbenchMaterialData udimMaterial;
                        udimMaterial.baseColorSceneLinear =
                            QVector3D(0.8f, 0.8f, 0.8f);
                        udimMaterial.textureImageSrgb = true;
                        udimMaterial.textureImageAlphaMode =
                            QStringLiteral("STRAIGHT");
                        udimMaterial.textureImageInterpolation =
                            QStringLiteral("Linear");
                        udimMaterial.textureImageExtension =
                            QStringLiteral("REPEAT");
                        DocumentImageTextureTile udimTileRecord1001;
                        udimTileRecord1001.number = 1001;
                        udimTileRecord1001.filepath = udimTile1001Path;
                        udimTileRecord1001.sourceFileExists = true;
                        DocumentImageTextureTile udimTileRecord1002;
                        udimTileRecord1002.number = 1002;
                        udimTileRecord1002.filepath = udimTile1002Path;
                        udimTileRecord1002.sourceFileExists = true;
                        udimMaterial.textureImageTiles = {
                            udimTileRecord1001, udimTileRecord1002};
                        const QVector<WorkbenchMaterialData> udimMaterials{
                            udimMaterial};
                        udimMeshTextureObject.meshMaterials = udimMaterials;
                        udimMeshTextureObject.objectId =
                            ObjectId::fromValue(9130);
                        udimMeshTextureObject.displayName =
                            QStringLiteral("UdimMeshTexturePatch");
                        udimMeshTextureObject.geometryRevision = 1;
                        QImage udimMeshTextureDisplay(
                            glSize, QImage::Format_ARGB32_Premultiplied);
                        udimMeshTextureDisplay.fill(
                            nurbsTextureDisplay.pixelColor(0, 0));
                        RasterWorkbenchFragmentSink udimMeshTextureFragments;
                        QPainter udimMeshTexturePainter(
                            &udimMeshTextureDisplay);
                        qpainterTextureRenderer.drawShape(
                            udimMeshTexturePainter,
                            udimMeshTextureObject.shape, glSize, false,
                            false, true, QColor(), QString(), 0.0,
                            udimMeshTextureObject.objectId,
                            udimMeshTextureObject.geometryRevision,
                            nullptr, {}, std::nullopt, std::nullopt,
                            udimMeshTextureObject.displayName, QString(),
                            identityMeshObjectTransform, udimMaterials,
                            &udimMeshTextureFragments);
                        udimMeshTexturePainter.end();
                        QVector<float> udimMeshTextureSceneLinear(
                            textureExpectedValues);
                        for (qsizetype pixel = 0;
                             pixel + 2 < udimMeshTextureSceneLinear.size();
                             pixel += 3) {
                            udimMeshTextureSceneLinear[pixel] =
                                textureBackground.x();
                            udimMeshTextureSceneLinear[pixel + 1] =
                                textureBackground.y();
                            udimMeshTextureSceneLinear[pixel + 2] =
                                textureBackground.z();
                        }
                        int udimTile1001Samples = 0;
                        int udimTile1002Samples = 0;
                        for (const RasterWorkbenchLinearFragment &fragment :
                             udimMeshTextureFragments) {
                            if (fragment.x < 0 ||
                                fragment.x >= glSize.width() ||
                                fragment.y < 0 ||
                                fragment.y >= glSize.height()) {
                                continue;
                            }
                            const qsizetype pixel =
                                (qsizetype(fragment.y) * glSize.width() +
                                 fragment.x) * 3;
                            udimMeshTextureSceneLinear[pixel] =
                                fragment.sceneLinearRgb[0];
                            udimMeshTextureSceneLinear[pixel + 1] =
                                fragment.sceneLinearRgb[1];
                            udimMeshTextureSceneLinear[pixel + 2] =
                                fragment.sceneLinearRgb[2];
                            if (fragment.sceneLinearRgb[0] >
                                fragment.sceneLinearRgb[1] * 2.0f) {
                                ++udimTile1001Samples;
                            }
                            if (fragment.sceneLinearRgb[1] >
                                fragment.sceneLinearRgb[0] * 2.0f) {
                                ++udimTile1002Samples;
                            }
                        }
                        const QString udimDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_mesh_udim_qpainter.png"));
                        const QString udimLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_mesh_udim_qpainter.pfm"));
                        QFile udimLinearFile(udimLinearPath);
                        const bool udimLinearSaved =
                            udimLinearFile.open(QIODevice::WriteOnly) &&
                            udimLinearFile.write(texturePfmHeader) ==
                                texturePfmHeader.size() &&
                            udimLinearFile.write(
                                reinterpret_cast<const char *>(
                                    udimMeshTextureSceneLinear.constData()),
                                udimMeshTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                udimMeshTextureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool udimMeshTextureSaved = udimTilesSaved &&
                            validatePolygonMesh(udimMesh) &&
                            udimTile1001Samples > 1000 &&
                            udimTile1002Samples > 1000 &&
                            udimMeshTextureFragments.size() > 25000 &&
                            udimMeshTextureFragments.size() < 40000 &&
                            udimLinearSaved &&
                            udimMeshTextureDisplay.save(udimDisplayPath);
                        passed &= check(
                            udimMeshTextureSaved,
                            "QPainter mesh UDIM capture must render both tiles and the transparent cutout");
                        lightingCapturesSucceeded &= udimMeshTextureSaved;

                        ViewportTransform perspectiveTextureTransform;
                        perspectiveTextureTransform.zoom() = 17.28;
                        perspectiveTextureTransform.setViewDirection(
                            {4.0, -6.0, 4.0});
                        perspectiveTextureTransform.setPerspectiveEnabled(true);
                        CurveHitTester perspectiveTextureHitTester;
                        ViewportRenderer perspectiveTextureRenderer(
                            perspectiveTextureTransform,
                            perspectiveTextureHitTester);
                        perspectiveTextureRenderer.setShadingSettings(
                            textureColorShading);
                        ViewportRenderObject perspectiveMeshTextureObject =
                            qpainterMeshTextureObject;
                        perspectiveMeshTextureObject.shape.polygonMesh
                            .faceMaterialIndices = {0, 0};
                        perspectiveMeshTextureObject.shape.polygonMesh
                            .materialSlotIds = {
                                QStringLiteral("TextureSlotA")};
                        const QVector<WorkbenchMaterialData>
                            perspectiveMeshTextureMaterials{
                                qpainterMeshTextureMaterial};
                        perspectiveMeshTextureObject.meshMaterials =
                            perspectiveMeshTextureMaterials;
                        perspectiveMeshTextureObject.objectId =
                            ObjectId::fromValue(9120);
                        perspectiveMeshTextureObject.displayName =
                            QStringLiteral("PerspectiveMeshTexturePatch");
                        QImage perspectiveMeshTextureDisplay(
                            glSize, QImage::Format_ARGB32_Premultiplied);
                        perspectiveMeshTextureDisplay.fill(
                            nurbsTextureDisplay.pixelColor(0, 0));
                        RasterWorkbenchFragmentSink
                            perspectiveMeshTextureFragments;
                        QPainter perspectiveMeshTexturePainter(
                            &perspectiveMeshTextureDisplay);
                        perspectiveTextureRenderer.drawShape(
                            perspectiveMeshTexturePainter,
                            perspectiveMeshTextureObject.shape, glSize, false,
                            false, true, QColor(), QString(), 0.0,
                            perspectiveMeshTextureObject.objectId,
                            perspectiveMeshTextureObject.geometryRevision,
                            nullptr, {}, std::nullopt, std::nullopt,
                            perspectiveMeshTextureObject.displayName, QString(),
                            identityMeshObjectTransform,
                            perspectiveMeshTextureMaterials,
                            &perspectiveMeshTextureFragments);
                        perspectiveMeshTexturePainter.end();
                        QVector<float> perspectiveMeshTextureSceneLinear(
                            textureExpectedValues);
                        for (qsizetype pixel = 0;
                             pixel + 2 <
                                 perspectiveMeshTextureSceneLinear.size();
                             pixel += 3) {
                            perspectiveMeshTextureSceneLinear[pixel] =
                                textureBackground.x();
                            perspectiveMeshTextureSceneLinear[pixel + 1] =
                                textureBackground.y();
                            perspectiveMeshTextureSceneLinear[pixel + 2] =
                                textureBackground.z();
                        }
                        for (const RasterWorkbenchLinearFragment &fragment :
                             perspectiveMeshTextureFragments) {
                            if (fragment.x < 0 ||
                                fragment.x >= glSize.width() ||
                                fragment.y < 0 ||
                                fragment.y >= glSize.height()) {
                                continue;
                            }
                            const qsizetype pixel =
                                (qsizetype(fragment.y) * glSize.width() +
                                 fragment.x) * 3;
                            perspectiveMeshTextureSceneLinear[pixel] =
                                fragment.sceneLinearRgb[0];
                            perspectiveMeshTextureSceneLinear[pixel + 1] =
                                fragment.sceneLinearRgb[1];
                            perspectiveMeshTextureSceneLinear[pixel + 2] =
                                fragment.sceneLinearRgb[2];
                        }
                        const QString perspectiveMeshTextureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_mesh_perspective_qpainter.png"));
                        const QString perspectiveMeshTextureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_mesh_perspective_qpainter.pfm"));
                        QFile perspectiveMeshTextureLinearFile(
                            perspectiveMeshTextureLinearPath);
                        const bool perspectiveMeshTextureLinearSaved =
                            perspectiveMeshTextureLinearFile.open(
                                QIODevice::WriteOnly) &&
                            perspectiveMeshTextureLinearFile.write(
                                texturePfmHeader) ==
                                texturePfmHeader.size() &&
                            perspectiveMeshTextureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    perspectiveMeshTextureSceneLinear.constData()),
                                perspectiveMeshTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                perspectiveMeshTextureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool perspectiveMeshTextureSaved =
                            patternedTextureSaved &&
                            perspectiveTextureTransform
                                .isPerspectiveEnabled() &&
                            validatePolygonMesh(
                                perspectiveMeshTextureObject.shape.polygonMesh) &&
                            perspectiveMeshTextureFragments.size() > 5000 &&
                            perspectiveMeshTextureLinearSaved &&
                            perspectiveMeshTextureDisplay.save(
                                perspectiveMeshTextureDisplayPath);
                        passed &= check(
                            perspectiveMeshTextureSaved,
                            "QPainter perspective mesh texture capture must render the matched varying-UV patch");
                        lightingCapturesSucceeded &=
                            perspectiveMeshTextureSaved;

                        QImage alphaCutoutTexture(2, 2,
                                                  QImage::Format_RGBA8888);
                        alphaCutoutTexture.setPixelColor(
                            0, 0, QColor(240, 32, 16, 0));
                        alphaCutoutTexture.setPixelColor(
                            1, 0, QColor(24, 224, 48, 255));
                        alphaCutoutTexture.setPixelColor(
                            0, 1, QColor(32, 64, 240, 0));
                        alphaCutoutTexture.setPixelColor(
                            1, 1, QColor(240, 208, 24, 255));
                        const QString alphaCutoutTexturePath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("workbench-texture-alpha.png"));
                        const bool alphaCutoutTextureSaved =
                            alphaCutoutTexture.save(alphaCutoutTexturePath);
                        nurbsTextureObject.workbenchMaterial.textureImagePath =
                            alphaCutoutTexturePath;
                        nurbsTextureObject.workbenchMaterial
                            .textureImageAlphaMode = QStringLiteral("STRAIGHT");
                        QImage nurbsTextureAlphaDisplay;
                        QVector<float> nurbsTextureAlphaSceneLinear;
                        const bool nurbsTextureAlphaRendered =
                            alphaCutoutTextureSaved && renderCoordinatedScene(
                                textureColorShading,
                                QByteArrayLiteral(
                                    "color-source-match-nurbs-texture-alpha"),
                                &nurbsTextureAlphaDisplay,
                                &nurbsTextureObject, nullptr, nullptr,
                                QString{}, &nurbsTextureAlphaSceneLinear,
                                &textureBackground);
                        int nurbsTextureAlphaForegroundPixels = 0;
                        for (qsizetype pixel = 0;
                             pixel + 2 < nurbsTextureAlphaSceneLinear.size();
                             pixel += 3) {
                            const QVector3D color(
                                nurbsTextureAlphaSceneLinear[pixel],
                                nurbsTextureAlphaSceneLinear[pixel + 1],
                                nurbsTextureAlphaSceneLinear[pixel + 2]);
                            if ((color - textureBackground).lengthSquared() >
                                1.0e-5f) {
                                ++nurbsTextureAlphaForegroundPixels;
                            }
                        }
                        passed &= check(
                            nurbsTextureAlphaRendered &&
                                nurbsTextureAlphaForegroundPixels > 5000 &&
                                nurbsTextureAlphaForegroundPixels <
                                    nurbsTextureForegroundPixels,
                            "GPU NURBS Image Texture alpha cutoff must retain both transparent and opaque UV regions");
                        const QString nurbsTextureAlphaLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_nurbs_alpha.pfm"));
                        const QString nurbsTextureAlphaDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral(
                                    "color_texture_nurbs_alpha.png"));
                        QFile nurbsTextureAlphaLinearFile(
                            nurbsTextureAlphaLinearPath);
                        const bool nurbsTextureAlphaLinearSaved =
                            nurbsTextureAlphaRendered &&
                            nurbsTextureAlphaSceneLinear.size() ==
                                textureExpectedValues &&
                            nurbsTextureAlphaLinearFile.open(
                                QIODevice::WriteOnly) &&
                            nurbsTextureAlphaLinearFile.write(
                                texturePfmHeader) == texturePfmHeader.size() &&
                            nurbsTextureAlphaLinearFile.write(
                                reinterpret_cast<const char *>(
                                    nurbsTextureAlphaSceneLinear.constData()),
                                nurbsTextureAlphaSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                nurbsTextureAlphaSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool nurbsTextureAlphaDisplaySaved =
                            nurbsTextureAlphaRendered &&
                            nurbsTextureAlphaDisplay.size() == QSize(640, 480) &&
                            nurbsTextureAlphaDisplay.save(
                                nurbsTextureAlphaDisplayPath);
                        lightingCapturesSucceeded &=
                            nurbsTextureAlphaLinearSaved &&
                            nurbsTextureAlphaDisplaySaved;

                        const auto captureNurbsAlphaVariant =
                            [&](const QImage &image, const QString &mode,
                                const QString &stem,
                                QVector<float> *linearOutput,
                                bool expectCutout) {
                                const QString imagePath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        stem + QStringLiteral("-source.png"));
                                if (!image.save(imagePath)) return false;
                                nurbsTextureObject.workbenchMaterial
                                    .textureImagePath = imagePath;
                                nurbsTextureObject.workbenchMaterial
                                    .textureImageAlphaMode = mode;
                                QImage display;
                                QVector<float> linear;
                                const bool rendered = renderCoordinatedScene(
                                    textureColorShading,
                                    QByteArrayLiteral(
                                        "color-source-nurbs-alpha-") +
                                        stem.toUtf8(),
                                    &display, &nurbsTextureObject, nullptr,
                                    nullptr, QString{}, &linear,
                                    &textureBackground);
                                int foregroundPixels = 0;
                                for (qsizetype pixel = 0;
                                     pixel + 2 < linear.size(); pixel += 3) {
                                    const QVector3D color(
                                        linear[pixel], linear[pixel + 1],
                                        linear[pixel + 2]);
                                    if ((color - textureBackground)
                                            .lengthSquared() > 1.0e-5f) {
                                        ++foregroundPixels;
                                    }
                                }
                                const bool coverageValid = rendered &&
                                    foregroundPixels > 5000 &&
                                    (expectCutout
                                         ? foregroundPixels <
                                               nurbsTextureForegroundPixels
                                         : std::abs(foregroundPixels -
                                               nurbsTextureForegroundPixels) <
                                               100);
                                if (!coverageValid) return false;
                                const QString linearPath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        stem + QStringLiteral(".pfm"));
                                const QString displayPath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        stem + QStringLiteral(".png"));
                                QFile linearFile(linearPath);
                                const bool linearSaved =
                                    linear.size() == textureExpectedValues &&
                                    linearFile.open(QIODevice::WriteOnly) &&
                                    linearFile.write(texturePfmHeader) ==
                                        texturePfmHeader.size() &&
                                    linearFile.write(
                                        reinterpret_cast<const char *>(
                                            linear.constData()),
                                        linear.size() *
                                            qsizetype(sizeof(float))) ==
                                        linear.size() *
                                            qsizetype(sizeof(float));
                                if (linearOutput != nullptr) {
                                    *linearOutput = std::move(linear);
                                }
                                return linearSaved &&
                                    display.size() == QSize(640, 480) &&
                                    display.save(displayPath);
                            };
                        QVector<float> packedAlphaSceneLinear;
                        QVector<float> ignoredAlphaSceneLinear;
                        const bool packedAlphaCaptured =
                            captureNurbsAlphaVariant(
                                alphaCutoutTexture,
                                QStringLiteral("CHANNEL_PACKED"),
                                QStringLiteral("color_texture_nurbs_channel_packed"),
                                &packedAlphaSceneLinear, false);
                        const bool ignoredAlphaCaptured =
                            captureNurbsAlphaVariant(
                                alphaCutoutTexture,
                                QStringLiteral("IGNORE"),
                                QStringLiteral("color_texture_nurbs_ignore"),
                                &ignoredAlphaSceneLinear, false);
                        bool ignoredAndPackedMatch =
                            packedAlphaSceneLinear.size() ==
                            ignoredAlphaSceneLinear.size();
                        for (qsizetype index = 0;
                             ignoredAndPackedMatch &&
                             index < packedAlphaSceneLinear.size(); ++index) {
                            ignoredAndPackedMatch &=
                                std::abs(packedAlphaSceneLinear[index] -
                                         ignoredAlphaSceneLinear[index]) <
                                1.0e-6f;
                        }
                        QImage premultipliedTexture(
                            1, 1, QImage::Format_RGBA8888);
                        premultipliedTexture.setPixelColor(
                            0, 0, QColor(100, 40, 20, 128));
                        QVector<float> premultipliedAlphaSceneLinear;
                        const bool premultipliedAlphaCaptured =
                            captureNurbsAlphaVariant(
                                premultipliedTexture,
                                QStringLiteral("PREMUL"),
                                QStringLiteral("color_texture_nurbs_premul"),
                                &premultipliedAlphaSceneLinear, false);
                        QImage straightEquivalentTexture(
                            1, 1, QImage::Format_RGBA8888);
                        straightEquivalentTexture.setPixelColor(
                            0, 0, QColor(138, 58, 31, 128));
                        QVector<float> straightEquivalentSceneLinear;
                        const bool straightEquivalentCaptured =
                            captureNurbsAlphaVariant(
                                straightEquivalentTexture,
                                QStringLiteral("STRAIGHT"),
                                QStringLiteral(
                                    "color_texture_nurbs_premul_straight_equivalent"),
                                &straightEquivalentSceneLinear, false);
                        bool premultipliedRgbMatches =
                            premultipliedAlphaSceneLinear.size() ==
                            straightEquivalentSceneLinear.size();
                        double premultipliedRgbErrorSum = 0.0;
                        float premultipliedRgbMaximumError = 0.0f;
                        for (qsizetype index = 0;
                             premultipliedRgbMatches &&
                             index < premultipliedAlphaSceneLinear.size();
                             ++index) {
                            const float error = std::abs(
                                premultipliedAlphaSceneLinear[index] -
                                straightEquivalentSceneLinear[index]);
                            premultipliedRgbErrorSum += error;
                            premultipliedRgbMaximumError = std::max(
                                premultipliedRgbMaximumError, error);
                        }
                        premultipliedRgbMatches &=
                            premultipliedRgbErrorSum /
                                    std::max<qsizetype>(
                                        1, premultipliedAlphaSceneLinear.size()) <
                                1.0e-4 &&
                            premultipliedRgbMaximumError < 2.0e-3f;
                        passed &= check(
                            packedAlphaCaptured && ignoredAlphaCaptured &&
                                premultipliedAlphaCaptured &&
                                straightEquivalentCaptured &&
                                ignoredAndPackedMatch &&
                                premultipliedRgbMatches,
                            "GPU NURBS Image Texture must preserve CHANNEL_PACKED/IGNORE RGB and unpremultiply PREMUL RGB");
                        lightingCapturesSucceeded &= packedAlphaCaptured &&
                            ignoredAlphaCaptured && premultipliedAlphaCaptured &&
                            straightEquivalentCaptured && ignoredAndPackedMatch &&
                            premultipliedRgbMatches;

                        textureColorShading.lightingMode =
                            ViewportLightingMode::Studio;
                        QImage studioTextureDisplay;
                        QVector<float> studioTextureSceneLinear;
                        const bool studioTextureRendered =
                            renderCoordinatedScene(
                                textureColorShading,
                                QByteArrayLiteral(
                                    "color-source-match-texture-studio"),
                                &studioTextureDisplay, &textureSourceObject,
                                nullptr, nullptr, QString{},
                                &studioTextureSceneLinear);
                        const QString studioTextureDisplayPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture_studio.png"));
                        const QString studioTextureLinearPath = QDir(
                            lightingCaptureDirectory).filePath(
                                QStringLiteral("color_texture_studio.pfm"));
                        QFile studioTextureLinearFile(
                            studioTextureLinearPath);
                        const bool studioTextureLinearSaved =
                            studioTextureRendered &&
                            studioTextureSceneLinear.size() ==
                                textureExpectedValues &&
                            studioTextureLinearFile.open(QIODevice::WriteOnly) &&
                            studioTextureLinearFile.write(texturePfmHeader) ==
                                texturePfmHeader.size() &&
                            studioTextureLinearFile.write(
                                reinterpret_cast<const char *>(
                                    studioTextureSceneLinear.constData()),
                                studioTextureSceneLinear.size() *
                                    qsizetype(sizeof(float))) ==
                                studioTextureSceneLinear.size() *
                                    qsizetype(sizeof(float));
                        const bool studioTextureDisplaySaved =
                            studioTextureRendered &&
                            studioTextureDisplay.size() == QSize(640, 480) &&
                            studioTextureDisplay.save(
                                studioTextureDisplayPath);
                        lightingCapturesSucceeded &= studioTextureLinearSaved &&
                            studioTextureDisplaySaved;
                        const QString nonlinearTexturePayloadPath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_TEXTURE_RGBA32F_PATH");
                        if (!nonlinearTexturePayloadPath.isEmpty()) {
                            QFile nonlinearTexturePayloadFile(
                                nonlinearTexturePayloadPath);
                            const QByteArray nonlinearTexturePayload =
                                nonlinearTexturePayloadFile.open(
                                    QIODevice::ReadOnly)
                                    ? nonlinearTexturePayloadFile.readAll()
                                    : QByteArray{};
                            ViewportRenderObject nonlinearTextureObject =
                                textureSourceObject;
                            bool nonlinearTexturePayloadValid =
                                nonlinearTexturePayload.size() ==
                                4 * qsizetype(sizeof(float));
                            if (nonlinearTexturePayloadValid &&
                                !nonlinearTextureObject.meshMaterials.isEmpty()) {
                                WorkbenchMaterialData &nonlinearTextureMaterial =
                                    nonlinearTextureObject.meshMaterials[0];
                                nonlinearTextureMaterial
                                    .textureImageSceneLinearFloatWidth = 1;
                                nonlinearTextureMaterial
                                    .textureImageSceneLinearFloatHeight = 1;
                                nonlinearTextureMaterial
                                    .textureImageSceneLinearFloatDataBase64 =
                                    nonlinearTexturePayload.toBase64();
                                nonlinearTextureMaterial.textureImagePath.clear();
                                nonlinearTextureMaterial.textureImageSrgb = false;
                                nonlinearTextureMaterial.textureImageAlphaMode =
                                    QStringLiteral("STRAIGHT");
                            } else {
                                nonlinearTexturePayloadValid = false;
                            }
                            textureColorShading.lightingMode =
                                ViewportLightingMode::Flat;
                            QImage nonlinearTextureDisplay;
                            QVector<float> nonlinearTextureSceneLinear;
                            const bool nonlinearTextureRendered =
                                nonlinearTexturePayloadValid &&
                                renderCoordinatedScene(
                                    textureColorShading,
                                    QByteArrayLiteral(
                                        "color-source-match-texture-adobe"),
                                    &nonlinearTextureDisplay,
                                    &nonlinearTextureObject, nullptr, nullptr,
                                    QString{}, &nonlinearTextureSceneLinear);
                            const QString nonlinearTextureDisplayPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    QStringLiteral(
                                        "color_texture_adobe.png"));
                            const QString nonlinearTextureLinearPath = QDir(
                                lightingCaptureDirectory).filePath(
                                    QStringLiteral(
                                        "color_texture_adobe.pfm"));
                            QFile nonlinearTextureLinearFile(
                                nonlinearTextureLinearPath);
                            const bool nonlinearTextureLinearSaved =
                                nonlinearTextureRendered &&
                                nonlinearTextureSceneLinear.size() ==
                                    textureExpectedValues &&
                                nonlinearTextureLinearFile.open(
                                    QIODevice::WriteOnly) &&
                                nonlinearTextureLinearFile.write(
                                    texturePfmHeader) ==
                                    texturePfmHeader.size() &&
                                nonlinearTextureLinearFile.write(
                                    reinterpret_cast<const char *>(
                                        nonlinearTextureSceneLinear.constData()),
                                    nonlinearTextureSceneLinear.size() *
                                        qsizetype(sizeof(float))) ==
                                    nonlinearTextureSceneLinear.size() *
                                        qsizetype(sizeof(float));
                            const bool nonlinearTextureDisplaySaved =
                                nonlinearTextureRendered &&
                                nonlinearTextureDisplay.size() ==
                                    QSize(640, 480) &&
                                nonlinearTextureDisplay.save(
                                    nonlinearTextureDisplayPath);
                            lightingCapturesSucceeded &=
                                nonlinearTextureLinearSaved &&
                                nonlinearTextureDisplaySaved;
                        }
                        const QVector3D matchedWorldColor(0.12f, 0.07f,
                                                          0.035f);
                        const QVector3D matchedViewportColorSceneLinear(
                            0.25f, 0.45f, 0.65f);
                        const QVector3D matchedViewportColorSrgb =
                            workbenchSceneLinearToSrgb(
                                matchedViewportColorSceneLinear);
                        const QColor matchedViewportColor =
                            QColor::fromRgbF(matchedViewportColorSrgb.x(),
                                             matchedViewportColorSrgb.y(),
                                             matchedViewportColorSrgb.z());
                        struct BackgroundCaptureCase {
                            const char *name;
                            ViewportBackgroundMode mode;
                        };
                        const BackgroundCaptureCase backgroundCases[] = {
                            {"world", ViewportBackgroundMode::World},
                            {"viewport", ViewportBackgroundMode::Custom}};
                        struct TransformCaptureCase {
                            const char *name;
                            int colorTransform;
                            qreal exposure;
                            qreal gamma;
                        };
                        const TransformCaptureCase transformCases[] = {
                            {"agx", 0, 0.0, 1.0},
                            {"standard", 1, 0.0, 1.0},
                            {"standard_exposure_gamma", 1, 1.0, 2.0}};
                        for (const TransformCaptureCase &transformCase :
                             transformCases) {
                            for (const BackgroundCaptureCase &captureCase :
                                 backgroundCases) {
                                if (transformCase.exposure != 0.0 &&
                                    captureCase.mode !=
                                        ViewportBackgroundMode::World) {
                                    continue;
                                }
                                ViewportShadingSettings backgroundShading =
                                    rawFaceShading;
                                backgroundShading.lightingMode =
                                    ViewportLightingMode::Flat;
                                backgroundShading.backgroundMode =
                                    captureCase.mode;
                                backgroundShading.colorTransform =
                                    transformCase.colorTransform;
                                backgroundShading.colorExposure =
                                    transformCase.exposure;
                                backgroundShading.colorGamma =
                                    transformCase.gamma;
                                const QString stem = QStringLiteral(
                                    "background_%1_%2")
                                    .arg(QString::fromLatin1(transformCase.name),
                                         QString::fromLatin1(captureCase.name));
                                QImage display;
                                QVector<float> sceneLinear;
                                const QVector3D *world =
                                    captureCase.mode ==
                                            ViewportBackgroundMode::World
                                        ? &matchedWorldColor : nullptr;
                                const QColor *viewport =
                                    captureCase.mode ==
                                            ViewportBackgroundMode::Custom
                                        ? &matchedViewportColor : nullptr;
                                const bool rendered = renderCoordinatedScene(
                                    backgroundShading,
                                    QByteArrayLiteral("background-match-") +
                                        stem.toUtf8(),
                                    &display, &polygonCubeObject, nullptr,
                                    nullptr, QString{}, &sceneLinear, world,
                                    viewport);
                                const QString displayPath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        stem + QStringLiteral(".png"));
                                const QString linearPath = QDir(
                                    lightingCaptureDirectory).filePath(
                                        stem + QStringLiteral(".pfm"));
                                QFile linearFile(linearPath);
                                const QByteArray pfmHeader =
                                    QByteArrayLiteral("PF\n640 480\n-1.0\n");
                                const qsizetype expectedValues = 640 * 480 * 3;
                                const bool linearSaved = rendered &&
                                    sceneLinear.size() == expectedValues &&
                                    linearFile.open(QIODevice::WriteOnly) &&
                                    linearFile.write(pfmHeader) ==
                                        pfmHeader.size() &&
                                    linearFile.write(reinterpret_cast<const char *>(
                                        sceneLinear.constData()),
                                        sceneLinear.size() *
                                            qsizetype(sizeof(float))) ==
                                        sceneLinear.size() *
                                            qsizetype(sizeof(float));
                                const bool displaySaved = rendered &&
                                    display.size() == QSize(640, 480) &&
                                    display.save(displayPath);
                                lightingCapturesSucceeded &= linearSaved &&
                                                             displaySaved;
                            }
                        }
                        passed &= check(
                            lightingCapturesSucceeded,
                            "matched cube lighting, color-source, and background captures must write scene-linear PFM and display PNG outputs");
                    }
                    const QString cavityDiagnosticPrefix = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_CAVITY_DIAGNOSTIC_PREFIX");
                    if (!cavityDiagnosticPrefix.isEmpty()) {
                        ViewportShadingSettings cavityDiagnosticShading =
                            rawFaceShading;
                        cavityDiagnosticShading.cavity = true;
                        cavityDiagnosticShading.cavityType =
                            ViewportCavityType::Both;
                        cavityDiagnosticShading.outline = false;
                        cavityDiagnosticShading.shadows = false;
                        QImage cavityDiagnosticDisplay;
                        const bool cavityDiagnosticSaved =
                            renderCoordinatedScene(
                                cavityDiagnosticShading,
                                QByteArrayLiteral(
                                    "polygon-cube-cavity-diagnostic"),
                                &cavityDiagnosticDisplay,
                                &polygonCubeObject, nullptr, nullptr,
                                cavityDiagnosticPrefix);
                        passed &= check(
                            cavityDiagnosticSaved,
                            "opt-in cavity diagnostic must save pre-composite amounts and shared inputs");
                    }
                    const QString gpuDofCapturePath = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_GPU_DOF_CAPTURE");
                    if (!gpuDofCapturePath.isEmpty()) {
                        ViewportTransform dofCamera = gpuTransform;
                        dofCamera.setPerspectiveEnabled(true);
                        ViewportCameraPreferences dofOptics =
                            dofCamera.cameraPreferences();
                        dofOptics.sceneCameraEnabled = true;
                        // The viewport projection's 2x camera zoom makes its
                        // 50 mm preference equivalent to a 25 mm Blender
                        // camera lens for this matched 36 mm horizontal fit.
                        dofOptics.focalLengthMillimeters = 25.0;
                        dofOptics.focusDistance = std::sqrt(
                            std::pow(dofCamera.cameraPosition(glSize).x, 2.0) +
                            std::pow(dofCamera.cameraPosition(glSize).y, 2.0) +
                            std::pow(dofCamera.cameraPosition(glSize).z, 2.0));
                        // Use a deliberately large aperture so this compact
                        // cube produces measurable CoC at the matched view.
                        dofOptics.fStop = 0.1;
                        bool apertureBladesOk = false;
                        const int requestedApertureBlades =
                            qgetenv("CLASSICAD_VIEWPORT_GPU_DOF_APERTURE_BLADES")
                                .toInt(&apertureBladesOk);
                        dofOptics.apertureBlades = apertureBladesOk &&
                                requestedApertureBlades >= 0 &&
                                requestedApertureBlades <= 16
                            ? requestedApertureBlades : 6;
                        dofOptics.apertureRotationRadians = 0.2;
                        dofOptics.apertureRatio = 1.2;
                        QImage dofBaselineImage;
                        QImage dofImage;
                        QImage dofGatedImage;
                        const QString dofBaselinePath = qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_GPU_DOF_BASELINE_CAPTURE");
                        const bool baselineCaptured = renderCoordinatedScene(
                            rawFaceShading,
                            QByteArrayLiteral("gpu-dof-baseline"),
                            &dofBaselineImage, &polygonCubeObject, &dofCamera) &&
                            (dofBaselinePath.isEmpty() ||
                             dofBaselineImage.save(dofBaselinePath));
                        const bool dofCaptured = renderCoordinatedScene(
                            rawFaceShading, QByteArrayLiteral("gpu-dof-enabled"),
                            &dofImage, &polygonCubeObject, &dofCamera,
                            &dofOptics) && dofImage.save(gpuDofCapturePath);
                        dofOptics.sceneCameraEnabled = false;
                        const bool gatedCaptured = renderCoordinatedScene(
                            rawFaceShading, QByteArrayLiteral("gpu-dof-gated"),
                            &dofGatedImage, &polygonCubeObject, &dofCamera,
                            &dofOptics);
                        const QColor expectedBackground(34, 34, 34, 255);
                        bool knownOpaqueBackground =
                            baselineCaptured && dofCaptured;
                        for (const QPoint &sample : {
                                 QPoint(5, 5),
                                 QPoint(glSize.width() - 6, 5),
                                 QPoint(5, glSize.height() - 6),
                                 QPoint(glSize.width() - 6,
                                        glSize.height() - 6)}) {
                            const QColor dofBackground =
                                dofImage.pixelColor(sample);
                            knownOpaqueBackground &=
                                dofBaselineImage.pixelColor(sample) ==
                                    expectedBackground &&
                                dofBackground.alpha() == 255 &&
                                (dofBackground.red() > 0 ||
                                 dofBackground.green() > 0 ||
                                 dofBackground.blue() > 0);
                        }
                        const auto isFixtureBackground =
                            [&expectedBackground](const QColor &pixel) {
                                return pixel == expectedBackground;
                            };
                        int changedObjectEdgePixels = 0;
                        if (baselineCaptured && dofCaptured) {
                            const auto channelDifference = [](
                                const QColor &left, const QColor &right) {
                                return std::max({
                                    std::abs(left.red() - right.red()),
                                    std::abs(left.green() - right.green()),
                                    std::abs(left.blue() - right.blue())});
                            };
                            for (int y = 0; y < glSize.height(); ++y) {
                                for (int x = 0; x < glSize.width(); ++x) {
                                    const QColor baselinePixel =
                                        dofBaselineImage.pixelColor(x, y);
                                    if (isFixtureBackground(baselinePixel)) {
                                        continue;
                                    }
                                    for (const QPoint &offset :
                                         {QPoint(1, 0), QPoint(0, 1)}) {
                                        const int neighborX = x + offset.x();
                                        const int neighborY = y + offset.y();
                                        if (neighborX >= glSize.width() ||
                                            neighborY >= glSize.height()) {
                                            continue;
                                        }
                                        const QColor baselineNeighbor =
                                            dofBaselineImage.pixelColor(
                                                neighborX, neighborY);
                                        if (isFixtureBackground(baselineNeighbor)) {
                                            continue;
                                        }
                                        const int baselineContrast =
                                            channelDifference(baselinePixel,
                                                              baselineNeighbor);
                                        if (baselineContrast <= 20) continue;
                                        const int blurredContrast =
                                            channelDifference(
                                                dofImage.pixelColor(x, y),
                                                dofImage.pixelColor(neighborX,
                                                                    neighborY));
                                        if (blurredContrast < baselineContrast - 2) {
                                            ++changedObjectEdgePixels;
                                        }
                                    }
                                }
                            }
                        }
                        passed &= check(
                            baselineCaptured && dofCaptured && gatedCaptured &&
                                knownOpaqueBackground &&
                                changedObjectEdgePixels > 0 &&
                                dofGatedImage == dofBaselineImage,
                            "GPU Workbench DOF must retain opaque background coverage, smooth high-contrast surface edges, and be bypassed when camera DOF is disabled");
                    }
                    ViewportShadingSettings outlinedSceneShading =
                        rawFaceShading;
                    outlinedSceneShading.outline = true;
                    const bool gpuWorkbenchOutlineDrawSucceeded =
                        renderCoordinatedScene(
                            outlinedSceneShading,
                            QByteArrayLiteral("outline-enabled"),
                            &gpuWorkbenchOutlineImage);
                    const bool gpuWorkbenchOutlineBaselineDrawSucceeded =
                        renderCoordinatedScene(
                            rawFaceShading,
                            QByteArrayLiteral("outline-baseline"),
                            &gpuWorkbenchOutlineBaselineImage);
                    gpuWorkbenchOutlineCaptureSucceeded =
                        gpuWorkbenchOutlineDrawSucceeded &&
                        gpuWorkbenchOutlineBaselineDrawSucceeded;
                    const QString outlineCapturePath = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_OUTLINE_CAPTURE");
                    if (!outlineCapturePath.isEmpty()) {
                        gpuWorkbenchOutlineImage.save(outlineCapturePath);
                    }
                    const QString outlineBaselineCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_OUTLINE_BASELINE_CAPTURE");
                    if (!outlineBaselineCapturePath.isEmpty()) {
                        gpuWorkbenchOutlineBaselineImage.save(
                            outlineBaselineCapturePath);
                    }
                    const QString polygonCubeOutlineCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_CAPTURE");
                    if (!polygonCubeOutlineCapturePath.isEmpty()) {
                        const QString polygonCubeOutlineBaselinePath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_BASELINE_CAPTURE");
                        const QString polygonCubeOutlineLinearPath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_LINEAR_CAPTURE");
                        const QString polygonCubeOutlineBaselineLinearPath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_BASELINE_LINEAR_CAPTURE");
                        QImage polygonCubeOutlineImage;
                        QImage polygonCubeOutlineBaselineImage;
                        QVector<float> outlineLinear;
                        QVector<float> outlineBaselineLinear;
                        const bool outlineCaptured =
                            renderCoordinatedScene(
                                outlinedSceneShading,
                                QByteArrayLiteral("polygon-cube-outline"),
                                &polygonCubeOutlineImage, &polygonCubeObject,
                                nullptr, nullptr, QString{},
                                polygonCubeOutlineLinearPath.isEmpty()
                                    ? nullptr : &outlineLinear) &&
                            polygonCubeOutlineImage.save(
                                polygonCubeOutlineCapturePath);
                        const bool baselineCaptured =
                            polygonCubeOutlineBaselinePath.isEmpty() ||
                            (renderCoordinatedScene(
                                 rawFaceShading,
                                 QByteArrayLiteral("polygon-cube-outline-baseline"),
                                 &polygonCubeOutlineBaselineImage,
                                 &polygonCubeObject, nullptr, nullptr,
                                 QString{},
                                 polygonCubeOutlineBaselineLinearPath.isEmpty()
                                     ? nullptr : &outlineBaselineLinear) &&
                             polygonCubeOutlineBaselineImage.save(
                                 polygonCubeOutlineBaselinePath));
                        const auto saveLinearCapture = [&](const QString &path,
                                                            const QVector<float> &pixels) {
                            if (path.isEmpty()) return true;
                            QFile file(path);
                            const QByteArray header =
                                QByteArrayLiteral("PF\n640 480\n-1.0\n");
                            return pixels.size() == 640 * 480 * 3 &&
                                file.open(QIODevice::WriteOnly) &&
                                file.write(header) == header.size() &&
                                file.write(reinterpret_cast<const char *>(
                                               pixels.constData()),
                                           pixels.size() * qsizetype(sizeof(float))) ==
                                    pixels.size() * qsizetype(sizeof(float));
                        };
                        const bool linearCapturesSaved =
                            saveLinearCapture(polygonCubeOutlineLinearPath,
                                              outlineLinear) &&
                            saveLinearCapture(
                                polygonCubeOutlineBaselineLinearPath,
                                outlineBaselineLinear);
                        passed &= check(
                            outlineCaptured && baselineCaptured &&
                                linearCapturesSaved,
                            "matched polygon cube outline captures must render and save");
                    }
                    const bool gpuWorkbenchShadowDrawSucceeded =
                        renderCoordinatedScene(
                            shadowShading,
                            QByteArrayLiteral("shadow-enabled"),
                            &gpuWorkbenchShadowImage);
                    gpuWorkbenchShadowCaptureSucceeded =
                        gpuWorkbenchShadowDrawSucceeded;
                    const QString workbenchShadowCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_SHADOW_CAPTURE");
                    if (!workbenchShadowCapturePath.isEmpty()) {
                        gpuWorkbenchShadowImage.save(
                            workbenchShadowCapturePath);
                    }
                    const QString workbenchShadowBaselineCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_SHADOW_BASELINE_CAPTURE");
                    if (!workbenchShadowBaselineCapturePath.isEmpty()) {
                        gpuWorkbenchOutlineBaselineImage.save(
                            workbenchShadowBaselineCapturePath);
                    }
                    const QString polygonCubeShadowCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_POLYGON_CUBE_SHADOW_CAPTURE");
                    if (!polygonCubeShadowCapturePath.isEmpty()) {
                        const QString polygonCubeShadowBaselinePath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_POLYGON_CUBE_SHADOW_BASELINE_CAPTURE");
                        QImage polygonCubeShadowImage;
                        QImage polygonCubeShadowBaselineImage;
                        const bool shadowCaptured =
                            renderCoordinatedScene(
                                shadowShading,
                                QByteArrayLiteral("polygon-cube-shadows"),
                                &polygonCubeShadowImage, &polygonCubeObject) &&
                            polygonCubeShadowImage.save(
                                polygonCubeShadowCapturePath);
                        const bool baselineCaptured =
                            polygonCubeShadowBaselinePath.isEmpty() ||
                            (renderCoordinatedScene(
                                 rawFaceShading,
                                 QByteArrayLiteral("polygon-cube-shadow-baseline"),
                                 &polygonCubeShadowBaselineImage,
                                 &polygonCubeObject) &&
                             polygonCubeShadowBaselineImage.save(
                                 polygonCubeShadowBaselinePath));
                        passed &= check(
                            shadowCaptured && baselineCaptured,
                            "matched polygon cube shadow captures must render and save");
                    }
                    gpuCavityDrawSucceeded = renderCoordinatedScene(
                        rawFaceShading, QByteArrayLiteral("cavity-baseline"),
                        &gpuCavityBaselineImage);
                    ViewportShadingSettings cavityShading = rawFaceShading;
                    cavityShading.cavity = true;
                    cavityShading.cavityType = ViewportCavityType::World;
                    gpuCavityDrawSucceeded = renderCoordinatedScene(
                        cavityShading, QByteArrayLiteral("cavity-enabled"),
                        &gpuCavityImage) && gpuCavityDrawSucceeded;
                    const QString cavityBaselineCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_CAVITY_BASELINE_CAPTURE");
                    if (!cavityBaselineCapturePath.isEmpty()) {
                        gpuCavityBaselineImage.save(
                            cavityBaselineCapturePath);
                    }
                    const QString cavityCapturePath = qEnvironmentVariable(
                        "CLASSICAD_VIEWPORT_CAVITY_CAPTURE");
                    if (!cavityCapturePath.isEmpty()) {
                        gpuCavityImage.save(cavityCapturePath);
                    }
                    const QString polygonCubeCavityCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_POLYGON_CUBE_CAVITY_CAPTURE");
                    if (!polygonCubeCavityCapturePath.isEmpty()) {
                        const QString polygonCubeCavityBaselinePath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_POLYGON_CUBE_CAVITY_BASELINE_CAPTURE");
                        QImage polygonCubeCavityImage;
                        QImage polygonCubeCavityBaselineImage;
                        const bool cavityCaptured =
                            renderCoordinatedScene(
                                cavityShading,
                                QByteArrayLiteral("polygon-cube-cavity"),
                                &polygonCubeCavityImage, &polygonCubeObject) &&
                            polygonCubeCavityImage.save(
                                polygonCubeCavityCapturePath);
                        const bool baselineCaptured =
                            polygonCubeCavityBaselinePath.isEmpty() ||
                            (renderCoordinatedScene(
                                 rawFaceShading,
                                 QByteArrayLiteral("polygon-cube-cavity-baseline"),
                                 &polygonCubeCavityBaselineImage,
                                 &polygonCubeObject) &&
                             polygonCubeCavityBaselineImage.save(
                                 polygonCubeCavityBaselinePath));
                        passed &= check(
                            cavityCaptured && baselineCaptured,
                            "matched polygon cube cavity captures must render and save");
                    }
                    const QString qpainterCavityCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_QPAINTER_CAVITY_CAPTURE");
                    const QString qpainterShadowCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_QPAINTER_SHADOW_CAPTURE");
                    if (!qpainterCavityCapturePath.isEmpty() ||
                        !qpainterShadowCapturePath.isEmpty()) {
                        const QString qpainterCavityBaselinePath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_QPAINTER_CAVITY_BASELINE_CAPTURE");
                        const QString qpainterCavityLinearPath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_QPAINTER_CAVITY_LINEAR_CAPTURE");
                        const QString qpainterCavityBaselineLinearPath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_QPAINTER_CAVITY_BASELINE_LINEAR_CAPTURE");
                        const QString qpainterShadowBaselinePath =
                            qEnvironmentVariable(
                                "CLASSICAD_VIEWPORT_QPAINTER_SHADOW_BASELINE_CAPTURE");
                        CurveHitTester qpainterCavityHitTester;
                        ViewportRenderer qpainterCavityRenderer(
                            gpuTransform, qpainterCavityHitTester);
                        ViewportRenderObject qpainterCavityObject =
                            polygonCubeObject;
                        auto qpainterCavityDepthGeometry =
                            QSharedPointer<ViewportDepthGeometry>::create();
                        const PolygonMesh3D &qpainterCavityMesh =
                            qpainterCavityObject.shape.polygonMesh;
                        for (const QVector<int> &face :
                             qpainterCavityMesh.faces) {
                            if (face.size() < 3) continue;
                            for (int corner = 1; corner + 1 < face.size();
                                 ++corner) {
                                const int indices[] = {
                                    face[0], face[corner], face[corner + 1]};
                                const auto pointAt =
                                    [&qpainterCavityMesh](int index) {
                                        const Point3D &point =
                                            qpainterCavityMesh.vertices[index];
                                        return QVector3D(
                                            float(point.x), float(point.y),
                                            float(point.z));
                                    };
                                const QVector3D a = pointAt(indices[0]);
                                const QVector3D b = pointAt(indices[1]);
                                const QVector3D c = pointAt(indices[2]);
                                const QVector3D normal =
                                    QVector3D::crossProduct(b - a, c - a)
                                        .normalized();
                                for (const QVector3D &point : {a, b, c}) {
                                    qpainterCavityDepthGeometry
                                        ->surfaceVertices.append(point);
                                    qpainterCavityDepthGeometry
                                        ->surfaceNormals.append(normal);
                                }
                            }
                        }
                        qpainterCavityObject.preparedDepthGeometry =
                            qpainterCavityDepthGeometry;
                        const auto captureQPainterCavity = [&] (
                            const ViewportShadingSettings &settings,
                            QImage *display,
                            QVector<float> *linear) {
                            if (display == nullptr) return false;
                            ViewportShadingSettings captureSettings = settings;
                            if (linear != nullptr) {
                                // The weighted compositor retains its
                                // scene-linear target for outline resolve.
                                // A zero-alpha outline keeps this cavity image
                                // unchanged while exposing that target.
                                captureSettings.outline = true;
                                captureSettings.outlineColor.setAlpha(0);
                            }
                            qpainterCavityRenderer.setShadingSettings(
                                captureSettings);
                            *display = QImage(
                                glSize, QImage::Format_ARGB32_Premultiplied);
                            display->fill(Qt::transparent);
                            RasterWorkbenchSurfaceLayers layers;
                            const auto drawObject = [&] (
                                QPainter &painter,
                                const ViewportRenderObject &object,
                                RasterWorkbenchFragmentSink *fragmentSink) {
                                qpainterCavityRenderer.drawShape(
                                    painter, object.shape, glSize, false, false,
                                    true, QColor(), QString(), 0.0,
                                    object.objectId, object.geometryRevision,
                                    object.preparedDepthGeometry.data(),
                                    object.preparedGeometryOffset,
                                    object.displayColorSceneLinear,
                                    object.workbenchMaterial,
                                    object.displayName, object.libraryPath,
                                    object.meshObjectTransform,
                                    object.meshMaterials, fragmentSink);
                            };
                            const QVector3D worldBackground(0.12f, 0.07f,
                                                            0.035f);
                            const bool composed =
                                compositeRasterWorkbenchSurfaces(
                                    display, {qpainterCavityObject}, gpuTransform,
                                    glSize, 1.0, false, true, captureSettings,
                                    worldBackground, drawObject, &layers);
                            if (!composed) return false;
                            applyRasterWorkbenchEffects(
                                display, {qpainterCavityObject}, gpuTransform,
                                glSize, 1.0, captureSettings, &layers);
                            if (linear != nullptr) {
                                if (layers.sceneLinearSize != glSize ||
                                    layers.sceneLinearColor.size() !=
                                        static_cast<std::size_t>(
                                            glSize.width() * glSize.height())) {
                                    return false;
                                }
                                linear->resize(glSize.width() *
                                               glSize.height() * 3);
                                for (int y = 0; y < glSize.height(); ++y) {
                                    for (int x = 0; x < glSize.width(); ++x) {
                                        const std::size_t source =
                                            static_cast<std::size_t>(
                                                y * glSize.width() + x);
                                        const qsizetype destination =
                                            (qsizetype(glSize.height() - 1 - y) *
                                                 glSize.width() + x) * 3;
                                        const RasterWorkbenchLinearPixel &pixel =
                                            layers.sceneLinearColor[source];
                                        (*linear)[destination] = pixel.red;
                                        (*linear)[destination + 1] = pixel.green;
                                        (*linear)[destination + 2] = pixel.blue;
                                    }
                                }
                            }
                            return !display->isNull();
                        };
                        ViewportShadingSettings qpainterCavityBaselineSettings =
                            rawFaceShading;
                        qpainterCavityBaselineSettings.outline = false;
                        qpainterCavityBaselineSettings.shadows = false;
                        qpainterCavityBaselineSettings.cavity = false;
                        ViewportShadingSettings qpainterCavitySettings =
                            qpainterCavityBaselineSettings;
                        qpainterCavitySettings.cavity = true;
                        qpainterCavitySettings.cavityType =
                            ViewportCavityType::World;
                        qpainterCavitySettings.cavityDistance = 0.2;
                        qpainterCavitySettings.cavityAttenuation = 1.0;
                        qpainterCavitySettings.cavitySamples = 16;
                        QImage qpainterCavityImage;
                        QImage qpainterCavityBaselineImage;
                        QVector<float> qpainterCavityLinear;
                        QVector<float> qpainterCavityBaselineLinear;
                        const bool qpainterCavityCaptured =
                            qpainterCavityCapturePath.isEmpty() ||
                            (captureQPainterCavity(
                                 qpainterCavitySettings,
                                 &qpainterCavityImage,
                                 qpainterCavityLinearPath.isEmpty()
                                     ? nullptr : &qpainterCavityLinear) &&
                             qpainterCavityImage.save(
                                 qpainterCavityCapturePath));
                        const bool qpainterCavityBaselineCaptured =
                            qpainterCavityBaselinePath.isEmpty() ||
                            (captureQPainterCavity(
                                 qpainterCavityBaselineSettings,
                                 &qpainterCavityBaselineImage,
                                 qpainterCavityBaselineLinearPath.isEmpty()
                                     ? nullptr
                                     : &qpainterCavityBaselineLinear) &&
                             qpainterCavityBaselineImage.save(
                                 qpainterCavityBaselinePath));
                        const auto saveQPainterLinearCapture = [&] (
                            const QString &path,
                            const QVector<float> &pixels) {
                            if (path.isEmpty()) return true;
                            QFile file(path);
                            const QByteArray header =
                                QByteArrayLiteral("PF\n640 480\n-1.0\n");
                            return pixels.size() == 640 * 480 * 3 &&
                                file.open(QIODevice::WriteOnly) &&
                                file.write(header) == header.size() &&
                                file.write(reinterpret_cast<const char *>(
                                               pixels.constData()),
                                           pixels.size() * qsizetype(sizeof(float))) ==
                                    pixels.size() * qsizetype(sizeof(float));
                        };
                        const bool qpainterLinearSaved =
                            saveQPainterLinearCapture(
                                qpainterCavityLinearPath,
                                qpainterCavityLinear) &&
                            saveQPainterLinearCapture(
                                qpainterCavityBaselineLinearPath,
                                qpainterCavityBaselineLinear);
                        bool qpainterShadowCaptured = true;
                        bool qpainterShadowBaselineCaptured = true;
                        if (!qpainterShadowCapturePath.isEmpty()) {
                            ViewportShadingSettings qpainterShadowSettings =
                                qpainterCavityBaselineSettings;
                            qpainterShadowSettings.shadows = true;
                            qpainterShadowSettings.shadowIntensity = 0.5;
                            QImage qpainterShadowImage;
                            QImage qpainterShadowBaselineImage;
                            qpainterShadowCaptured = captureQPainterCavity(
                                qpainterShadowSettings, &qpainterShadowImage,
                                nullptr) &&
                                qpainterShadowImage.save(
                                    qpainterShadowCapturePath);
                            qpainterShadowBaselineCaptured =
                                qpainterShadowBaselinePath.isEmpty() ||
                                (captureQPainterCavity(
                                     qpainterCavityBaselineSettings,
                                     &qpainterShadowBaselineImage, nullptr) &&
                                 qpainterShadowBaselineImage.save(
                                     qpainterShadowBaselinePath));
                        }
                        passed &= check(
                            qpainterCavityCaptured &&
                                qpainterCavityBaselineCaptured &&
                                qpainterLinearSaved &&
                                qpainterShadowCaptured &&
                                qpainterShadowBaselineCaptured,
                            "opt-in QPainter cavity and shadow captures must render matched baseline and effect output");
                    }
                    const QString transparencyCapturePath =
                        qEnvironmentVariable(
                            "CLASSICAD_VIEWPORT_TRANSPARENCY_CAPTURE");
                    if (!transparencyCapturePath.isEmpty()) {
                        const QMatrix4x4 transparencyViewProjection =
                            viewportViewProjection(gpuTransform, glSize);
                        bool inverseValid = false;
                        const QMatrix4x4 inverseViewProjection =
                            transparencyViewProjection.inverted(&inverseValid);
                        const auto worldAtClip = [&](float x, float y,
                                                      float z) {
                            const QVector4D homogeneous = inverseViewProjection *
                                QVector4D(x, y, z, 1.0f);
                            return homogeneous.toVector3DAffine();
                        };
                        const auto worldPoint = [](const QVector3D &point) {
                            return Point3D{point.x(), point.y(), point.z()};
                        };
                        const auto makeTransparentQuad = [&] (
                            const QString &name, float centerX,
                            float halfWidth, float halfHeight, float depth,
                            const QVector3D &color, qreal alpha,
                            quint64 objectValue) {
                            ViewportRenderObject object;
                            object.shape.geometryType = GeometryType::PolygonMesh;
                            object.shape.polygonMesh.vertices = {
                                worldPoint(worldAtClip(
                                    centerX - halfWidth, -halfHeight, depth)),
                                worldPoint(worldAtClip(
                                    centerX + halfWidth, -halfHeight, depth)),
                                worldPoint(worldAtClip(
                                    centerX + halfWidth, halfHeight, depth)),
                                worldPoint(worldAtClip(
                                    centerX - halfWidth, halfHeight, depth))};
                            object.shape.polygonMesh.faces = {{0, 1, 2, 3}};
                            object.shape.polygonMesh.faceMaterialIndices = {0};
                            object.objectId = ObjectId::fromValue(objectValue);
                            object.displayName = name;
                            object.geometryRevision = 1;
                            object.hasWorkbenchMaterial = true;
                            object.workbenchMaterial.baseColorSceneLinear = color;
                            object.workbenchMaterial.alpha = alpha;
                            object.meshMaterials = {object.workbenchMaterial};
                            return object;
                        };
                        const Point3D cameraTargetPoint =
                            gpuTransform.viewTarget();
                        const QVector3D target(
                            float(cameraTargetPoint.x),
                            float(cameraTargetPoint.y),
                            float(cameraTargetPoint.z));
                        const Point3D cameraDirectionPoint =
                            gpuTransform.viewDirection();
                        const QVector3D cameraDirection(
                            float(cameraDirectionPoint.x),
                            float(cameraDirectionPoint.y),
                            float(cameraDirectionPoint.z));
                        const auto projectedDepth =
                            [&](const QVector3D &worldPoint) {
                                const QVector4D clip =
                                    transparencyViewProjection *
                                    QVector4D(worldPoint, 1.0f);
                                return clip.z() / clip.w();
                            };
                        const float rearDepth = projectedDepth(
                            target - cameraDirection * 0.35f);
                        const float frontDepth = projectedDepth(
                            target + cameraDirection * 0.35f);
                        const QVector<ViewportRenderObject> transparentObjects{
                            makeTransparentQuad(
                                QStringLiteral("RearTranslucentSurface"),
                                -0.1f, 0.48f, 0.64f, rearDepth,
                                QVector3D(0.08f, 0.25f, 0.95f), 0.55, 9101),
                            makeTransparentQuad(
                                QStringLiteral("FrontTranslucentSurface"),
                                0.1f, 0.4f, 0.53333336f, frontDepth,
                                QVector3D(0.95f, 0.12f, 0.04f), 0.48, 9102)};
                        ViewportShadingSettings transparencyShading;
                        transparencyShading.outline = false;
                        transparencyShading.shadows = false;
                        transparencyShading.cavity = false;
                        ViewportAaRenderer transparencyAaRenderer;
                        transparencyAaRenderer.setMode(static_cast<int>(
                            ViewportAntiAliasingMode::SinglePass));
                        transparencyAaRenderer.setDisplayTransform(
                            transparencyShading, true);
                        const bool destinationFramebufferReady =
                            framebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                            glSize.height());
                        const GLboolean previousScissorEnabled =
                            functions.glIsEnabled(GL_SCISSOR_TEST);
                        GLint previousScissorBox[4] = {};
                        functions.glGetIntegerv(GL_SCISSOR_BOX,
                                                previousScissorBox);
                        functions.glDisable(GL_SCISSOR_TEST);
                        functions.glScissor(0, 0, glSize.width(),
                                            glSize.height());
                        const bool transparencyAaReady =
                            destinationFramebufferReady &&
                            transparencyAaRenderer.beginSceneSample(
                                glSize, 1.0, gpuTransform,
                                QByteArrayLiteral("transparent-overlap"),
                                false, true);
                        ViewportWorkbenchSceneColorPassResult transparencyResult;
                        if (transparencyAaReady) {
                            const GLfloat background[4] = {
                                0.0232f, 0.0232f, 0.0232f, 1.0f};
                            functions.glClearBufferfv(GL_COLOR, 0, background);
                            transparencyResult =
                                ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                                    gpuSurfaceRenderer,
                                    ViewportWorkbenchScenePassPacket::fromScene(
                                        transparentObjects,
                                        transparencyAaRenderer.sampleCamera(),
                                        glSize, 1.0, transparencyShading,
                                        &transparencyAaRenderer.targets(), true));
                            if (transparencyResult.rendered) {
                                transparencyAaRenderer
                                    .markCurrentSampleAttachmentsValid(
                                        transparencyResult.report
                                            .populatedAttachments);
                            }
                            transparencyAaRenderer.finishSceneSample();
                        }
                        functions.glFinish();
                        const QImage transparencyImage = framebuffer.toImage();
                        const QColor transparencyBackground =
                            transparencyImage.pixelColor(0, 0);
                        int transparencyMinX = glSize.width();
                        int transparencyMinY = glSize.height();
                        int transparencyMaxX = -1;
                        int transparencyMaxY = -1;
                        for (int y = 0; y < transparencyImage.height(); ++y) {
                            for (int x = 0; x < transparencyImage.width(); ++x) {
                                const QColor pixel =
                                    transparencyImage.pixelColor(x, y);
                                const int channelDelta = std::max({
                                    std::abs(pixel.red() -
                                             transparencyBackground.red()),
                                    std::abs(pixel.green() -
                                             transparencyBackground.green()),
                                    std::abs(pixel.blue() -
                                             transparencyBackground.blue())});
                                if (channelDelta <= 4) continue;
                                transparencyMinX =
                                    std::min(transparencyMinX, x);
                                transparencyMinY =
                                    std::min(transparencyMinY, y);
                                transparencyMaxX =
                                    std::max(transparencyMaxX, x);
                                transparencyMaxY =
                                    std::max(transparencyMaxY, y);
                            }
                        }
                        const auto withinPixels = [](int actual,
                                                     int expected) {
                            return std::abs(actual - expected) <= 2;
                        };
                        const bool transparencyBoundsMatch =
                            withinPixels(transparencyMinX, 134) &&
                            withinPixels(transparencyMinY, 86) &&
                            withinPixels(transparencyMaxX, 479) &&
                            withinPixels(transparencyMaxY, 393);
                        functions.glScissor(previousScissorBox[0],
                                            previousScissorBox[1],
                                            previousScissorBox[2],
                                            previousScissorBox[3]);
                        if (previousScissorEnabled) {
                            functions.glEnable(GL_SCISSOR_TEST);
                        } else {
                            functions.glDisable(GL_SCISSOR_TEST);
                        }
                        passed &= check(
                            inverseValid && destinationFramebufferReady &&
                                transparencyAaReady &&
                                transparencyResult.rendered &&
                                transparencyImage.size() == glSize &&
                                transparencyBoundsMatch &&
                                transparencyImage.save(
                                    transparencyCapturePath),
                            "shared Workbench transparency resolve must preserve the fixture bounds");
                    }
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
                    const auto readAaSampleJitter = [&](int aaMode,
                                                        int sampleIndex,
                                                        QPointF *jitter) {
                        if (!jitter || sampleIndex < 0) return false;
                        ViewportAaRenderer aaRenderer;
                        aaRenderer.setMode(aaMode);
                        aaRenderer.setDisplayTransform(gpuSolidShading, true);
                        const QByteArray historyKey =
                            QByteArray("viewport-aa-jitter-contract-") +
                            QByteArray::number(aaMode);
                        for (int sample = 0; sample <= sampleIndex; ++sample) {
                            if (!aaRenderer.beginSceneSample(
                                    glSize, 1.0, gpuTransform, historyKey,
                                    false, true)) {
                                return false;
                            }
                            if (sample == sampleIndex) {
                                *jitter = aaRenderer.sampleCamera()
                                              .renderJitterPixels();
                            }
                            aaRenderer.finishSceneSample();
                        }
                        return true;
                    };
                    QPointF offJitter;
                    QPointF singlePassJitter;
                    QPointF multisampleFirstJitter;
                    QPointF multisampleSecondJitter;
                    const bool aaJitterContractRead =
                        readAaSampleJitter(
                            static_cast<int>(ViewportAntiAliasingMode::Off),
                            0, &offJitter) &&
                        readAaSampleJitter(
                            static_cast<int>(ViewportAntiAliasingMode::SinglePass),
                            0, &singlePassJitter) &&
                        readAaSampleJitter(
                            static_cast<int>(ViewportAntiAliasingMode::Samples8),
                            0, &multisampleFirstJitter) &&
                        readAaSampleJitter(
                            static_cast<int>(ViewportAntiAliasingMode::Samples8),
                            1, &multisampleSecondJitter);
                    passed &= check(
                        aaJitterContractRead && offJitter.isNull() &&
                            singlePassJitter.isNull() &&
                            multisampleFirstJitter.isNull() &&
                            !multisampleSecondJitter.isNull(),
                        "AA Off, Single Pass, and first multisample position are centered; later multisample positions are jittered");
                    bool interruptedAaContract = true;
                    const std::array<int, 5> multisampleModes = {
                        static_cast<int>(ViewportAntiAliasingMode::Samples5),
                        static_cast<int>(ViewportAntiAliasingMode::Samples8),
                        static_cast<int>(ViewportAntiAliasingMode::Samples11),
                        static_cast<int>(ViewportAntiAliasingMode::Samples16),
                        static_cast<int>(ViewportAntiAliasingMode::Samples32)};
                    for (const int sampleCount : multisampleModes) {
                        ViewportAaRenderer interruptedAaRenderer;
                        interruptedAaRenderer.setMode(sampleCount);
                        interruptedAaRenderer.setDisplayTransform(
                            gpuSolidShading, true);
                        const bool interruptedFirstSampleStarted =
                            interruptedAaRenderer.beginSceneSample(
                                glSize, 1.0, gpuTransform, aaBaselineKey,
                                false, true);
                        const quint64 interruptedInitialEpoch =
                            interruptedAaRenderer.targets().historyEpoch;
                        bool modeContract = interruptedFirstSampleStarted &&
                            interruptedAaRenderer.targets().sampleIndex == 0 &&
                            interruptedAaRenderer.targets().sampleCount ==
                                sampleCount &&
                            interruptedAaRenderer.sampleCamera()
                                .renderJitterPixels().isNull();
                        if (interruptedAaRenderer.sceneSampleActive()) {
                            interruptedAaRenderer.finishSceneSample();
                        }
                        const bool interruptedSecondSampleStarted =
                            interruptedAaRenderer.beginSceneSample(
                                glSize, 1.0, gpuTransform, aaBaselineKey,
                                false, true);
                        modeContract &= interruptedSecondSampleStarted &&
                            interruptedAaRenderer.targets().sampleIndex == 1 &&
                            !interruptedAaRenderer.sampleCamera()
                                 .renderJitterPixels().isNull();
                        if (interruptedAaRenderer.sceneSampleActive()) {
                            interruptedAaRenderer.finishSceneSample();
                        }

                        ViewportTransform navigatingAaCamera = gpuTransform;
                        navigatingAaCamera.setViewDirection({3.5, -6.0, 4.0});
                        const QByteArray navigatingAaHistoryKey =
                            aaBaselineKey + QByteArrayLiteral("-camera-moved");
                        const bool navigationSampleStarted =
                            interruptedAaRenderer.beginSceneSample(
                                glSize, 1.0, navigatingAaCamera,
                                navigatingAaHistoryKey, true, true);
                        const quint64 navigationAaEpoch =
                            interruptedAaRenderer.targets().historyEpoch;
                        modeContract &= navigationSampleStarted &&
                            navigationAaEpoch > interruptedInitialEpoch &&
                            interruptedAaRenderer.targets().sampleIndex == 0 &&
                            interruptedAaRenderer.targets().sampleCount == 1 &&
                            interruptedAaRenderer.sampleCamera()
                                .renderJitterPixels().isNull();
                        if (interruptedAaRenderer.sceneSampleActive()) {
                            interruptedAaRenderer.finishSceneSample();
                        }
                        const bool resumedFirstSampleStarted =
                            interruptedAaRenderer.beginSceneSample(
                                glSize, 1.0, navigatingAaCamera,
                                navigatingAaHistoryKey, false, true);
                        const quint64 resumedAaEpoch =
                            interruptedAaRenderer.targets().historyEpoch;
                        modeContract &= resumedFirstSampleStarted &&
                            resumedAaEpoch > navigationAaEpoch &&
                            interruptedAaRenderer.targets().sampleIndex == 0 &&
                            interruptedAaRenderer.targets().sampleCount ==
                                sampleCount &&
                            interruptedAaRenderer.sampleCamera()
                                .renderJitterPixels().isNull();
                        if (interruptedAaRenderer.sceneSampleActive()) {
                            interruptedAaRenderer.finishSceneSample();
                        }
                        const bool resumedSecondSampleStarted =
                            interruptedAaRenderer.beginSceneSample(
                                glSize, 1.0, navigatingAaCamera,
                                navigatingAaHistoryKey, false, true);
                        modeContract &= resumedSecondSampleStarted &&
                            interruptedAaRenderer.targets().historyEpoch ==
                                resumedAaEpoch &&
                            interruptedAaRenderer.targets().sampleIndex == 1 &&
                            interruptedAaRenderer.targets().sampleCount ==
                                sampleCount &&
                            !interruptedAaRenderer.sampleCamera()
                                 .renderJitterPixels().isNull();
                        if (interruptedAaRenderer.sceneSampleActive()) {
                            interruptedAaRenderer.finishSceneSample();
                        }
                        interruptedAaContract &= modeContract;
                    }
                    passed &= check(
                        interruptedAaContract,
                        "navigation must interrupt partial 5/8/11/16/32-sample history with one centered sample, then restart idle accumulation at centered sample zero");
                    ViewportTransform positiveYJitter = gpuTransform;
                    positiveYJitter.setRenderJitterPixels(QPointF(0.0, 0.5));
                    const QVector4D centerPoint(0.0f, 0.0f, 0.0f, 1.0f);
                    const float baseProjectionY =
                        (viewportViewProjection(gpuTransform, glSize) *
                         centerPoint).y();
                    const float jitteredProjectionY =
                        (viewportViewProjection(positiveYJitter, glSize) *
                         centerPoint).y();
                    passed &= check(
                        jitteredProjectionY > baseProjectionY,
                        "Positive Workbench sample Y must translate projection Y positively");
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
                        const QString sampleCapturePrefix =
                            aaMode == static_cast<int>(
                                          ViewportAntiAliasingMode::Samples8)
                                ? qEnvironmentVariable(
                                      "CLASSICAD_VIEWPORT_AA_SAMPLES8_SAMPLE_CAPTURE_PREFIX")
                                : QString();
                        const auto writeRgbTexturePfm = [&](const QString &path,
                                                             GLuint texture) {
                            if (path.isEmpty()) return true;
                            if (texture == 0) return false;
                            QVector<float> rgba(glSize.width() *
                                                glSize.height() * 4);
                            GLint previousActiveTexture = GL_TEXTURE0;
                            GLint previousTexture = 0;
                            functions.glGetIntegerv(
                                GL_ACTIVE_TEXTURE, &previousActiveTexture);
                            functions.glActiveTexture(GL_TEXTURE0);
                            functions.glGetIntegerv(
                                GL_TEXTURE_BINDING_2D, &previousTexture);
                            functions.glBindTexture(GL_TEXTURE_2D, texture);
                            functions.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA,
                                                    GL_FLOAT, rgba.data());
                            functions.glBindTexture(
                                GL_TEXTURE_2D,
                                static_cast<GLuint>(previousTexture));
                            functions.glActiveTexture(
                                static_cast<GLenum>(previousActiveTexture));
                            QVector<float> rgb(glSize.width() *
                                               glSize.height() * 3);
                            for (int pixel = 0;
                                 pixel < glSize.width() * glSize.height();
                                 ++pixel) {
                                rgb[pixel * 3] = rgba[pixel * 4];
                                rgb[pixel * 3 + 1] = rgba[pixel * 4 + 1];
                                rgb[pixel * 3 + 2] = rgba[pixel * 4 + 2];
                            }
                            QFile file(path);
                            const QByteArray header =
                                QByteArrayLiteral("PF\n") +
                                QByteArray::number(glSize.width()) +
                                QByteArrayLiteral(" ") +
                                QByteArray::number(glSize.height()) +
                                QByteArrayLiteral("\n-1.0\n");
                            return file.open(QIODevice::WriteOnly) &&
                                file.write(header) == header.size() &&
                                file.write(reinterpret_cast<const char *>(
                                               rgb.constData()),
                                           qsizetype(rgb.size()) *
                                               qsizetype(sizeof(float))) ==
                                    qsizetype(rgb.size()) *
                                        qsizetype(sizeof(float));
                        };
                        const auto writeDepthTexturePfm = [&](const QString &path,
                                                               GLuint texture) {
                            if (path.isEmpty()) return true;
                            if (texture == 0) return false;
                            GLint previousReadFramebuffer = 0;
                            GLint previousReadBuffer = GL_BACK;
                            functions.glGetIntegerv(
                                GL_READ_FRAMEBUFFER_BINDING,
                                &previousReadFramebuffer);
                            functions.glGetIntegerv(GL_READ_BUFFER,
                                                    &previousReadBuffer);
                            GLuint readFramebuffer = 0;
                            functions.glGenFramebuffers(1, &readFramebuffer);
                            functions.glBindFramebuffer(
                                GL_READ_FRAMEBUFFER, readFramebuffer);
                            functions.glFramebufferTexture2D(
                                GL_READ_FRAMEBUFFER,
                                GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                texture, 0);
                            const bool framebufferValid =
                                functions.glCheckFramebufferStatus(
                                    GL_READ_FRAMEBUFFER) ==
                                GL_FRAMEBUFFER_COMPLETE;
                            QVector<float> depth(glSize.width() *
                                                 glSize.height());
                            if (framebufferValid) {
                                functions.glReadBuffer(GL_NONE);
                                functions.glReadPixels(
                                    0, 0, glSize.width(), glSize.height(),
                                    GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
                                const ViewportProjectionClipRange clipRange =
                                    viewportProjectionClipRange(gpuTransform,
                                                                glSize);
                                const float nearClip =
                                    static_cast<float>(clipRange.nearPlane);
                                const float farClip =
                                    static_cast<float>(clipRange.farPlane);
                                const float cameraDepthOffset =
                                    8.0f - farClip * 0.5f;
                                for (float &value : depth) {
                                    value = !std::isfinite(value) || value >= 1.0f
                                        ? 1.0e10f
                                        : nearClip +
                                              std::clamp(value, 0.0f, 1.0f) *
                                                  (farClip - nearClip) +
                                              cameraDepthOffset;
                                }
                            }
                            functions.glBindFramebuffer(
                                GL_READ_FRAMEBUFFER,
                                static_cast<GLuint>(previousReadFramebuffer));
                            functions.glReadBuffer(
                                static_cast<GLenum>(previousReadBuffer));
                            functions.glDeleteFramebuffers(1,
                                                           &readFramebuffer);
                            if (!framebufferValid) return false;
                            QFile file(path);
                            const QByteArray header =
                                QByteArrayLiteral("Pf\n") +
                                QByteArray::number(glSize.width()) +
                                QByteArrayLiteral(" ") +
                                QByteArray::number(glSize.height()) +
                                QByteArrayLiteral("\n-1.0\n");
                            return file.open(QIODevice::WriteOnly) &&
                                file.write(header) == header.size() &&
                                file.write(reinterpret_cast<const char *>(
                                               depth.constData()),
                                           qsizetype(depth.size()) *
                                               qsizetype(sizeof(float))) ==
                                    qsizetype(depth.size()) *
                                        qsizetype(sizeof(float));
                        };
                        QByteArray sampleJitterMetadata;
                        sampleJitterMetadata += QByteArrayLiteral(
                            "# sample_index render_jitter_x_pixels render_jitter_y_pixels\n");
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
                            const bool expectsJitter = aaMode >= 5 && sample > 0;
                            const bool jitterMatchesMode = expectsJitter
                                ? !renderJitter.isNull()
                                : renderJitter.isNull();
                            if (!jitterMatchesMode) {
                                captureSucceeded = false;
                                aaRenderer.finishSceneSample();
                                break;
                            }
                            if (!sampleCapturePrefix.isEmpty()) {
                                sampleJitterMetadata +=
                                    QByteArray::number(sample) + ' ' +
                                    QByteArray::number(renderJitter.x(), 'g', 9) +
                                    ' ' +
                                    QByteArray::number(renderJitter.y(), 'g', 9) +
                                    '\n';
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
                            const ViewportWorkbenchSceneColorPassResult result =
                                ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                                    gpuSurfaceRenderer,
                                    ViewportWorkbenchScenePassPacket::fromScene(
                                        {surfaceObject}, aaRenderer.sampleCamera(),
                                        glSize, 1.0, rawFaceShading,
                                        &aaRenderer.targets(), true));
                            if (!result.rendered) {
                                captureSucceeded = false;
                                aaRenderer.finishSceneSample();
                                break;
                            }
                            aaRenderer.markCurrentSampleAttachmentsValid(
                                result.report.populatedAttachments);
                            if (!sampleCapturePrefix.isEmpty()) {
                                const ViewportRenderTargets &targets =
                                    aaRenderer.targets();
                                const QString sampleStem =
                                    sampleCapturePrefix +
                                    QStringLiteral("-sample-") +
                                    QString::number(sample);
                                captureSucceeded =
                                    writeRgbTexturePfm(
                                        sampleStem +
                                            QStringLiteral("-color.pfm"),
                                        targets.currentSampleColorTexture) &&
                                    writeDepthTexturePfm(
                                        sampleStem +
                                            QStringLiteral("-depth.pfm"),
                                        targets.depthTexture);
                            }
                            aaRenderer.finishSceneSample();
                            if (captureSucceeded &&
                                !sampleCapturePrefix.isEmpty()) {
                                const ViewportRenderTargets &targets =
                                    aaRenderer.targets();
                                const QString sampleStem =
                                    sampleCapturePrefix +
                                    QStringLiteral("-sample-") +
                                    QString::number(sample);
                                captureSucceeded = writeRgbTexturePfm(
                                    sampleStem +
                                        QStringLiteral("-history-log2-weighted.pfm"),
                                    targets.historyColorTexture) &&
                                    writeRgbTexturePfm(
                                        sampleStem +
                                            QStringLiteral("-resolved.pfm"),
                                        targets.resolvedSceneLinearColorTexture);
                            }
                            ++completedSamples;
                        }
                        bool sampleJitterMetadataSaved = true;
                        if (!sampleCapturePrefix.isEmpty() &&
                            completedSamples == expectedSamples) {
                            QFile jitterFile(sampleCapturePrefix +
                                             QStringLiteral("-jitter.txt"));
                            sampleJitterMetadataSaved =
                                jitterFile.open(QIODevice::WriteOnly) &&
                                jitterFile.write(sampleJitterMetadata) ==
                                    sampleJitterMetadata.size();
                        }
                        const QImage image = framebuffer.toImage();
                        bool linearCaptureSaved = true;
                        const char *linearCaptureEnvironmentVariable = nullptr;
                        switch (aaMode) {
                        case static_cast<int>(ViewportAntiAliasingMode::Samples5):
                            linearCaptureEnvironmentVariable =
                                "CLASSICAD_VIEWPORT_AA_SAMPLES5_LINEAR_CAPTURE";
                            break;
                        case static_cast<int>(ViewportAntiAliasingMode::Samples8):
                            linearCaptureEnvironmentVariable =
                                "CLASSICAD_VIEWPORT_AA_SAMPLES8_LINEAR_CAPTURE";
                            break;
                        case static_cast<int>(ViewportAntiAliasingMode::Samples11):
                            linearCaptureEnvironmentVariable =
                                "CLASSICAD_VIEWPORT_AA_SAMPLES11_LINEAR_CAPTURE";
                            break;
                        case static_cast<int>(ViewportAntiAliasingMode::Samples16):
                            linearCaptureEnvironmentVariable =
                                "CLASSICAD_VIEWPORT_AA_SAMPLES16_LINEAR_CAPTURE";
                            break;
                        case static_cast<int>(ViewportAntiAliasingMode::Samples32):
                            linearCaptureEnvironmentVariable =
                                "CLASSICAD_VIEWPORT_AA_SAMPLES32_LINEAR_CAPTURE";
                            break;
                        default:
                            break;
                        }
                        const QString linearCapturePath =
                            linearCaptureEnvironmentVariable
                                ? qEnvironmentVariable(
                                      linearCaptureEnvironmentVariable)
                                : QString();
                        if (!linearCapturePath.isEmpty() &&
                            completedSamples == expectedSamples) {
                            const ViewportRenderTargets &targets =
                                aaRenderer.targets();
                            const GLuint linearTexture =
                                targets.isValid(ViewportRenderTargets::
                                                    ResolvedSceneLinearColor)
                                    ? targets.resolvedSceneLinearColorTexture
                                    : 0;
                            QVector<float> rgba(glSize.width() *
                                                glSize.height() * 4);
                            GLint previousActiveTexture = GL_TEXTURE0;
                            GLint previousTexture = 0;
                            functions.glGetIntegerv(
                                GL_ACTIVE_TEXTURE, &previousActiveTexture);
                            functions.glActiveTexture(GL_TEXTURE0);
                            functions.glGetIntegerv(
                                GL_TEXTURE_BINDING_2D, &previousTexture);
                            if (linearTexture != 0) {
                                functions.glBindTexture(GL_TEXTURE_2D,
                                                        linearTexture);
                                functions.glGetTexImage(
                                    GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT,
                                    rgba.data());
                            }
                            functions.glBindTexture(
                                GL_TEXTURE_2D,
                                static_cast<GLuint>(previousTexture));
                            functions.glActiveTexture(
                                static_cast<GLenum>(previousActiveTexture));

                            QFile linearFile(linearCapturePath);
                            const QByteArray header =
                                QByteArrayLiteral("PF\n") +
                                QByteArray::number(glSize.width()) +
                                QByteArrayLiteral(" ") +
                                QByteArray::number(glSize.height()) +
                                QByteArrayLiteral("\n-1.0\n");
                            QVector<float> rgb(glSize.width() *
                                               glSize.height() * 3);
                            for (int pixel = 0;
                                 pixel < glSize.width() * glSize.height();
                                 ++pixel) {
                                rgb[pixel * 3] = rgba[pixel * 4];
                                rgb[pixel * 3 + 1] = rgba[pixel * 4 + 1];
                                rgb[pixel * 3 + 2] = rgba[pixel * 4 + 2];
                            }
                            linearCaptureSaved = linearTexture != 0 &&
                                linearFile.open(QIODevice::WriteOnly) &&
                                linearFile.write(header) == header.size() &&
                                linearFile.write(
                                    reinterpret_cast<const char *>(
                                        rgb.constData()),
                                    qsizetype(rgb.size()) * qsizetype(sizeof(float))) ==
                                    qsizetype(rgb.size()) * qsizetype(sizeof(float));
                        }
                        bool depthCaptureSaved = true;
                        const QString depthCapturePath =
                            aaMode == static_cast<int>(
                                          ViewportAntiAliasingMode::Samples8)
                                ? qEnvironmentVariable(
                                      "CLASSICAD_VIEWPORT_AA_SAMPLES8_DEPTH_CAPTURE")
                                : QString();
                        if (!depthCapturePath.isEmpty() &&
                            completedSamples == expectedSamples) {
                            QVector<float> normalizedDepth(
                                glSize.width() * glSize.height());
                            functions.glReadPixels(
                                0, 0, glSize.width(), glSize.height(),
                                GL_DEPTH_COMPONENT, GL_FLOAT,
                                normalizedDepth.data());
                            const ViewportProjectionClipRange clipRange =
                                viewportProjectionClipRange(gpuTransform,
                                                            glSize);
                            const float nearClip =
                                static_cast<float>(clipRange.nearPlane);
                            const float farClip =
                                static_cast<float>(clipRange.farPlane);
                            // The orthographic viewport's depth matrix uses
                            // an eye distance of far/2; the Blender fixture's
                            // render camera is eight units from its target.
                            const float cameraDepthOffset =
                                8.0f - farClip * 0.5f;
                            for (float &depth : normalizedDepth) {
                                depth = !std::isfinite(depth) || depth >= 1.0f
                                    ? 1.0e10f
                                    : nearClip + std::clamp(depth, 0.0f, 1.0f) *
                                          (farClip - nearClip) +
                                          cameraDepthOffset;
                            }
                            QFile depthFile(depthCapturePath);
                            const QByteArray header =
                                QByteArrayLiteral("Pf\n") +
                                QByteArray::number(glSize.width()) +
                                QByteArrayLiteral(" ") +
                                QByteArray::number(glSize.height()) +
                                QByteArrayLiteral("\n-1.0\n");
                            depthCaptureSaved =
                                depthFile.open(QIODevice::WriteOnly) &&
                                depthFile.write(header) == header.size() &&
                                depthFile.write(
                                    reinterpret_cast<const char *>(
                                        normalizedDepth.constData()),
                                    qsizetype(normalizedDepth.size()) *
                                        qsizetype(sizeof(float))) ==
                                    qsizetype(normalizedDepth.size()) *
                                        qsizetype(sizeof(float));
                        }
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
                                linearCaptureSaved &&
                                depthCaptureSaved &&
                                sampleJitterMetadataSaved &&
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
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_SAMPLES5_CAPTURE", 5, 5,
                        "5-sample baseline must resolve five nonblank outline-free cube samples");
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_SAMPLES11_CAPTURE", 11, 11,
                        "11-sample baseline must resolve eleven nonblank outline-free cube samples");
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_SAMPLES16_CAPTURE", 16, 16,
                        "16-sample baseline must resolve sixteen nonblank outline-free cube samples");
                    passed &= captureAaMode(
                        "CLASSICAD_VIEWPORT_AA_SAMPLES32_CAPTURE", 32, 32,
                        "32-sample baseline must resolve thirty-two nonblank outline-free cube samples");
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

                    const WorkbenchPackedMaterialData materialBucketA =
                        workbenchQuantizeMaterialLighting(
                            workbenchPackMaterialData(0.0f, 0.25f, 1.0f));
                    const WorkbenchPackedMaterialData materialBucketSame =
                        workbenchQuantizeMaterialLighting(
                            workbenchPackMaterialData(0.0f, 0.255f, 1.0f));
                    const WorkbenchPackedMaterialData materialBucketNext =
                        workbenchQuantizeMaterialLighting(
                            workbenchPackMaterialData(0.0f, 0.27f, 1.0f));
                    const WorkbenchPackedMaterialData metallicBucketA =
                        workbenchQuantizeMaterialLighting(
                            workbenchPackMaterialData(0.10f, 0.25f, 1.0f));
                    const WorkbenchPackedMaterialData metallicBucketSame =
                        workbenchQuantizeMaterialLighting(
                            workbenchPackMaterialData(0.14f, 0.25f, 1.0f));
                    const WorkbenchPackedMaterialData metallicBucketNext =
                        workbenchQuantizeMaterialLighting(
                            workbenchPackMaterialData(0.15f, 0.25f, 1.0f));
                    passed &= check(
                        materialBucketA.roughness ==
                                materialBucketSame.roughness &&
                            materialBucketA.roughness !=
                                materialBucketNext.roughness &&
                            metallicBucketA.metallic ==
                                metallicBucketSame.metallic &&
                            metallicBucketA.metallic !=
                                metallicBucketNext.metallic,
                        "Workbench material roughness and metallic must share their quantized buckets and change at the next boundary");
                    const auto renderMaterialBucket = [&](float roughness) {
                        ViewportRenderObject materialObject = surfaceObject;
                        materialObject.selected = false;
                        materialObject.outlineHandledBySceneStroke = false;
                        materialObject.hasWorkbenchMaterial = true;
                        materialObject.workbenchMaterial.baseColorSceneLinear =
                            QVector3D(0.42f, 0.42f, 0.42f);
                        materialObject.workbenchMaterial.roughness =
                            roughness;
                        materialObject.workbenchMaterial.metallic = 0.0;
                        ViewportShadingSettings materialShading =
                            gpuSolidShading;
                        materialShading.colorMode =
                            ViewportColorMode::Material;
                        materialShading.lightingMode =
                            ViewportLightingMode::Studio;
                        materialShading.outline = false;
                        materialShading.shadows = false;
                        materialShading.cavity = false;
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        const bool drawn = gpuSurfaceRenderer.draw(
                            {materialObject}, gpuTransform, glSize, 1.0,
                            materialShading);
                        functions.glFinish();
                        return qMakePair(drawn, framebuffer.toImage());
                    };
                    const auto roughnessBucketA = renderMaterialBucket(0.25f);
                    const auto roughnessBucketSame =
                        renderMaterialBucket(0.255f);
                    const auto roughnessBucketNext =
                        renderMaterialBucket(0.27f);
                    int roughnessBoundaryPixels = 0;
                    bool sameBucketImage =
                        roughnessBucketA.second.size() ==
                        roughnessBucketSame.second.size();
                    for (int y = 0; sameBucketImage &&
                                    y < roughnessBucketA.second.height(); ++y) {
                        for (int x = 0; x < roughnessBucketA.second.width();
                             ++x) {
                            sameBucketImage &= roughnessBucketA.second.pixel(
                                                   x, y) ==
                                               roughnessBucketSame.second.pixel(
                                                   x, y);
                        }
                    }
                    for (int y = 0; y < roughnessBucketA.second.height(); ++y) {
                        for (int x = 0; x < roughnessBucketA.second.width();
                             ++x) {
                            roughnessBoundaryPixels +=
                                roughnessBucketA.second.pixel(x, y) !=
                                roughnessBucketNext.second.pixel(x, y);
                        }
                    }
                    passed &= check(
                        roughnessBucketA.first && roughnessBucketSame.first &&
                            roughnessBucketNext.first && sameBucketImage &&
                            roughnessBoundaryPixels > 0,
                        "GPU Workbench material lighting must be identical within a 5-bit roughness bucket and change across its boundary");

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

                        ScalarVolume3D rotatedSliceVolume;
                        rotatedSliceVolume.name =
                            QStringLiteral("rotated anisotropic slice");
                        rotatedSliceVolume.activeBounds = {
                            -1, -1, -2, 0, 0, 1};
                        rotatedSliceVolume.expandedVoxelCount = 16;
                        rotatedSliceVolume.denseVoxelCount = 16;
                        rotatedSliceVolume.activeRecords = {
                            {-1, -1, -2, 2, true, 1.0f},
                            {-1, -1, 0, 2, true, 1.0f}};
                        rotatedSliceVolume.displayDensity = 1.0f;
                        rotatedSliceVolume.densityScale = 1.0f;
                        rotatedSliceVolume.useSlice = true;
                        rotatedSliceVolume.sliceAxis = QStringLiteral("X");
                        rotatedSliceVolume.sliceDepth = 0.5f;
                        rotatedSliceVolume.interpolationMethod =
                            QStringLiteral("LINEAR");
                        // A 90-degree Y rotation makes the world X AABB
                        // extent 4, while Blender's selected object X
                        // dimension remains 2. The expected opacity is
                        // 1-exp(-density*2*0.05).
                        rotatedSliceVolume.objectToWorld = {
                            0.0, 0.0, -1.0, 0.0,
                            0.0, 1.0, 0.0, 0.0,
                            1.0, 0.0, 0.0, 0.0,
                            0.0, 0.0, 0.0, 1.0};
                        ViewportRenderObject rotatedSliceObject = volumeObject;
                        rotatedSliceObject.shape.scalarVolume =
                            rotatedSliceVolume;
                        functions.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        const bool rotatedSliceDrawSucceeded =
                            gpuSurfaceRenderer.draw(
                                {rotatedSliceObject}, gpuTransform, glSize,
                                1.0, volumeShading, false, true, false,
                                &volumeTargets, nullptr, true);
                        QVector<GLfloat> rotatedSlicePixels(
                            glSize.width() * glSize.height() * 4);
                        functions.glReadPixels(
                            0, 0, glSize.width(), glSize.height(), GL_RGBA,
                            GL_FLOAT, rotatedSlicePixels.data());
                        GLfloat maximumRotatedSliceAlpha = 0.0f;
                        for (qsizetype pixel = 0;
                             pixel < qsizetype(glSize.width()) *
                                         glSize.height();
                             ++pixel) {
                            maximumRotatedSliceAlpha = std::max(
                                maximumRotatedSliceAlpha,
                                rotatedSlicePixels[pixel * 4 + 3]);
                        }
                        gpuRotatedVolumeSliceOpacityMatches =
                            rotatedSliceDrawSucceeded &&
                            std::abs(maximumRotatedSliceAlpha -
                                     (1.0f - std::exp(-0.1f))) < 0.015f;
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

                    QOpenGLFramebufferObjectFormat sceneLinearStrokeFormat;
                    sceneLinearStrokeFormat.setAttachment(
                        QOpenGLFramebufferObject::CombinedDepthStencil);
                    sceneLinearStrokeFormat.setInternalTextureFormat(
                        GL_RGBA16F);
                    QOpenGLFramebufferObject sceneLinearStrokeFramebuffer(
                        glSize, sceneLinearStrokeFormat);
                    ViewportSceneStroke committedCurveStroke = editWire;
                    committedCurveStroke.color = QColor(220, 30, 10, 128);
                    committedCurveStroke.width = 5.0f;
                    committedCurveStroke.editModeWire = false;
                    committedCurveStroke.writeDepth = false;
                    bool sceneLinearStrokeRendered = false;
                    bool depthReplayPreservedColor = false;
                    bool depthReplayMatchesLegacy = false;
                    if (sceneLinearStrokeFramebuffer.isValid()) {
                        sceneLinearStrokeFramebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        functions.glDisable(GL_FRAMEBUFFER_SRGB);
                        functions.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        functions.glClearDepth(1.0);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        sceneLinearStrokeRendered = linePointsValid &&
                            wireRenderer.draw(
                                {committedCurveStroke}, wireTransform, glSize,
                                1.0, true, 1.0, true, true, true);
                        std::array<float, 4> sceneLinearStrokePixel{};
                        float sceneDepthBeforeReplay = 0.0f;
                        functions.glReadPixels(
                            300, 239, 1, 1, GL_RGBA, GL_FLOAT,
                            sceneLinearStrokePixel.data());
                        functions.glReadPixels(300, 239, 1, 1,
                                               GL_DEPTH_COMPONENT, GL_FLOAT,
                                               &sceneDepthBeforeReplay);
                        const bool sceneLinearStrokeColorValid =
                            sceneLinearStrokePixel[0] > 0.25f &&
                            sceneLinearStrokePixel[0] < 0.5f &&
                            sceneLinearStrokePixel[1] < 0.02f &&
                            sceneLinearStrokePixel[3] > 0.45f &&
                            sceneLinearStrokePixel[3] < 0.55f;
                        const bool depthUnchangedByLinearColor =
                            sceneDepthBeforeReplay > 0.999f;

                        ViewportSceneStroke depthStroke =
                            committedCurveStroke;
                        depthStroke.writeDepth = true;
                        const bool depthReplayRendered = wireRenderer.drawDepthOnly(
                            {depthStroke}, wireTransform, glSize, 1.0,
                            true, true);
                        std::array<float, 4> pixelAfterDepthReplay{};
                        float sceneDepthAfterReplay = 1.0f;
                        functions.glReadPixels(
                            300, 239, 1, 1, GL_RGBA, GL_FLOAT,
                            pixelAfterDepthReplay.data());
                        functions.glReadPixels(300, 239, 1, 1,
                                               GL_DEPTH_COMPONENT, GL_FLOAT,
                                               &sceneDepthAfterReplay);
                        depthReplayPreservedColor = depthReplayRendered &&
                            std::abs(pixelAfterDepthReplay[0] -
                                     sceneLinearStrokePixel[0]) < 1.0e-4f &&
                            std::abs(pixelAfterDepthReplay[1] -
                                     sceneLinearStrokePixel[1]) < 1.0e-4f &&
                            std::abs(pixelAfterDepthReplay[3] -
                                     sceneLinearStrokePixel[3]) < 1.0e-4f;
                        depthReplayMatchesLegacy =
                            sceneDepthAfterReplay < 0.999f &&
                            sceneDepthBeforeReplay > 0.999f;
                        passed &= check(sceneLinearStrokeRendered &&
                                            sceneLinearStrokeColorValid &&
                                            depthUnchangedByLinearColor &&
                                            depthReplayPreservedColor &&
                                            depthReplayMatchesLegacy,
                                        "committed curve strokes must enter scene-linear Combined once, leave sample depth untouched, and preserve legacy depth with a color-masked replay");
                    } else {
                        passed &= check(false,
                                        "scene-linear stroke contract must allocate an RGBA16F depth framebuffer");
                    }

                    ViewportSceneStroke displayResultStroke =
                        committedCurveStroke;
                    displayResultStroke.writeDepth = true;
                    wireFramebuffer.bind();
                    functions.glViewport(0, 0, glSize.width(), glSize.height());
                    functions.glDisable(GL_FRAMEBUFFER_SRGB);
                    functions.glClearColor(128.0f / 255.0f,
                                           128.0f / 255.0f,
                                           128.0f / 255.0f, 1.0f);
                    functions.glClearDepth(1.0);
                    functions.glClear(GL_COLOR_BUFFER_BIT |
                                      GL_DEPTH_BUFFER_BIT);
                    const bool displayStrokeRenderedOnce = wireRenderer.draw(
                        {displayResultStroke}, wireTransform, glSize, 1.0,
                        true, 1.0, true, true);
                    unsigned char displayStrokeOnce[4] = {};
                    functions.glReadPixels(300, 239, 1, 1, GL_RGBA,
                                           GL_UNSIGNED_BYTE,
                                           displayStrokeOnce);
                    const bool displayStrokeRenderedTwice = wireRenderer.draw(
                        {displayResultStroke}, wireTransform, glSize, 1.0,
                        true, 1.0, true, true);
                    unsigned char displayStrokeTwice[4] = {};
                    functions.glReadPixels(300, 239, 1, 1, GL_RGBA,
                                           GL_UNSIGNED_BYTE,
                                           displayStrokeTwice);
                    passed &= check(displayStrokeRenderedOnce &&
                                        displayStrokeRenderedTwice &&
                                        displayStrokeOnce[0] >
                                            displayStrokeOnce[1] + 40 &&
                                        displayStrokeTwice[0] >
                                            displayStrokeOnce[0] + 4,
                                    "the display Combined stroke path must produce one translucent curve overlay rather than a double draw");

                    const auto captureSmoothWireCategory = [&](
                        ViewportSmoothWireCategory category,
                        bool editModeStyle,
                        bool smoothOverlay,
                        bool smoothEditMode,
                        bool *drawSucceeded) {
                        wireFramebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        functions.glDisable(GL_FRAMEBUFFER_SRGB);
                        functions.glClearColor(128.0f / 255.0f,
                                               128.0f / 255.0f,
                                               128.0f / 255.0f, 1.0f);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        ViewportSceneStroke stroke;
                        stroke.color = Qt::black;
                        stroke.width = 1.0f;
                        stroke.editModeWire = editModeStyle;
                        stroke.smoothWireCategory = category;
                        stroke.preparedDepthGeometry =
                            editWire.preparedDepthGeometry;
                        *drawSucceeded = linePointsValid &&
                            wireRenderer.draw(
                                {stroke}, wireTransform, glSize, 1.0,
                                false, 1.0, smoothOverlay, smoothEditMode);
                        functions.glFinish();
                        return wireFramebuffer.toImage();
                    };
                    std::array<QImage, 4> overlayPreferenceImages;
                    std::array<QImage, 4> editModePreferenceImages;
                    std::array<bool, 4> overlayDrawSucceeded{};
                    std::array<bool, 4> editModeDrawSucceeded{};
                    for (int overlay = 0; overlay < 2; ++overlay) {
                        for (int edit = 0; edit < 2; ++edit) {
                            const int index = overlay * 2 + edit;
                            overlayPreferenceImages[index] =
                                captureSmoothWireCategory(
                                    ViewportSmoothWireCategory::Overlay,
                                    false, overlay != 0, edit != 0,
                                    &overlayDrawSucceeded[index]);
                            editModePreferenceImages[index] =
                                captureSmoothWireCategory(
                                    ViewportSmoothWireCategory::EditMode,
                                    true, overlay != 0, edit != 0,
                                    &editModeDrawSucceeded[index]);
                        }
                    }
                    const auto imageDifferenceCount = [](const QImage &first,
                                                         const QImage &second) {
                        if (first.isNull() || second.isNull() ||
                            first.size() != second.size()) {
                            return -1;
                        }
                        int changed = 0;
                        for (int y = 0; y < first.height(); ++y) {
                            for (int x = 0; x < first.width(); ++x) {
                                changed += first.pixel(x, y) !=
                                           second.pixel(x, y);
                            }
                        }
                        return changed;
                    };
                    const bool overlayPreferenceIsolated =
                        overlayDrawSucceeded[0] && overlayDrawSucceeded[1] &&
                        overlayDrawSucceeded[2] && overlayDrawSucceeded[3] &&
                        imageDifferenceCount(overlayPreferenceImages[0],
                                             overlayPreferenceImages[1]) == 0 &&
                        imageDifferenceCount(overlayPreferenceImages[2],
                                             overlayPreferenceImages[3]) == 0 &&
                        imageDifferenceCount(overlayPreferenceImages[0],
                                             overlayPreferenceImages[2]) > 0 &&
                        imageDifferenceCount(overlayPreferenceImages[1],
                                             overlayPreferenceImages[3]) > 0;
                    const bool editModePreferenceIsolated =
                        editModeDrawSucceeded[0] && editModeDrawSucceeded[1] &&
                        editModeDrawSucceeded[2] && editModeDrawSucceeded[3] &&
                        imageDifferenceCount(editModePreferenceImages[0],
                                             editModePreferenceImages[2]) == 0 &&
                        imageDifferenceCount(editModePreferenceImages[1],
                                             editModePreferenceImages[3]) == 0 &&
                        imageDifferenceCount(editModePreferenceImages[0],
                                             editModePreferenceImages[1]) > 0 &&
                        imageDifferenceCount(editModePreferenceImages[2],
                                             editModePreferenceImages[3]) > 0;
                    passed &= check(overlayPreferenceIsolated &&
                                        editModePreferenceIsolated,
                                    "the four Smooth Wires combinations must affect only their matching Overlay or Edit Mode stroke category");

                    wireFramebuffer.bind();
                    functions.glViewport(0, 0, glSize.width(),
                                         glSize.height());
                    functions.glDisable(GL_FRAMEBUFFER_SRGB);
                    functions.glClearColor(128.0f / 255.0f,
                                           128.0f / 255.0f,
                                           128.0f / 255.0f, 1.0f);
                    functions.glClear(GL_COLOR_BUFFER_BIT |
                                      GL_DEPTH_BUFFER_BIT);
                    BlenderGridRenderer smoothWireGridRenderer;
                    const bool gridDrawSucceeded =
                        smoothWireGridRenderer.renderToCurrentFramebuffer(
                            wireTransform, glSize, 1.0, {}, 1.0,
                            BlenderGridAppearance{});
                    functions.glFinish();
                    const QImage gridImage = wireFramebuffer.toImage();
                    int partiallyCoveredGridPixels = 0;
                    if (!gridImage.isNull()) {
                        for (int y = 0; y < gridImage.height(); ++y) {
                            for (int x = 0; x < gridImage.width(); ++x) {
                                const QColor pixel = gridImage.pixelColor(x, y);
                                if (pixel.red() == pixel.green() &&
                                    pixel.green() == pixel.blue() &&
                                    pixel.red() > 105 &&
                                    pixel.red() < 128) {
                                    ++partiallyCoveredGridPixels;
                                }
                            }
                        }
                    }
                    passed &= check(gridDrawSucceeded &&
                                        partiallyCoveredGridPixels > 0,
                                    "grid lines must retain multisample edge coverage independently from Smooth Wires preferences");

                    const auto captureGridAndOverlayStroke = [&] (
                        bool smoothOverlay, bool *drawSucceeded) {
                        wireFramebuffer.bind();
                        functions.glViewport(0, 0, glSize.width(),
                                             glSize.height());
                        functions.glDisable(GL_FRAMEBUFFER_SRGB);
                        functions.glClearColor(128.0f / 255.0f,
                                               128.0f / 255.0f,
                                               128.0f / 255.0f, 1.0f);
                        functions.glClearDepth(1.0);
                        functions.glClear(GL_COLOR_BUFFER_BIT |
                                          GL_DEPTH_BUFFER_BIT);
                        BlenderGridRenderer gridRenderer;
                        const bool gridRendered =
                            gridRenderer.renderToCurrentFramebuffer(
                                wireTransform, glSize, 1.0, {}, 1.0,
                                BlenderGridAppearance{});
                        wireFramebuffer.bind();
                        ViewportSceneStroke overlayStroke = editWire;
                        overlayStroke.editModeWire = false;
                        overlayStroke.smoothWireCategory =
                            ViewportSmoothWireCategory::Overlay;
                        const bool strokeRendered = gridRendered &&
                            wireRenderer.draw(
                                {overlayStroke}, wireTransform, glSize, 1.0,
                                false, 1.0, smoothOverlay, true);
                        functions.glFinish();
                        *drawSucceeded = gridRendered && strokeRendered;
                        return wireFramebuffer.toImage();
                    };
                    bool gridAndOverlayUnsmoothSucceeded = false;
                    bool gridAndOverlaySmoothSucceeded = false;
                    const QImage gridAndOverlayUnsmooth =
                        captureGridAndOverlayStroke(
                            false, &gridAndOverlayUnsmoothSucceeded);
                    const QImage gridAndOverlaySmooth =
                        captureGridAndOverlayStroke(
                            true, &gridAndOverlaySmoothSucceeded);
                    int partialGridPixelsOutsideStroke = 0;
                    bool gridCoverageUnaffectedByWirePreference = true;
                    if (!gridAndOverlayUnsmooth.isNull() &&
                        !gridAndOverlaySmooth.isNull() &&
                        gridAndOverlayUnsmooth.size() ==
                            gridAndOverlaySmooth.size()) {
                        for (int y = 0; y < gridAndOverlayUnsmooth.height(); ++y) {
                            if (std::abs(y - 240) <= 6) continue;
                            for (int x = 0;
                                 x < gridAndOverlayUnsmooth.width(); ++x) {
                                const QColor pixel =
                                    gridImage.pixelColor(x, y);
                                if (pixel.red() == pixel.green() &&
                                    pixel.green() == pixel.blue() &&
                                    pixel.red() > 105 && pixel.red() < 128) {
                                    ++partialGridPixelsOutsideStroke;
                                    gridCoverageUnaffectedByWirePreference &=
                                        gridAndOverlayUnsmooth.pixel(x, y) ==
                                        gridAndOverlaySmooth.pixel(x, y);
                                }
                            }
                        }
                    }
                    passed &= check(
                        gridAndOverlayUnsmoothSucceeded &&
                            gridAndOverlaySmoothSucceeded &&
                            imageDifferenceCount(gridAndOverlayUnsmooth,
                                                 gridAndOverlaySmooth) > 0 &&
                            partialGridPixelsOutsideStroke > 0 &&
                            gridCoverageUnaffectedByWirePreference,
                        "composed grid antialiasing must remain unchanged when Overlay Smooth Wires toggles, while the overlay stroke responds");

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
                                gpuWorkbenchOutlineCaptureSucceeded &&
                                gpuWorkbenchShadowCaptureSucceeded &&
                                gpuOutlineRendered &&
                                gpuSceneStrokeSuppressesPostOutline &&
                                gpuBottomDrawSucceeded &&
                                gpuBackfaceCullingKeepsOutwardFaces &&
                                gpuShadowDrawSucceeded &&
                                !gpuShadowImage.isNull() &&
                                gpuUnselectedBaselineDrawSucceeded &&
                                gpuShadowDarkenedPixels > 0,
                            "OpenGL NURBS cubes must match Blender face colors, show outlines, retain outward bottom faces under culling, and render the Shadow option");
            passed &= check(gpuSceneLinearOutlineColorPreserved,
                            "scene-linear Workbench output must convert the outline color from sRGB before blending");
            passed &= check(gpuCavityDrawSucceeded &&
                                gpuCavityImage.size() == glSize &&
                                gpuCavityBaselineImage.size() == glSize &&
                                gpuCavityChangedPixels > 0,
                            "GPU Workbench world cavity must change pixels from the matching outline-free cube baseline");
            passed &= check(
                !gpuCavityDrawSucceeded || gpuNamedPrepassReportValid,
                "GPU cavity fixture must report populated opaque depth, view-normal, and object-ID prepasses and leave front-domain pass bits clear when no front geometry is present");
            passed &= check(gpuVolumeDrawSucceeded && gpuVolumePassReported,
                            "a bounded scalar volume must complete the GPU volume pass");
            passed &= check(gpuVolumeChangedPixels > 100,
                            "a bounded scalar volume must change offscreen framebuffer pixels");
            passed &= check(gpuLowDensityVolumePreserved,
                            "low-density volume extinction must remain nonzero in the float target");
            passed &= check(gpuSignedVolumeScatteringPreserved,
                            "negative scalar density must retain Blender's signed scattering");
            passed &= check(
                gpuRotatedVolumeSliceOpacityMatches,
                "GPU scalar-volume slice opacity must use the selected object-axis extent without rotating it into a world AABB");
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
