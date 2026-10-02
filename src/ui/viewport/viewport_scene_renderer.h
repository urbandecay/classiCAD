/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/model.h"
#include "services/viewport/viewport_transform.h"

#include <QColor>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>
#include <QVector>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>

namespace classiCAD {

enum class ViewportSceneLineStyle : int {
    Solid = 0,
    Dashed = 1,
    Dotted = 2,
    Pattern = 3,
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
    std::array<float, 8> linePatternSegmentsWidthUnits{};
    int linePatternSegmentCount = 0;
};

struct ViewportSceneStrokePattern {
    ViewportSceneLineStyle style = ViewportSceneLineStyle::Solid;
    float periodPixels = 0.0f;
    float onLengthPixels = 0.0f;
    std::array<float, 8> segmentsPixels{};
    int segmentCount = 0;
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
                safeWidth};
    case ViewportSceneLineStyle::Solid:
    default:
        return {ViewportSceneLineStyle::Solid, 0.0f, 0.0f};
    case ViewportSceneLineStyle::Pattern: {
        const int count = std::clamp(stroke.linePatternSegmentCount,
                                     0,
                                     static_cast<int>(stroke.linePatternSegmentsWidthUnits.size()));
        if (count < 2 || count % 2 != 0) {
            return {ViewportSceneLineStyle::Solid, 0.0f, 0.0f};
        }
        ViewportSceneStrokePattern pattern;
        pattern.style = ViewportSceneLineStyle::Pattern;
        pattern.segmentCount = count;
        for (int index = 0; index < count; ++index) {
            const float segmentPixels =
                stroke.linePatternSegmentsWidthUnits[index] * safeWidth * safeScale;
            if (!std::isfinite(segmentPixels) || segmentPixels <= 0.0f) {
                return {ViewportSceneLineStyle::Solid, 0.0f, 0.0f};
            }
            pattern.segmentsPixels[static_cast<std::size_t>(index)] = segmentPixels;
            pattern.periodPixels += segmentPixels;
        }
        return pattern;
    }
    }
}

// Renders cached curve strokes, points, and picture textures into the current
// widget framebuffer. A second instance can render transient tool previews
// without invalidating committed-scene geometry.
class ViewportSceneRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportSceneRenderer();

    bool draw(const QVector<ViewportSceneStroke> &strokes,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio);
    bool drawArcOnePointOverlay(const ViewportTransform &transform,
                                const QSize &viewportSize,
                                qreal devicePixelRatio,
                                const WorkPlaneFrame &workPlaneFrame,
                                const QVector<QPointF> &pendingPoints,
                                const QPointF &cursorWorld,
                                bool cursorValid,
                                qreal arcSweep,
                                const QColor &curveColor,
                                const SnapResult &currentSnap,
                                qreal compassRotation,
                                const QImage &hudText,
                                bool drawCurve,
                                bool snapLabelsVisible);
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
    bool initializeOverlay();
    bool initializePicture();

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram pointProgram_;
    QOpenGLShaderProgram pictureProgram_;
    QOpenGLShaderProgram overlayProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLVertexArrayObject overlayVertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer overlayVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer patternOffsetBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject pictureVertexArray_;
    QOpenGLBuffer pictureVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QHash<qint64, PictureTexture> pictureTextures_;
    GLuint overlayTextTexture_ = 0;
    QVector<QByteArray> strokeGeometryKeys_;
    QVector<QPair<int, int>> strokeRanges_;
    QVector<QVector3D> cachedVertices_;
    QVector<float> cachedPatternOffsets_;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
    bool overlayInitializationAttempted_ = false;
    bool overlayInitialized_ = false;
    bool pictureInitializationAttempted_ = false;
    bool pictureInitialized_ = false;
};

} // namespace classiCAD
