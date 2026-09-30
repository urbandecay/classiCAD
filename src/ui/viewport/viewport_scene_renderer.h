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

#include <cmath>

namespace classiCAD {

enum class ViewportSceneLineStyle : int {
    Solid = 0,
    Dashed = 1,
    Dotted = 2,
};

struct ViewportSceneStroke {
    const Shape *shape = nullptr;
    QColor color;
    float width = 2.0f;
    bool controlGuide = false;
    float pointDiameter = 0.0f;
    bool dashed = false;
    bool pointOutline = false;
    ViewportSceneLineStyle lineStyle = ViewportSceneLineStyle::Solid;
    float linePatternScale = 1.0f;
};

struct ViewportSceneStrokePattern {
    ViewportSceneLineStyle style = ViewportSceneLineStyle::Solid;
    float periodPixels = 0.0f;
    float onLengthPixels = 0.0f;
};

// Keep the legacy dashed flag and control-guide strokes dashed while allowing
// scene callers to select the explicit solid/dashed/dotted line style.
inline ViewportSceneLineStyle effectiveViewportSceneLineStyle(
    const ViewportSceneStroke &stroke) noexcept
{
    return stroke.controlGuide || stroke.dashed
               ? ViewportSceneLineStyle::Dashed
               : stroke.lineStyle;
}

// Pattern lengths use the final framebuffer-pixel stroke width, so high-DPI
// rendering and line weight scale together. The ratios match the standard
// DASHED (7:3) and DOT (1:2) layer patterns.
inline ViewportSceneStrokePattern viewportSceneStrokePattern(
    const ViewportSceneStroke &stroke, float widthPixels) noexcept
{
    const float safeWidth = widthPixels > 0.0f ? widthPixels : 1.0f;
    const float safeScale = std::isfinite(stroke.linePatternScale) &&
                                    stroke.linePatternScale > 0.0f
                                ? stroke.linePatternScale
                                : 1.0f;
    const ViewportSceneLineStyle style = effectiveViewportSceneLineStyle(stroke);
    switch (style) {
    case ViewportSceneLineStyle::Dashed:
        return {style, safeWidth * 10.0f * safeScale,
                safeWidth * 7.0f * safeScale};
    case ViewportSceneLineStyle::Dotted:
        return {style, safeWidth * 3.0f * safeScale,
                safeWidth * safeScale};
    case ViewportSceneLineStyle::Solid:
    default:
        return {ViewportSceneLineStyle::Solid, 0.0f, 0.0f};
    }
}

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
