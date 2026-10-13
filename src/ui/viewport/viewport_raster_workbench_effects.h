/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"
#include "viewport_render_frame.h"
#include "viewport_raster_workbench_linear.h"
#include "viewport_shading.h"

#include <QImage>
#include <QPainter>
#include <QVector3D>

#include <functional>
#include <optional>
#include <vector>

namespace classiCAD {

// Positive camera-space depth used by the QPainter Workbench effects. The
// origin is the perspective eye or the orthographic view target.
float rasterWorkbenchCameraDepth(const QVector3D &position,
                                 const Point3D &cameraPosition,
                                 const Point3D &viewDirection) noexcept;

// QPainter SSAO sample step, with perspective footprint scaled from the view
// target depth to the receiver depth. Orthographic views retain the base step.
float rasterWorkbenchCavityStep(float baseRadiusPixels,
                                const QVector3D &receiverPosition,
                                const Point3D &cameraPosition,
                                const Point3D &viewTarget,
                                const Point3D &viewDirection,
                                bool perspective) noexcept;

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
    // True when cavity was folded into the opaque contribution before
    // transparency composition. The later effects pass then skips cavity.
    bool cavityAppliedBeforeTransparency = false;
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
                                 const RasterWorkbenchSurfaceLayers *surfaceLayers = nullptr,
                                 std::vector<float> *cavityFactors = nullptr);

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
    const std::vector<QVector3D> &sceneLinearBackgroundPixels = {});

} // namespace classiCAD
