/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/model.h"
#include "services/viewport/viewport_transform.h"

#include <QColor>
#include <QByteArray>
#include <QHash>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>
#include <QVector>
#include <QVector3D>

namespace classiCAD {

struct ViewportSceneStroke {
    const Shape *shape = nullptr;
    QColor color;
    float width = 2.0f;
    bool controlGuide = false;
    float pointDiameter = 0.0f;
    bool dashed = false;
    bool pointOutline = false;
};

// Renders cached curve strokes and points into the current widget framebuffer.
// A second instance can render transient tool previews without invalidating
// committed-scene geometry. It also renders picture previews as GL textures;
// committed pictures and viewport annotations remain in Qt.
class ViewportSceneRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportSceneRenderer();

    bool draw(const QVector<ViewportSceneStroke> &strokes,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio);
    bool drawPicture(const Shape &picture,
                     const ViewportTransform &transform,
                     const QSize &viewportSize,
                     qreal devicePixelRatio,
                     float opacity);

private:
    struct PictureTexture {
        GLuint texture = 0;
        QSize size;
    };

    bool initialize();
    bool initializePicture();

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram pointProgram_;
    QOpenGLShaderProgram pictureProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject pictureVertexArray_;
    QOpenGLBuffer pictureVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QHash<qint64, PictureTexture> pictureTextures_;
    QVector<QByteArray> strokeGeometryKeys_;
    QVector<QPair<int, int>> strokeRanges_;
    QVector<QVector3D> cachedVertices_;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
    bool pictureInitializationAttempted_ = false;
    bool pictureInitialized_ = false;
};

} // namespace classiCAD
