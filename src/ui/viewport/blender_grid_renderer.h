/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 *
 * Adapted from Blender's 3D View grid shaders and grid draw setup.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "blender_grid_appearance.h"
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
#include <QSurfaceFormat>
#include <QSize>
#include <QMatrix4x4>

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
                                              const QColor &solidColor = QColor());
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
    void setSurfaceTessellationCache(
        const SurfaceTessellationCache *surfaceTessellationCache);
    void setAntiAliasingSamples(int samples);
    int antiAliasingSamples() const;

private:
    bool initialize();
    bool initializeResources();
    bool ensureFramebuffer(const QSize &pixelSize);
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
                        bool surfacesOnly = false);
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
    QOpenGLTextureBlitter textureBlitter_;
    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram backgroundProgram_;
    QOpenGLShaderProgram sceneDepthProgram_;
    QOpenGLShaderProgram componentPickProgram_;
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
