/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 *
 * Adapted from Blender's 3D View grid shaders and grid draw setup.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "blender_grid_appearance.h"
#include "viewport_theme_background.h"
#include "services/sampling/surface_tessellation_cache.h"
#include "services/viewport/viewport_transform.h"
#include "viewport_depth_geometry.h"

#include <QOpenGLBuffer>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLTextureBlitter>
#include <QOpenGLVertexArrayObject>
#include <QRectF>
#include <QSurfaceFormat>
#include <QSize>
#include <QMatrix4x4>
#include <QVector3D>

#include <memory>

namespace classiCAD {

struct ViewportComponentPickCandidate {
    Point3D first;
    Point3D second;
    int componentIndex = -1;
    int shapeIndex = -1;
    bool edge = false;
};

QMatrix4x4 viewportViewProjection(const ViewportTransform &transform,
                                   const QSize &viewportSize);

struct ViewportProjectionClipRange {
    qreal nearPlane = 0.01;
    qreal farPlane = 1000.0;
};

// The depth readback decoder must use the same clip range as the scene matrix.
ViewportProjectionClipRange viewportProjectionClipRange(
    const ViewportTransform &transform, const QSize &viewportSize);

// Blender's view-dependent depth offset for its regular mesh-wire overlay.
qreal viewportBlenderMeshWireNdcOffsetFactor(
    const ViewportTransform &transform, const QSize &viewportSize);

class BlenderGridRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~BlenderGridRenderer();

    QImage render(const ViewportTransform &transform,
                  const QSize &viewportSize,
                  qreal devicePixelRatio,
                  const QVector<ViewportRenderObject> &visibleSceneShapes,
                  qreal baseGridStep,
                  const BlenderGridAppearance &appearance);
    // Called while a QOpenGLWidget framebuffer is current. The grid remains
    // GPU resident and is composited over the scene already drawn there.
    bool renderToCurrentFramebuffer(const ViewportTransform &transform,
                                    const QSize &viewportSize,
                                    qreal devicePixelRatio,
                                    const QVector<ViewportRenderObject> &visibleSceneShapes,
                                    qreal baseGridStep,
                                    const BlenderGridAppearance &appearance);
    bool renderBackgroundToCurrentFramebuffer(const QSize &viewportSize,
                                              qreal devicePixelRatio,
                                              const QColor &solidColor = QColor(),
                                              bool sceneLinearOutput = false);
    bool renderBackgroundToCurrentFramebuffer(
        const QSize &viewportSize, qreal devicePixelRatio,
        const ViewportThemeBackground &themeBackground,
        bool sceneLinearOutput = false);
    bool renderBackgroundToCurrentFramebuffer(
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const QVector3D &solidColorSceneLinear);
    bool renderUnjitteredSceneDepthToCurrentFramebuffer(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const QVector<ViewportRenderObject> &visibleSceneShapes);
    bool pickScenePoint(const QPointF &screenPosition,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        qreal devicePixelRatio,
                        const QVector<ViewportRenderObject> &visibleSceneShapes,
                        Point3D *worldPoint);
    bool pickComponentElement(
        const QPointF &screenPosition,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const QVector<ViewportRenderObject> &visibleSceneShapes,
        const QVector<ViewportComponentPickCandidate> &candidates,
        int *candidateIndex);
    bool pickComponentElementsInRect(
        const QRectF &selectionRect,
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const QVector<ViewportRenderObject> &visibleSceneShapes,
        const QVector<ViewportComponentPickCandidate> &candidates,
        bool depthTest,
        QVector<int> *candidateIndices);
    void setSurfaceTessellationCache(
        const SurfaceTessellationCache *surfaceTessellationCache);
    // Draw only showInFront geometry into the currently bound overlay depth
    // target. The caller owns and clears that target; this method preserves
    // ordinary scene depth and uses normal depth testing between front items.
    void drawFrontSceneDepth(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const QVector<ViewportRenderObject> &visibleSceneShapes,
        bool surfacesOnly = false);
    void setAntiAliasingSamples(int samples);
    int antiAliasingSamples() const;

private:
    bool initialize();
    bool initializeResources();
    bool renderBackgroundWithColors(const QSize &viewportSize,
                                    qreal devicePixelRatio,
                                    const QVector3D &highColor,
                                    const QVector3D &lowColor,
                                    bool colorsSceneLinear,
                                    bool sceneLinearOutput,
                                    int gradientStyle = 2);
    bool ensureFramebuffer(const QSize &pixelSize);
    bool ensureFrontDepthFramebuffer(const QSize &pixelSize);
    void mergeFrontDepthToCurrentFramebuffer(const QSize &pixelSize);
    void drawAndMergeFrontSceneDepth(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const QVector<ViewportRenderObject> &visibleSceneShapes,
        bool surfacesOnly = false);
    bool renderGridLayer(const ViewportTransform &transform,
                         const QSize &viewportSize,
                         qreal devicePixelRatio,
                         const QVector<ViewportRenderObject> &visibleSceneShapes,
                         qreal baseGridStep,
                         const BlenderGridAppearance &appearance);
    void drawSceneDepth(const ViewportDepthGeometry &geometry,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        qreal devicePixelRatio,
                        bool surfacesOnly = false,
                        bool overlayOnly = false);
    void uploadSceneDepthGeometry(const ViewportDepthGeometry &geometry);
    void updateSceneDepthGeometry(
        const QVector<ViewportRenderObject> &visibleSceneShapes);
    bool drawGrid(const ViewportTransform &transform,
                  const QSize &viewportSize,
                  qreal devicePixelRatio,
                  qreal baseGridStep,
                  const BlenderGridAppearance &appearance);

    QOpenGLContext context_;
    QOffscreenSurface surface_;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer_;
    std::unique_ptr<QOpenGLFramebufferObject> resolvedFramebuffer_;
    std::unique_ptr<QOpenGLFramebufferObject> pickFramebuffer_;
    GLuint frontDepthFramebuffer_ = 0;
    GLuint frontDepthTexture_ = 0;
    QSize frontDepthPixelSize_;
    QOpenGLTextureBlitter textureBlitter_;
    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram backgroundProgram_;
    QOpenGLShaderProgram sceneDepthProgram_;
    QOpenGLShaderProgram componentPickProgram_;
    QOpenGLShaderProgram frontDepthMergeProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLVertexArrayObject sceneDepthVertexArray_;
    QOpenGLVertexArrayObject componentPickVertexArray_;
    QOpenGLBuffer sceneDepthVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer componentPickVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    ViewportDepthGeometry cachedDepthGeometry_;
    struct DepthObjectRange {
        int lineFirst = 0;
        int lineCount = 0;
        int surfaceFirst = 0;
        int surfaceCount = 0;
        int pointFirst = 0;
        int pointCount = 0;
        Point3D offset;
        bool showInFront = false;
    };
    QVector<DepthObjectRange> depthObjectRanges_;
    const SurfaceTessellationCache *surfaceTessellationCache_ = nullptr;
    QByteArray depthGeometryCacheKey_;
    int sceneDepthLineVertexCount_ = 0;
    int sceneDepthSurfaceVertexCount_ = 0;
    int sceneDepthPointVertexCount_ = 0;
    bool depthGeometryCacheValid_ = false;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
    bool usingWidgetContext_ = false;
    int antiAliasingSamples_ = 8;
    int framebufferSampleRequest_ = -1;
};

} // namespace classiCAD
