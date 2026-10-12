/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "blender_grid_renderer.h"
#include "viewport_control_point_renderer.h"
#include "viewport_scene_renderer.h"
#include "viewport_surface_renderer.h"
#include "viewport_aa_renderer.h"

#include <QOpenGLWidget>
#include <QVector>

#include <functional>
#include <memory>

class QPainter;

namespace classiCAD {

// Presents the viewport in Qt's on-screen OpenGL framebuffer. The owning
// widget retains input/tool state and the raster fallback.
class ViewportGpuSurface final : public QOpenGLWidget {
public:
    using DrawCallback = std::function<void(QPainter &, BlenderGridRenderer &,
                                            ViewportAaRenderer &,
                                            ViewportSceneRenderer &,
                                            ViewportSceneRenderer &,
                                            ViewportControlPointRenderer &,
                                            ViewportSurfaceRenderer &)>;
    using LegacyDrawCallback = std::function<void(
        QPainter &, BlenderGridRenderer &, ViewportSceneRenderer &,
        ViewportSceneRenderer &, ViewportControlPointRenderer &,
        ViewportSurfaceRenderer &)>;

    explicit ViewportGpuSurface(QWidget *parent = nullptr);
    ~ViewportGpuSurface() override;

    static bool isSupported();
    void setDrawCallback(DrawCallback callback);
    // Keep source compatibility for renderer clients that do not use the AA
    // renderer supplied to the newer callback form.
    void setDrawCallback(LegacyDrawCallback callback);
    void setAntiAliasingMode(int mode);
    // Source-compatibility wrapper for the previous widget API.
    void setAntiAliasingSamples(int samples);
    void setSurfaceTessellationCache(
        const SurfaceTessellationCache *surfaceTessellationCache);
    using OffscreenDrawCallback = std::function<bool(
        const QSize &, ViewportAaRenderer &, BlenderGridRenderer &,
        ViewportSceneRenderer &, ViewportSurfaceRenderer &)>;
    using OffscreenOverlayCallback = std::function<bool(
        const QSize &, ViewportSceneRenderer &)>;
    bool renderOffscreen(const QSize &pixelSize,
                         const ViewportTransform &camera,
                         const OffscreenDrawCallback &draw,
                         const OffscreenOverlayCallback &drawOverlay,
                         QImage *combined,
                         QVector<float> *depth,
                         QString *errorMessage = nullptr,
                         QVector<float> *sceneLinearRgba = nullptr,
                         int antiAliasingMode = -1,
                         bool transparentFilm = false,
                         bool transparentBackground = false);
    bool pickScenePoint(const QPointF &screenPosition,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        const QVector<ViewportRenderObject> &visibleSceneShapes,
                        Point3D *worldPoint);
    bool pickScenePoint(const QPointF &screenPosition,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        const QVector<Shape> &visibleSceneShapes,
                        Point3D *worldPoint);
    bool pickComponentElement(
        const QPointF &screenPosition,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        const QVector<ViewportRenderObject> &visibleSceneShapes,
        const QVector<ViewportComponentPickCandidate> &candidates,
        int *candidateIndex);
    bool pickComponentElementsInRect(
        const QRectF &selectionRect,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        const QVector<ViewportRenderObject> &visibleSceneShapes,
        const QVector<ViewportComponentPickCandidate> &candidates,
        bool depthTest,
        QVector<int> *candidateIndices);

protected:
    void paintGL() override;

private:
    DrawCallback drawCallback_;
    std::unique_ptr<BlenderGridRenderer> gridRenderer_;
    std::unique_ptr<ViewportAaRenderer> aaRenderer_;
    std::unique_ptr<ViewportSceneRenderer> sceneRenderer_;
    std::unique_ptr<ViewportSceneRenderer> previewRenderer_;
    std::unique_ptr<ViewportControlPointRenderer> controlPointRenderer_;
    std::unique_ptr<ViewportSurfaceRenderer> surfaceRenderer_;
    quint64 offscreenRenderSerial_ = 0;
};

} // namespace classiCAD
