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
#include <QOpenGLVertexArrayObject>
#include <QSurfaceFormat>
#include <QSize>

#include <memory>

namespace classiCAD {

class BlenderGridRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~BlenderGridRenderer();

    QImage render(const ViewportTransform &transform,
                  const QSize &viewportSize,
                  qreal devicePixelRatio,
                  const QVector<Shape> &visibleSceneShapes,
                  qreal baseGridStep,
                  const BlenderGridAppearance &appearance);
    void setAntiAliasingSamples(int samples);
    int antiAliasingSamples() const;

private:
    bool initialize();
    void drawSceneDepth(const ViewportDepthGeometry &geometry,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        qreal devicePixelRatio);
    void uploadSceneDepthGeometry(const ViewportDepthGeometry &geometry);
    bool drawGrid(const ViewportTransform &transform,
                  const QSize &viewportSize,
                  qreal devicePixelRatio,
                  qreal baseGridStep,
                  const BlenderGridAppearance &appearance);

    QOpenGLContext context_;
    QOffscreenSurface surface_;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer_;
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
    int antiAliasingSamples_ = 8;
    int framebufferSampleRequest_ = -1;
};

} // namespace classiCAD
