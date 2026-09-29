/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/model.h"
#include "services/viewport/viewport_transform.h"

#include <QColor>
#include <QByteArray>
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
};

// Renders committed curve strokes and points into the current widget framebuffer.
// Text, pictures, and tool overlays remain in the Qt overlay pass.
class ViewportSceneRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportSceneRenderer();

    bool draw(const QVector<ViewportSceneStroke> &strokes,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio);

private:
    bool initialize();

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram pointProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QVector<QByteArray> strokeGeometryKeys_;
    QVector<QPair<int, int>> strokeRanges_;
    QVector<QVector3D> cachedVertices_;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
};

} // namespace classiCAD
