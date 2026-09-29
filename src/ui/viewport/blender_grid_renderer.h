/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 *
 * Adapted from Blender's 3D View grid shaders and grid draw setup.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "blender_grid_appearance.h"
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

QMatrix4x4 viewportViewProjection(const ViewportTransform &transform,
                                   const QSize &viewportSize);

class BlenderGridRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~BlenderGridRenderer();

    QImage render(const ViewportTransform &transform,
                  const QSize &viewportSize,
                  qreal devicePixelRatio,
                  const QVector<Shape> &visibleSceneShapes,
                  qreal baseGridStep,
                  const BlenderGridAppearance &appearance);
    // Called while a QOpenGLWidget framebuffer is current. The grid remains
    // GPU resident and is composited over the scene already drawn there.
    bool renderToCurrentFramebuffer(const ViewportTransform &transform,
                                    const QSize &viewportSize,
                                    qreal devicePixelRatio,
                                    const QVector<Shape> &visibleSceneShapes,
                                    qreal baseGridStep,
                                    const BlenderGridAppearance &appearance);
    bool pickScenePoint(const QPointF &screenPosition,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        qreal devicePixelRatio,
                        const QVector<Shape> &visibleSceneShapes,
                        Point3D *worldPoint);
    void setAntiAliasingSamples(int samples);
    int antiAliasingSamples() const;

private:
    bool initialize();
    bool initializeResources();
    bool ensureFramebuffer(const QSize &pixelSize);
    bool renderGridLayer(const ViewportTransform &transform,
                         const QSize &viewportSize,
                         qreal devicePixelRatio,
                         const QVector<Shape> &visibleSceneShapes,
                         qreal baseGridStep,
                         const BlenderGridAppearance &appearance);
    void drawSceneDepth(const ViewportDepthGeometry &geometry,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        qreal devicePixelRatio);
    void uploadSceneDepthGeometry(const ViewportDepthGeometry &geometry);
    void updateSceneDepthGeometry(const QVector<Shape> &visibleSceneShapes);
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
    QOpenGLShaderProgram sceneDepthProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLVertexArrayObject sceneDepthVertexArray_;
    QOpenGLBuffer sceneDepthVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    ViewportDepthGeometry cachedDepthGeometry_;
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
