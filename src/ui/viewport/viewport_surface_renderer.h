/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_shading.h"
#include "viewport_render_frame.h"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMatrix4x4>
#include <QVector3D>
#include <QString>

namespace classiCAD {

// Draws display triangles derived from exact NURBS surfaces. The saved
// NurbsSurface3D remains authoritative; this renderer consumes the cached
// tessellation shared with hit testing and scene-depth rendering.
class ViewportSurfaceRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportSurfaceRenderer();

    bool draw(const QVector<ViewportRenderObject> &objects,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio,
              const ViewportShadingSettings &settings,
              bool previewOverlay = false,
              bool clearDepth = true,
              bool selectionOverlay = false);

private:
    struct SurfaceVertex {
        float x;
        float y;
        float z;
        float nx;
        float ny;
        float nz;
    };

    struct DrawRange {
        int first = 0;
        int count = 0;
        Point3D offset;
        bool selected = false;
        QColor objectColor;
        quint64 objectSeed = 0;
        bool outlineHandledBySceneStroke = false;
    };

    bool initialize();
    void prepareGeometry(const QVector<ViewportRenderObject> &objects);
    bool renderShadowMap(const ViewportTransform &transform,
                         const QSize &viewportSize,
                         const ViewportShadingSettings &settings);
    bool ensureShadowMap();
    bool ensureObjectIdFramebuffer(const QSize &pixelSize);
    bool ensureMatcapTexture(const QString &presetName);
    bool ensureAgxDisplayTexture();

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram outlineProgram_;
    QOpenGLShaderProgram objectIdProgram_;
    QOpenGLShaderProgram shadowProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLVertexArrayObject outlineVertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    GLuint objectIdFramebuffer_ = 0;
    GLuint objectIdTexture_ = 0;
    GLuint objectIdDepthStencil_ = 0;
    QSize objectIdFramebufferSize_;
    bool objectIdFramebufferAttempted_ = false;
    QVector<SurfaceVertex> vertices_;
    QVector<DrawRange> ranges_;
    unsigned int matcapTexture_ = 0;
    unsigned int agxDisplayTexture_ = 0;
    unsigned int shadowTexture_ = 0;
    unsigned int shadowFramebuffer_ = 0;
    QMatrix4x4 shadowViewProjection_;
    QString matcapTextureName_;
    QByteArray geometryKey_;
    bool geometryDirty_ = true;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
};

} // namespace classiCAD
