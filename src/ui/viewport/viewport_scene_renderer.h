/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/object_id.h"
#include "core/document/shape.h"
#include "services/snapping/snap_types.h"
#include "services/viewport/viewport_transform.h"

#include <QColor>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSharedPointer>
#include <QSize>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cmath>

namespace classiCAD {

struct ViewportDepthGeometry;
struct ViewportRenderObject;

enum class ViewportSceneLineStyle : int {
    Solid = 0,
    Dashed = 1,
    Dotted = 2,
    Pattern = 3,
};

enum class ViewportSmoothWireCategory : int {
    Overlay = 0,
    EditMode = 1,
};

struct ViewportSceneStroke {
    ViewportSceneStroke() = default;
    ViewportSceneStroke(const Shape *strokeShape,
                        const QColor &strokeColor,
                        float strokeWidth,
                        bool isControlGuide,
                        float markerDiameter = 0.0f,
                        bool isDashed = false,
                        bool outlinesPoint = false)
        : shape(strokeShape)
        , color(strokeColor)
        , width(strokeWidth)
        , controlGuide(isControlGuide)
        , pointDiameter(markerDiameter)
        , dashed(isDashed)
        , pointOutline(outlinesPoint)
    {
    }

    const Shape *shape = nullptr;
    QColor color;
    float width = 2.0f;
    bool controlGuide = false;
    float pointDiameter = 0.0f;
    bool dashed = false;
    bool pointOutline = false;
    // Opt-in for committed Solid-mode CAD wires that participate in the
    // depth-writing Overlay wire pass. Transient/edit overlays stay disabled.
    bool writeDepth = false;
    // Marks Blender mesh wire-over-Solid strokes for the ordinary QPainter
    // fallback, which does not replay the complete GPU scene-stroke list.
    bool blenderMeshWire = false;
    // Determines which Smooth Wires preference controls coverage. This is
    // independent from editModeWire, which selects the geometry/color style.
    ViewportSmoothWireCategory smoothWireCategory =
        ViewportSmoothWireCategory::Overlay;
    bool editModeWire = false;
    ViewportSceneLineStyle lineStyle = ViewportSceneLineStyle::Solid;
    float linePatternScale = 1.0f;
    std::array<float, 8> linePatternSegmentsWidthUnits{};
    int linePatternSegmentCount = 0;
    // Optional color multiplier per GL_LINES endpoint. White preserves the
    // stroke's uniform color; varying endpoint colors produce Blender-style
    // edit-wire fades along a whole edge.
    QVector<QVector4D> lineVertexColors;
    // Render GL_POINTS using per-vertex world radii (Blender Point Clouds).
    bool worldSizedPoints = false;
    // Per-endpoint screen-plane normal nudge for Blender mesh wire overlays.
    // The GPU path applies this in clip space; the QPainter fallback applies
    // the equivalent half-pixel displacement in logical screen coordinates.
    QVector<QVector3D> lineVertexWireOffsets;
    ObjectId objectId = ObjectId::invalid();
    quint64 geometryRevision = 0;
    bool cacheableGeometry = false;
    // Optional exact key for transient geometry such as view-dependent
    // surface silhouettes. It takes precedence over object/revision caching.
    QByteArray geometryCacheKey;
    QSharedPointer<const ViewportDepthGeometry> preparedDepthGeometry;
    Point3D worldOffset;
};

struct ViewportSceneStrokePattern {
    ViewportSceneLineStyle style = ViewportSceneLineStyle::Solid;
    float periodPixels = 0.0f;
    float onLengthPixels = 0.0f;
    std::array<float, 8> segmentsPixels{};
    int segmentCount = 0;
};

// Resolve whether an object can use the native scene stroke renderer and
// populate its committed stroke/control-guide values when it can.
bool makeViewportSceneStrokes(const ViewportRenderObject &object,
                              bool rendererAvailable,
                              ViewportSceneStroke *sceneStroke,
                              ViewportSceneStroke *controlGuide = nullptr);

// Surface perimeters in Solid mode use the same screen-space stroke renderer
// as curves, so layer line weight and antialiasing stay consistent.
bool makeViewportSurfaceOutlineStroke(
    const ViewportRenderObject &object,
    bool rendererAvailable,
    const QColor &outlineColor,
    const QSharedPointer<const ViewportDepthGeometry> &boundary,
    ViewportSceneStroke *sceneStroke);

// Build the visible NURBS surface boundary and view-dependent silhouette from
// the same tessellated triangles used by the surface renderer.
QSharedPointer<const ViewportDepthGeometry> makeViewportSurfaceOutlineGeometry(
    const ViewportDepthGeometry &surfaceMesh,
    const ViewportTransform &camera,
    const Point3D &worldOffset = {});

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
              qreal devicePixelRatio,
              bool depthTest = false,
              qreal opacity = 1.0,
              bool smoothOverlayWires = true,
              bool smoothEditModeWires = true,
              bool sceneLinearOutput = false,
              bool *strokeResolveUsed = nullptr);
    bool drawDepthOnly(const QVector<ViewportSceneStroke> &strokes,
                       const ViewportTransform &transform,
                       const QSize &viewportSize,
                       qreal devicePixelRatio,
                       bool smoothOverlayWires = true,
                       bool smoothEditModeWires = true);
    bool drawArcToolOverlay(const ViewportTransform &transform,
                            const QSize &viewportSize,
                            qreal devicePixelRatio,
                            const WorkPlaneFrame &workPlaneFrame,
                            const QVector<QPointF> &pendingPoints,
                            const QPointF &cursorWorld,
                            bool cursorValid,
                            ArcMode arcMode,
                            qreal arcSweep,
                            const QColor &curveColor,
                            const SnapResult &currentSnap,
                            qreal compassRotation,
                            const QImage &hudText,
                            qreal hudPanelWidth,
                            bool drawCurve,
                            bool snapLabelsVisible);
    bool drawPicture(const Shape &picture,
                     const ViewportTransform &transform,
                     const QSize &viewportSize,
                     qreal devicePixelRatio,
                     float opacity,
                     bool sceneLinearOutput = false,
                     bool depthTest = false,
                     bool displayReferredOverlay = false);

private:
    struct PictureTexture {
        GLuint texture = 0;
        QSize size;
        bool premultipliedAlpha = false;
        GLint internalFormat = GL_RGBA8;
    };

    bool initialize();
    bool initializeStrokeResolve();
    bool ensureStrokeResolveTargets(const QSize &pixelSize,
                                    GLenum depthInternalFormat,
                                    GLenum depthTextureFormat,
                                    GLenum depthTextureType,
                                    GLenum depthAttachment);
    bool initializeOverlay();
    bool initializePicture();

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram pointProgram_;
    QOpenGLShaderProgram pictureProgram_;
    QOpenGLShaderProgram overlayProgram_;
    QOpenGLShaderProgram strokeResolveProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLVertexArrayObject strokeResolveVertexArray_;
    QOpenGLVertexArrayObject overlayVertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer overlayVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer patternOffsetBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer lineColorBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer lineWireOffsetBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer pointRadiusBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject pictureVertexArray_;
    QOpenGLBuffer pictureVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QHash<QString, PictureTexture> pictureTextures_;
    GLuint pictureDestinationTexture_ = 0;
    QSize pictureDestinationTextureSize_;
    GLuint overlayTextTexture_ = 0;
    GLuint strokeBaseFramebuffer_ = 0;
    GLuint strokeLineFramebuffer_ = 0;
    GLuint strokeBaseColorTexture_ = 0;
    GLuint strokeSceneDepthTexture_ = 0;
    GLuint strokeLineColorTexture_ = 0;
    GLuint strokeLineDataTexture_ = 0;
    GLuint strokeLineDepthTexture_ = 0;
    GLuint strokeLineWireFlagTexture_ = 0;
    GLuint strokeWorkingDepthTexture_ = 0;
    QSize strokeResolvePixelSize_;
    GLenum strokeResolveDepthInternalFormat_ = 0;
    GLenum strokeResolveDepthTextureFormat_ = 0;
    GLenum strokeResolveDepthTextureType_ = 0;
    GLenum strokeResolveDepthAttachment_ = GL_DEPTH_ATTACHMENT;
    QVector<QByteArray> strokeGeometryKeys_;
    QVector<QPair<int, int>> strokeRanges_;
    QVector<QVector3D> cachedVertices_;
    QVector<QVector4D> cachedLineColors_;
    QVector<QVector3D> cachedLineWireOffsets_;
    QVector<float> cachedPointWorldRadii_;
    QVector<float> cachedPatternOffsets_;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
    bool strokeResolveInitializationAttempted_ = false;
    bool strokeResolveInitialized_ = false;
    bool overlayInitializationAttempted_ = false;
    bool overlayInitialized_ = false;
    bool pictureInitializationAttempted_ = false;
    bool pictureInitialized_ = false;
};

} // namespace classiCAD
