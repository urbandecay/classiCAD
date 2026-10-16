/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"
#include "viewport_render_frame.h"
#include "viewport_raster_workbench_linear.h"
#include "viewport_shading.h"

#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QVector3D>

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace classiCAD {

// Positive camera-space depth used by the QPainter Workbench effects. The
// origin is the perspective eye or the orthographic view target.
float rasterWorkbenchCameraDepth(const QVector3D &position,
                                 const Point3D &cameraPosition,
                                 const Point3D &viewDirection) noexcept;

// Project the configured cavity distance into pixels without an artificial
// minimum or upper radius clamp.
float rasterWorkbenchCavityRadius(float cavityDistance,
                                  float pixelsPerWorldUnit,
                                  float devicePixelRatio) noexcept;

// QPainter SSAO sample step, with perspective footprint scaled from the view
// target depth to the receiver depth. Orthographic views retain the base step.
float rasterWorkbenchCavityStep(float baseRadiusPixels,
                                const QVector3D &receiverPosition,
                                const Point3D &cameraPosition,
                                const Point3D &viewTarget,
                                const Point3D &viewDirection,
                                bool perspective) noexcept;

// Keep the QPainter SSAO work bounded while honoring Blender's 1..500
// requested-sample range whenever the covered target fits the CPU budget.
int rasterWorkbenchCavitySampleCount(int requestedSamples,
                                     qsizetype coveredPixels) noexcept;

// Match GL_NEAREST sampling of normalized screen coordinates expressed in
// top-down pixel space. The texture clamps exact viewport-edge coordinates.
QPoint rasterWorkbenchNearestCavityPixel(QPointF samplePixel,
                                         QSize pixelSize) noexcept;

// Blender's opaque Workbench prepass stores the facing-adjusted view normal
// with normal_encode in an RG16F target, then effects decode that value.
QVector3D rasterWorkbenchPrepassNormal(const QVector3D &worldNormal,
                                       bool frontFacing,
                                       const QVector3D &viewRight,
                                       const QVector3D &viewUp,
                                       const QVector3D &viewFacing) noexcept;

// Map a normalized circular sample through Workbench's polygon aperture,
// rotation, and X-axis aperture-ratio scale.
QPointF rasterWorkbenchApertureTap(QPointF tap, int apertureBlades,
                                  qreal apertureRotationRadians,
                                  qreal apertureRatio) noexcept;

struct RasterWorkbenchDofTap {
    QPointF position;
    float radius = 0.0f;
};

// Blender 5.2.2's radius-three square-ring sample pattern, mapped to a disk
// and shaped by the active aperture settings. Radius remains unshaped, as in
// the GPU sample buffer's z component.
std::array<RasterWorkbenchDofTap, 49> rasterWorkbenchDofKernel(
    int apertureBlades, qreal apertureRotationRadians,
    qreal apertureRatio) noexcept;

// Blender 5.2.2's per-pixel DOF gather-origin jitter, mapped to the
// QPainter target's top-down pixel coordinates with temporal offset zero.
QPointF rasterWorkbenchDofGatherOriginOffset(
    int x, int y, int targetWidth, int targetHeight,
    float centerCocPixels) noexcept;

struct RasterWorkbenchDofPrefilter {
    float nearCoc = 0.0f;
    float farCoc = 0.0f;
    std::array<float, 4> colorWeights{};
};

struct RasterWorkbenchCavityTap {
    int receiverX = 0;
    int receiverY = 0;
    int requestedSamples = 0;
    int effectiveSamples = 0;
    int tap = 0;
    int sampleIndex = 0;
    int sampleX = 0;
    int sampleY = 0;
    float samplePixelX = 0.0f;
    float samplePixelY = 0.0f;
    bool inBounds = false;
    quint64 neighborObjectId = 0;
    float distance = 0.0f;
    float cavityDot = 0.0f;
    float edgeDot = 0.0f;
    float bias = 0.0f;
    float attenuation = 0.0f;
    float cavityContribution = 0.0f;
    float edgeContribution = 0.0f;
};

// Opt-in tap trace for a small caller-selected set of receiver pixels.
struct RasterWorkbenchCavityTapCapture {
    QVector<QPoint> receiverPixels;
    std::vector<RasterWorkbenchCavityTap> taps;
};

// Blender's 2x2 CoC-aware prefilter chooses far weights when available, else
// near weights, and attenuates samples whose CoC differs by one pixel or more.
RasterWorkbenchDofPrefilter rasterWorkbenchDofPrefilter2x2(
    const std::array<float, 4> &signedCocs) noexcept;
// Mip downsampling keeps near and far CoC channels independently rather than
// collapsing each texel back to one signed value.
RasterWorkbenchDofPrefilter rasterWorkbenchDofPrefilter2x2(
    const std::array<float, 4> &nearCocs,
    const std::array<float, 4> &farCocs) noexcept;

// Blender's gather kernel's radial CoC weight for one aperture tap.
float rasterWorkbenchDofGatherWeight(float sampleCoc, float maximumCoc,
                                     float sampleRadius) noexcept;

// Blender Workbench's adaptive mip level for one DOF gather sample. The
// returned level is clamped to the three-level half-resolution pyramid.
float rasterWorkbenchDofMipLod(float sampleCoc, float centerCoc) noexcept;

// Keep the QPainter fallback's 49-tap gather within a fixed total tap budget.
int rasterWorkbenchDofTapCountForBudget(quint64 candidatePixels) noexcept;

// Blender Workbench's DOF resolve uses smoothstep(1, 3, abs(CoC)) to blend
// its gathered bokeh image over the sharp source.
float rasterWorkbenchDofResolveBlend(float cocPixels) noexcept;

// Blender Workbench's shadow focus/shift remap for one opaque receiver. The
// software shadow map supplies the binary shadowed state.
float rasterWorkbenchShadowFactor(const QVector3D &normal,
                                 const QVector3D &lightDirection,
                                 float intensity, float shift, float focus,
                                 bool shadowed) noexcept;

// Scale the QPainter shadow-map comparison tolerance to caster depth range
// while retaining headroom for float depth quantization.
float rasterWorkbenchShadowDepthBias(float minimumLightDepth,
                                     float maximumLightDepth) noexcept;

// Supersample the QPainter light-depth map when a 2x-per-axis map stays under
// its fixed pixel budget; otherwise retain the viewport-sized fallback.
QSize rasterWorkbenchShadowMapSize(const QSize &viewportSize) noexcept;

// Blender Workbench's DOF cleanup takes a component-wise median over a 3x3
// neighborhood of gathered RGBA samples.
QRgb rasterWorkbenchDofMedian3x3(
    const std::array<QRgb, 9> &samples) noexcept;

// Blender Workbench's signed screen-curvature response from four cardinal
// view-space normal components and their object-ID outline mask.
float rasterWorkbenchCurvatureResponse(quint64 objectUp, quint64 objectDown,
                                      quint64 objectRight, quint64 objectLeft,
                                      float normalUp, float normalDown,
                                      float normalRight, float normalLeft,
                                      float ridgeFactor,
                                      float valleyFactor) noexcept;

// Retained surface layers let the QPainter cavity approximation shade the
// opaque receiver before applying the already resolved transparent surface
// contribution, as Blender does with its opaque and transparent passes.
struct RasterWorkbenchSurfaceLayers {
    QImage opaqueColor;
    QImage transparentColor;
    // Physical-target camera depth of the nearest regular opaque surface.
    // Negative infinity marks uncovered pixels. Retained for QPainter strokes
    // that must depth-test against the already composited surface pass.
    QVector<float> opaqueSceneDepth;
    QSize opaqueSceneDepthSize;
    // Physical-target depth of the nearest regular opaque or transparent
    // surface. Negative infinity marks uncovered pixels. Used by committed
    // QPainter curve strokes after the regular transparent resolve.
    QVector<float> regularSceneDepth;
    QSize regularSceneDepthSize;
    // Physical-target depth of the nearest in-front opaque or transparent
    // surface. Strokes belonging to showInFront objects test against this
    // depth domain while remaining visible over regular scene geometry.
    QVector<float> inFrontSceneDepth;
    QSize inFrontSceneDepthSize;
    // Optional pre-display resolved scene for the bounded weighted-compositor
    // outline path. Pixels are row-major physical-target samples.
    std::vector<RasterWorkbenchLinearPixel> sceneLinearColor;
    // Opaque and transparent contributions used to apply the QPainter shadow
    // factor before a deferred scene-linear outline.
    std::vector<RasterWorkbenchLinearPixel> sceneLinearOpaqueColor;
    // Retain regular and in-front opaque contributions separately so their
    // receiver shadow factors remain independent at partially covered edges.
    std::vector<RasterWorkbenchLinearPixel> sceneLinearRegularOpaqueColor;
    std::vector<RasterWorkbenchLinearPixel> sceneLinearInFrontOpaqueColor;
    std::vector<RasterWorkbenchLinearPixel> sceneLinearTransparentColor;
    // Weighted transparency with Blender's per-fragment normal shadow factor
    // already applied. Accumulated in per-pixel buffers so dense scenes do not
    // need a scene-wide retained fragment list.
    std::vector<RasterWorkbenchLinearPixel>
        sceneLinearShadowedTransparentColor;
    bool sceneLinearShadowedTransparentColorAvailable = false;
    std::vector<float> sceneLinearOutlineCoverage;
    QSize sceneLinearSize;
    // True when cavity was folded into the opaque contribution before
    // transparency composition. The later effects pass then skips cavity.
    bool cavityAppliedBeforeTransparency = false;
    // True when outline color was composed into the scene-linear target
    // before its display transform.
    bool outlineAppliedBeforeDisplay = false;
    // True when a shadow-enabled scene-linear outline mask awaits the shadow
    // or depth-of-field resolve, after which the combined result is
    // display-transformed once.
    bool sceneLinearOutlinePending = false;
    bool sceneLinearDofPending = false;
};

// Software Workbench screen-space effects for the QPainter fallback. The color
// image is shaded first; this pass resolves a scene-wide depth/normal/object-ID
// buffer from NURBS display triangles and imported polygon-mesh triangles.
void applyRasterWorkbenchEffects(QImage *colorImage,
                                 const QVector<ViewportRenderObject> &objects,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 qreal devicePixelRatio,
                                 const ViewportShadingSettings &settings,
                                 RasterWorkbenchSurfaceLayers *surfaceLayers = nullptr,
                                 std::vector<float> *cavityFactors = nullptr,
                                 RasterWorkbenchCavityTapCapture *cavityTapCapture = nullptr,
                                 const QVector<float> *committedCurveDepth = nullptr,
                                 const QVector<float> *committedInFrontCurveDepth = nullptr,
                                 const QSize &committedCurveDepthSize = {},
                                 qreal cameraFrameSensorScale = 1.0,
                                 bool smoothWiresOverlay = true,
                                 bool sceneCameraView = true,
                                 qreal themeOutlineWidth = 1.0);

// Render surface objects into temporary color layers, use their display
// tessellation to depth-test opaque coverage, then accumulate visible partial
// alpha layers and resolve once into the QPainter scene image. This is a
// software fallback compositor, not Blender's weighted OIT implementation.
bool compositeRasterWorkbenchSurfaces(
    QImage *colorImage,
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    bool antialias,
    bool useWeightedOit,
    const ViewportShadingSettings &shadingSettings,
    const std::optional<QVector3D> &sceneLinearBackground,
    const std::function<void(QPainter &, const ViewportRenderObject &,
                             RasterWorkbenchFragmentSink *)> &drawObject,
    RasterWorkbenchSurfaceLayers *surfaceLayers = nullptr,
    const std::vector<QVector3D> &sceneLinearBackgroundPixels = {},
    bool allowFragmentGeneratedDepth = false,
    const QVector<ViewportRenderObject> &scalarVolumeObjects = {},
    std::vector<RasterWorkbenchLinearPixel> *sceneLinearCapture = nullptr,
    std::vector<float> *cavityFactorCapture = nullptr,
    RasterWorkbenchCavityTapCapture *cavityTapCapture = nullptr);

} // namespace classiCAD
