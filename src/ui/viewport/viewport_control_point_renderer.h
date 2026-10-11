/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"

#include <QColor>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>
#include <QVector>
#include <QVector3D>

namespace classiCAD {

enum class ViewportControlPointShape : int {
    Circle = 0,
    Square = 1,
};

// One screen-sized control-point marker. Position is in world coordinates;
// diameter and outline width are in logical viewport pixels (Qt device pixels
// are applied by draw()).
struct ViewportControlPointHandle {
    QVector3D worldPosition;
    QColor fillColor = QColor(QStringLiteral("#263b4b"));
    QColor outlineColor = QColor(QStringLiteral("#77b7e6"));
    float diameterPixels = 8.0f;
    float outlineWidthPixels = 1.5f;
    ViewportControlPointShape shape = ViewportControlPointShape::Square;
};

// Draws control-point markers into the currently bound viewport framebuffer.
// The caller must have a current OpenGL 3.3 core context. The caller selects
// whether markers respect scene depth (normal edit display) or draw through it
// (X-Ray display).
class ViewportControlPointRenderer final
    : protected QOpenGLFunctions_3_3_Core {
public:
    ViewportControlPointRenderer() = default;
    ~ViewportControlPointRenderer();

    ViewportControlPointRenderer(const ViewportControlPointRenderer &) = delete;
    ViewportControlPointRenderer &operator=(
        const ViewportControlPointRenderer &) = delete;

    // Returns true if the batch was drawn (an empty batch is a successful
    // no-op). Returns false when there is no compatible current GL context or
    // renderer setup/drawing cannot proceed. A false result lets an integrator
    // retain its existing fallback for that frame.
    bool draw(const QVector<ViewportControlPointHandle> &handles,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio = 1.0,
              bool depthTest = false);

    // Free GL resources. Call while the context used for draw() is current,
    // preferably from the viewport's context-destruction callback. Returns
    // false if a different context (or no context) is current.
    bool release();

private:
    bool initialize();

    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLContext *ownerContext_ = nullptr;
    bool initialized_ = false;
};

} // namespace classiCAD
