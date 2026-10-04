/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_scene_renderer.h"

#include "blender_grid_renderer.h"
#include "core/geometry/arc_curve_factory.h"
#include "core/geometry/nurbs_curve.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/shape_mapping.h"
#include "line_type_style.h"
#include "viewport_depth_geometry.h"
#include "viewport_render_frame.h"

#include <QImage>
#include <QDebug>
#include <QDataStream>
#include <QFont>
#include <QFontMetricsF>
#include <QIODevice>
#include <QOpenGLContext>
#include <QPainter>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cstddef>

namespace classiCAD {

namespace {

struct PictureVertex {
    QVector3D position;
    QVector2D textureCoordinate;
};

struct OverlayVertex {
    float x = 0.0f;
    float y = 0.0f;
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float alpha = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
};

void appendOverlayVertex(QVector<OverlayVertex> &vertices,
                         const QPointF &point,
                         const QColor &color,
                         qreal u = 0.0,
                         qreal v = 0.0)
{
    vertices.append({static_cast<float>(point.x()),
                     static_cast<float>(point.y()),
                     static_cast<float>(color.redF()),
                     static_cast<float>(color.greenF()),
                     static_cast<float>(color.blueF()),
                     static_cast<float>(color.alphaF()),
                     static_cast<float>(u),
                     static_cast<float>(v)});
}

void appendOverlayTriangle(QVector<OverlayVertex> &vertices,
                           const QPointF &first,
                           const QPointF &second,
                           const QPointF &third,
                           const QColor &color)
{
    appendOverlayVertex(vertices, first, color);
    appendOverlayVertex(vertices, second, color);
    appendOverlayVertex(vertices, third, color);
}

void appendOverlayLine(QVector<OverlayVertex> &vertices,
                       const QPointF &first,
                       const QPointF &second,
                       const QColor &color,
                       qreal width = 1.0)
{
    const QPointF delta = second - first;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (!std::isfinite(length) || length <= 1.0e-9) {
        return;
    }
    const QPointF normal(-delta.y() * width / (2.0 * length),
                         delta.x() * width / (2.0 * length));
    const QPointF firstLeft = first + normal;
    const QPointF firstRight = first - normal;
    const QPointF secondLeft = second + normal;
    const QPointF secondRight = second - normal;
    appendOverlayTriangle(vertices, firstLeft, firstRight, secondRight, color);
    appendOverlayTriangle(vertices, firstLeft, secondRight, secondLeft, color);
}

void appendOverlayFilledCircle(QVector<OverlayVertex> &vertices,
                               const QPointF &center,
                               qreal radius,
                               const QColor &color,
                               int samples = 32)
{
    if (radius <= 0.0) {
        return;
    }
    constexpr qreal twoPi = 6.28318530717958647692;
    for (int index = 0; index < samples; ++index) {
        const qreal firstAngle = twoPi * index / samples;
        const qreal secondAngle = twoPi * (index + 1) / samples;
        appendOverlayTriangle(
            vertices,
            center,
            center + QPointF(radius * std::cos(firstAngle),
                             radius * std::sin(firstAngle)),
            center + QPointF(radius * std::cos(secondAngle),
                             radius * std::sin(secondAngle)),
            color);
    }
}

void appendOverlayCircle(QVector<OverlayVertex> &vertices,
                         const QPointF &center,
                         qreal radius,
                         const QColor &color,
                         qreal width = 1.0,
                         int samples = 48)
{
    constexpr qreal twoPi = 6.28318530717958647692;
    QPointF previous = center + QPointF(radius, 0.0);
    for (int index = 1; index <= samples; ++index) {
        const qreal angle = twoPi * index / samples;
        const QPointF next = center +
            QPointF(radius * std::cos(angle), radius * std::sin(angle));
        appendOverlayLine(vertices, previous, next, color, width);
        previous = next;
    }
}

QColor twoPointArcGuideColor(const QPointF &localVector,
                             const WorkPlaneFrame &frame)
{
    const Point3D worldVector{
        localVector.x() * frame.xAxis.x + localVector.y() * frame.yAxis.x,
        localVector.x() * frame.xAxis.y + localVector.y() * frame.yAxis.y,
        localVector.x() * frame.xAxis.z + localVector.y() * frame.yAxis.z};
    const qreal length = std::sqrt(worldVector.x * worldVector.x +
                                   worldVector.y * worldVector.y +
                                   worldVector.z * worldVector.z);
    if (length <= 1.0e-9) {
        return QColor(72, 72, 72);
    }

    constexpr qreal axisTolerance = 0.9999;
    if (std::abs(worldVector.x) / length > axisTolerance) {
        return QColor(255, 26, 26);
    }
    if (std::abs(worldVector.y) / length > axisTolerance) {
        return QColor(26, 179, 26);
    }
    if (std::abs(worldVector.z) / length > axisTolerance) {
        return QColor(51, 128, 255);
    }
    return QColor(92, 92, 92);
}

void appendOverlayRectangle(QVector<OverlayVertex> &vertices,
                            const QRectF &rect,
                            const QColor &color,
                            qreal width = 1.0)
{
    const QPointF topLeft = rect.topLeft();
    const QPointF topRight = rect.topRight();
    const QPointF bottomRight = rect.bottomRight();
    const QPointF bottomLeft = rect.bottomLeft();
    appendOverlayLine(vertices, topLeft, topRight, color, width);
    appendOverlayLine(vertices, topRight, bottomRight, color, width);
    appendOverlayLine(vertices, bottomRight, bottomLeft, color, width);
    appendOverlayLine(vertices, bottomLeft, topLeft, color, width);
}

void appendOverlayDiamond(QVector<OverlayVertex> &vertices,
                          const QPointF &center,
                          qreal radius,
                          const QColor &color,
                          qreal width)
{
    const QPointF top = center + QPointF(0.0, -radius);
    const QPointF right = center + QPointF(radius, 0.0);
    const QPointF bottom = center + QPointF(0.0, radius);
    const QPointF left = center + QPointF(-radius, 0.0);
    appendOverlayLine(vertices, top, right, color, width);
    appendOverlayLine(vertices, right, bottom, color, width);
    appendOverlayLine(vertices, bottom, left, color, width);
    appendOverlayLine(vertices, left, top, color, width);
}

QColor arcCompassColor(const Point3D &normal)
{
    constexpr qreal axisTolerance = 0.999999;
    if (std::abs(normal.x) > axisTolerance) {
        return QColor::fromRgbF(0.85, 0.0, 0.0, 1.0);
    }
    if (std::abs(normal.y) > axisTolerance) {
        return QColor::fromRgbF(0.0, 0.60, 0.0, 1.0);
    }
    if (std::abs(normal.z) > axisTolerance) {
        return QColor::fromRgbF(0.149, 0.376, 1.0, 1.0);
    }
    return QColor(225, 225, 225, 255);
}

void appendOverlayRoundedRect(QVector<OverlayVertex> &vertices,
                              const QRectF &rect,
                              qreal radius,
                              const QColor &color)
{
    constexpr qreal halfPi = 1.57079632679489661923;
    constexpr int cornerSamples = 6;
    const qreal cornerRadius = std::clamp(
        radius, 0.0, std::min(rect.width(), rect.height()) * 0.5);
    QVector<QPointF> outline;
    outline.reserve(cornerSamples * 4 + 4);
    const std::array<QPointF, 4> centers = {
        QPointF(rect.right() - cornerRadius, rect.top() + cornerRadius),
        QPointF(rect.right() - cornerRadius, rect.bottom() - cornerRadius),
        QPointF(rect.left() + cornerRadius, rect.bottom() - cornerRadius),
        QPointF(rect.left() + cornerRadius, rect.top() + cornerRadius)};
    const std::array<qreal, 4> starts = {-halfPi, 0.0, halfPi, 2.0 * halfPi};
    for (int corner = 0; corner < 4; ++corner) {
        for (int sample = 0; sample <= cornerSamples; ++sample) {
            const qreal angle = starts[static_cast<std::size_t>(corner)] +
                halfPi * sample / cornerSamples;
            outline.append(centers[static_cast<std::size_t>(corner)] +
                           QPointF(cornerRadius * std::cos(angle),
                                   cornerRadius * std::sin(angle)));
        }
    }
    const QPointF center = rect.center();
    for (int index = 0; index < outline.size(); ++index) {
        appendOverlayTriangle(vertices,
                              center,
                              outline[index],
                              outline[(index + 1) % outline.size()],
                              color);
    }
}

QImage snapLabelImage(SnapType type, qreal devicePixelRatio)
{
    const QString label = snapTypeName(type);
    QFont font(QStringLiteral("Sans"), 9, QFont::Bold);
    const QFontMetricsF metrics(font);
    const qreal logicalWidth = std::max<qreal>(1.0, metrics.horizontalAdvance(label));
    const qreal logicalHeight = std::max<qreal>(1.0, metrics.height());
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    QImage image(QSize(static_cast<int>(std::ceil(logicalWidth * dpr)),
                       static_cast<int>(std::ceil(logicalHeight * dpr))),
                 QImage::Format_RGBA8888);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#63b5e8")));
    painter.drawText(QPointF(0.0, metrics.ascent()), label);
    return image;
}

QVector<QVector3D> controlGuideVertices(const Shape &shape)
{
    QVector<QVector3D> vertices;
    const QVector<QPointF> &points = shape.nurbs.controlPoints;
    if (points.size() < 2) {
        return vertices;
    }
    vertices.reserve((points.size() - 1) * 2);
    for (int index = 0; index + 1 < points.size(); ++index) {
        for (const QPointF &point : {points[index], points[index + 1]}) {
            const Point3D world = shapePointToWorld(shape, point);
            vertices.append(QVector3D(world.x, world.y, world.z));
        }
    }
    return vertices;
}

float projectedStrokeSegmentLength(const QVector3D &first,
                                   const QVector3D &second,
                                   const QMatrix4x4 &viewProjection,
                                   const QSize &pixelSize)
{
    QVector4D clipFirst = viewProjection * QVector4D(first, 1.0f);
    QVector4D clipSecond = viewProjection * QVector4D(second, 1.0f);
    const float nearFirst = clipFirst.z() + clipFirst.w();
    const float nearSecond = clipSecond.z() + clipSecond.w();
    if (nearFirst < 0.0f && nearSecond < 0.0f) {
        return 0.0f;
    }
    if (nearFirst < 0.0f) {
        const float fraction = nearFirst / (nearFirst - nearSecond);
        clipFirst = clipFirst * (1.0f - fraction) + clipSecond * fraction;
    } else if (nearSecond < 0.0f) {
        const float fraction = nearSecond / (nearSecond - nearFirst);
        clipSecond = clipSecond * (1.0f - fraction) + clipFirst * fraction;
    }
    if (clipFirst.w() <= 0.0f || clipSecond.w() <= 0.0f) {
        return 0.0f;
    }

    const float deltaX = (clipSecond.x() / clipSecond.w() -
                          clipFirst.x() / clipFirst.w()) *
                         static_cast<float>(pixelSize.width()) * 0.5f;
    const float deltaY = (clipSecond.y() / clipSecond.w() -
                          clipFirst.y() / clipFirst.w()) *
                         static_cast<float>(pixelSize.height()) * 0.5f;
    return std::hypot(deltaX, deltaY);
}

} // namespace

bool makeViewportSceneStrokes(const ViewportRenderObject &object,
                              bool rendererAvailable,
                              ViewportSceneStroke *sceneStroke,
                              ViewportSceneStroke *controlGuide)
{
    if (!rendererAvailable || sceneStroke == nullptr) {
        return false;
    }
    if (controlGuide != nullptr) {
        *controlGuide = {};
    }

    const Shape &shape = object.shape;
    const GeometryType geometryType = shape.geometryType;
    const bool strokeType =
        geometryType == GeometryType::Point ||
        geometryType == GeometryType::Line ||
        geometryType == GeometryType::Rectangle ||
        geometryType == GeometryType::Polygon ||
        geometryType == GeometryType::Circle ||
        geometryType == GeometryType::Ellipse ||
        geometryType == GeometryType::Arc ||
        geometryType == GeometryType::PolyCurve ||
        geometryType == GeometryType::Bezier ||
        geometryType == GeometryType::Nurbs ||
        geometryType == GeometryType::NurbsSurface ||
        geometryType == GeometryType::NurbsSolid;
    const LayerGpuLinePattern layerPattern =
        layerGpuLinePattern(object.layerLineType);
    const bool nativeStroke =
        strokeType &&
        (geometryType == GeometryType::Point || object.selected ||
         object.scalePreview || object.rotatePreview ||
         layerPattern.kind != LayerGpuLinePatternKind::Unsupported) &&
        (geometryType != GeometryType::Arc || validateNurbsCurve(shape.nurbs)) &&
        (geometryType != GeometryType::Circle ||
         validateNurbsCurve(shape.nurbs)) &&
        (geometryType != GeometryType::Ellipse ||
         validateNurbsCurve(shape.nurbs)) &&
        ((geometryType != GeometryType::Bezier &&
          geometryType != GeometryType::Nurbs) ||
         validateNurbsCurve(shape.nurbs)) &&
        (geometryType != GeometryType::NurbsSurface ||
         validateNurbsSurface(shape.nurbsSurface)) &&
        (geometryType != GeometryType::NurbsSolid ||
         validateNurbsSolid(shape.nurbsSolid)) &&
        (geometryType != GeometryType::PolyCurve ||
         !shape.components.isEmpty());
    if (!nativeStroke) {
        return false;
    }

    const bool highlighted =
        object.selected || object.scalePreview || object.rotatePreview;
    const qreal storedWidth = object.layerLineWeightMm > 0.0
                                  ? std::clamp(object.layerLineWeightMm * 6.0,
                                               1.0, 10.0)
                                  : 2.0;
    if (controlGuide != nullptr &&
        (geometryType == GeometryType::Bezier ||
         geometryType == GeometryType::Nurbs)) {
        *controlGuide = {&shape, QColor(QStringLiteral("#8aa7c7")), 1.0f, true};
        controlGuide->objectId = object.objectId;
        controlGuide->geometryRevision = object.geometryRevision;
        controlGuide->cacheableGeometry = object.cacheable;
    }

    *sceneStroke = {
        &shape,
        highlighted ? QColor(QStringLiteral("#5da9e9"))
                    : object.layerColor.isValid()
                          ? object.layerColor
                          : QColor(QStringLiteral("#d28b45")),
        static_cast<float>(highlighted ? 3.5 : storedWidth),
        false,
        geometryType == GeometryType::Point
            ? (highlighted ? 10.0f : 9.0f)
            : 0.0f};
    sceneStroke->objectId = object.objectId;
    sceneStroke->geometryRevision = object.geometryRevision;
    sceneStroke->cacheableGeometry = object.cacheable;
    sceneStroke->preparedDepthGeometry = object.preparedDepthGeometry;

    if (!highlighted) {
        switch (layerPattern.kind) {
        case LayerGpuLinePatternKind::Dashed:
            sceneStroke->lineStyle = ViewportSceneLineStyle::Dashed;
            break;
        case LayerGpuLinePatternKind::Dotted:
            sceneStroke->lineStyle = ViewportSceneLineStyle::Dotted;
            break;
        case LayerGpuLinePatternKind::Pattern:
            sceneStroke->lineStyle = ViewportSceneLineStyle::Pattern;
            break;
        case LayerGpuLinePatternKind::Solid:
        case LayerGpuLinePatternKind::Unsupported:
        default:
            sceneStroke->lineStyle = ViewportSceneLineStyle::Solid;
            break;
        }
        sceneStroke->linePatternScale = static_cast<float>(layerPattern.scale);
        sceneStroke->linePatternSegmentCount = std::min(
            static_cast<int>(layerPattern.segments.size()),
            static_cast<int>(sceneStroke->linePatternSegmentsWidthUnits.size()));
        for (int index = 0; index < sceneStroke->linePatternSegmentCount; ++index) {
            sceneStroke->linePatternSegmentsWidthUnits[
                static_cast<std::size_t>(index)] =
                static_cast<float>(layerPattern.segments[index]);
        }
    }
    return true;
}

ViewportSceneRenderer::~ViewportSceneRenderer()
{
    if (QOpenGLContext::currentContext() != nullptr) {
        if (vertexBuffer_.isCreated()) {
            vertexBuffer_.destroy();
        }
        if (patternOffsetBuffer_.isCreated()) {
            patternOffsetBuffer_.destroy();
        }
        if (vertexArray_.isCreated()) {
            vertexArray_.destroy();
        }
        if (overlayVertexBuffer_.isCreated()) {
            overlayVertexBuffer_.destroy();
        }
        if (overlayVertexArray_.isCreated()) {
            overlayVertexArray_.destroy();
        }
        if (pictureVertexBuffer_.isCreated()) {
            pictureVertexBuffer_.destroy();
        }
        if (pictureVertexArray_.isCreated()) {
            pictureVertexArray_.destroy();
        }
        for (auto texture = pictureTextures_.begin();
             texture != pictureTextures_.end();
             ++texture) {
            if (texture->texture != 0) {
                glDeleteTextures(1, &texture->texture);
            }
        }
        if (overlayTextTexture_ != 0) {
            glDeleteTextures(1, &overlayTextTexture_);
            overlayTextTexture_ = 0;
        }
        program_.removeAllShaders();
        pointProgram_.removeAllShaders();
        pictureProgram_.removeAllShaders();
        overlayProgram_.removeAllShaders();
    }
}

bool ViewportSceneRenderer::initialize()
{
    if (initializationAttempted_) {
        return initialized_;
    }
    initializationAttempted_ = true;
    if (!initializeOpenGLFunctions() ||
        !program_.addShaderFromSourceFile(
            QOpenGLShader::Vertex,
            QStringLiteral(":/classiCAD/shaders/scene_stroke.vert")) ||
        !program_.addShaderFromSourceFile(
            QOpenGLShader::Geometry,
            QStringLiteral(":/classiCAD/shaders/scene_stroke.geom")) ||
        !program_.addShaderFromSourceFile(
            QOpenGLShader::Fragment,
            QStringLiteral(":/classiCAD/shaders/scene_stroke.frag")) ||
        !program_.link() ||
        !pointProgram_.addShaderFromSourceFile(
            QOpenGLShader::Vertex,
            QStringLiteral(":/classiCAD/shaders/scene_point.vert")) ||
        !pointProgram_.addShaderFromSourceFile(
            QOpenGLShader::Fragment,
            QStringLiteral(":/classiCAD/shaders/scene_point.frag")) ||
        !pointProgram_.link() || !vertexArray_.create() ||
        !vertexBuffer_.create() || !patternOffsetBuffer_.create()) {
        qWarning().noquote() << "Viewport scene stroke shader setup failed:"
                             << program_.log() << pointProgram_.log();
        return false;
    }
    initialized_ = true;
    return true;
}

bool ViewportSceneRenderer::initializePicture()
{
    if (pictureInitializationAttempted_) {
        return pictureInitialized_;
    }
    pictureInitializationAttempted_ = true;
    if (QOpenGLContext::currentContext() == nullptr ||
        !initializeOpenGLFunctions() ||
        !pictureProgram_.addShaderFromSourceFile(
            QOpenGLShader::Vertex,
            QStringLiteral(":/classiCAD/shaders/scene_picture.vert")) ||
        !pictureProgram_.addShaderFromSourceFile(
            QOpenGLShader::Fragment,
            QStringLiteral(":/classiCAD/shaders/scene_picture.frag")) ||
        !pictureProgram_.link() || !pictureVertexArray_.create() ||
        !pictureVertexBuffer_.create()) {
        qWarning().noquote() << "Viewport picture preview shader setup failed:"
                             << pictureProgram_.log();
        return false;
    }
    pictureInitialized_ = true;
    return true;
}

bool ViewportSceneRenderer::initializeOverlay()
{
    if (overlayInitializationAttempted_) {
        return overlayInitialized_;
    }
    overlayInitializationAttempted_ = true;
    static constexpr const char *vertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec2 aPosition;
        layout(location = 1) in vec4 aColor;
        layout(location = 2) in vec2 aTextureCoordinate;
        uniform vec2 uViewportSize;
        out vec4 vColor;
        out vec2 vTextureCoordinate;
        void main()
        {
            vec2 normalized = vec2(2.0 * aPosition.x / uViewportSize.x - 1.0,
                                   1.0 - 2.0 * aPosition.y / uViewportSize.y);
            gl_Position = vec4(normalized, 0.0, 1.0);
            vColor = aColor;
            vTextureCoordinate = aTextureCoordinate;
        }
    )glsl";
    static constexpr const char *fragmentShader = R"glsl(
        #version 330 core
        in vec4 vColor;
        in vec2 vTextureCoordinate;
        uniform sampler2D uTextTexture;
        uniform int uUseTexture;
        out vec4 fragmentColor;
        void main()
        {
            fragmentColor = uUseTexture == 0
                                ? vColor
                                : texture(uTextTexture, vTextureCoordinate) * vColor;
        }
    )glsl";
    if (QOpenGLContext::currentContext() == nullptr ||
        !initializeOpenGLFunctions() ||
        !overlayProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                  vertexShader) ||
        !overlayProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                  fragmentShader) ||
        !overlayProgram_.link() || !overlayVertexArray_.create() ||
        !overlayVertexBuffer_.create()) {
        qWarning().noquote() << "Viewport overlay shader setup failed:"
                             << overlayProgram_.log();
        return false;
    }

    overlayVertexArray_.bind();
    overlayVertexBuffer_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex),
                          reinterpret_cast<const void *>(offsetof(OverlayVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex),
                          reinterpret_cast<const void *>(offsetof(OverlayVertex, red)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex),
                          reinterpret_cast<const void *>(offsetof(OverlayVertex, u)));
    overlayVertexBuffer_.release();
    overlayVertexArray_.release();
    overlayInitialized_ = true;
    return true;
}

bool ViewportSceneRenderer::draw(
    const QVector<ViewportSceneStroke> &strokes,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio)
{
    if (strokes.isEmpty()) {
        strokeGeometryKeys_.clear();
        strokeRanges_.clear();
        cachedVertices_.clear();
        cachedPatternOffsets_.clear();
        return true;
    }
    if (QOpenGLContext::currentContext() == nullptr || !initialize()) {
        return false;
    }

    QVector<QByteArray> geometryKeys;
    geometryKeys.reserve(strokes.size());
    for (const ViewportSceneStroke &stroke : strokes) {
        QByteArray key;
        QDataStream keyStream(&key, QIODevice::WriteOnly);
        if (stroke.cacheableGeometry && stroke.objectId.isValid() &&
            stroke.geometryRevision != 0) {
            keyStream << quint8(1) << quint64(stroke.objectId.value())
                      << stroke.geometryRevision;
        } else if (stroke.shape != nullptr) {
            keyStream << quint8(0)
                      << viewportDepthGeometryCacheKey(
                             QVector<Shape>{*stroke.shape});
        } else {
            keyStream << quint8(0);
        }
        keyStream << quint8(stroke.controlGuide ? 1 : 0);
        geometryKeys.append(std::move(key));
    }
    const bool geometryChanged = geometryKeys != strokeGeometryKeys_;
    if (geometryChanged) {
        strokeGeometryKeys_ = geometryKeys;
        strokeRanges_.clear();
        cachedVertices_.clear();
        cachedPatternOffsets_.clear();
        strokeRanges_.reserve(strokes.size());
        for (int strokeIndex = 0; strokeIndex < strokes.size(); ++strokeIndex) {
            const ViewportSceneStroke &stroke = strokes[strokeIndex];
            QVector<QVector3D> generatedVertices;
            const QVector<QVector3D> *vertices = nullptr;
            if (stroke.controlGuide && stroke.shape != nullptr) {
                generatedVertices = controlGuideVertices(*stroke.shape);
                vertices = &generatedVertices;
            } else if (stroke.preparedDepthGeometry) {
                const ViewportDepthGeometry &geometry =
                    *stroke.preparedDepthGeometry;
                vertices = stroke.pointDiameter > 0.0f
                               ? &geometry.pointVertices
                               : &geometry.lineVertices;
            } else if (stroke.shape != nullptr) {
                const ViewportDepthGeometry geometry =
                    buildViewportDepthGeometry(*stroke.shape);
                generatedVertices = stroke.pointDiameter > 0.0f
                                       ? geometry.pointVertices
                                       : geometry.lineVertices;
                vertices = &generatedVertices;
            }
            const int first = cachedVertices_.size();
            if (vertices != nullptr) {
                cachedVertices_ += *vertices;
            }
            strokeRanges_.append(
                {first, cachedVertices_.size() - first});
        }
        cachedPatternOffsets_.fill(0.0f, cachedVertices_.size());
    }
    if (cachedVertices_.isEmpty()) {
        return true;
    }

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    const QMatrix4x4 viewProjection =
        viewportViewProjection(transform, viewportSize);
    QVector<float> patternOffsets(cachedVertices_.size(), 0.0f);
    for (int index = 0; index < strokes.size(); ++index) {
        if (strokes[index].pointDiameter > 0.0f) {
            continue;
        }
        const auto range = strokeRanges_[index];
        float accumulatedPixels = 0.0f;
        QVector3D previousEnd;
        bool hasPreviousEnd = false;
        for (int offset = 0; offset + 1 < range.second; offset += 2) {
            const int firstIndex = range.first + offset;
            const int secondIndex = firstIndex + 1;
            const QVector3D &first = cachedVertices_[firstIndex];
            const QVector3D &second = cachedVertices_[secondIndex];
            if (hasPreviousEnd) {
                const float coordinateScale = std::max(
                    {1.0f, first.length(), previousEnd.length()});
                const float connectionTolerance =
                    std::max(1.0e-5f, coordinateScale * 1.0e-6f);
                if ((first - previousEnd).lengthSquared() >
                    connectionTolerance * connectionTolerance) {
                    accumulatedPixels = 0.0f;
                }
            }
            patternOffsets[firstIndex] = accumulatedPixels;
            patternOffsets[secondIndex] = accumulatedPixels;
            accumulatedPixels += projectedStrokeSegmentLength(
                first, second, viewProjection, pixelSize);
            previousEnd = second;
            hasPreviousEnd = true;
        }
    }
    const bool patternOffsetsChanged = patternOffsets != cachedPatternOffsets_;
    if (patternOffsetsChanged) {
        cachedPatternOffsets_ = std::move(patternOffsets);
    }
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glEnable(GL_MULTISAMPLE);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);

    vertexArray_.bind();
    vertexBuffer_.bind();
    if (geometryChanged) {
        vertexBuffer_.allocate(
            cachedVertices_.constData(),
            static_cast<int>(cachedVertices_.size() * sizeof(QVector3D)));
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(QVector3D), nullptr);
    vertexBuffer_.release();
    if (geometryChanged || patternOffsetsChanged) {
        patternOffsetBuffer_.bind();
        patternOffsetBuffer_.allocate(
            cachedPatternOffsets_.constData(),
            static_cast<int>(cachedPatternOffsets_.size() * sizeof(float)));
        patternOffsetBuffer_.release();
    }
    patternOffsetBuffer_.bind();
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
    patternOffsetBuffer_.release();
    QOpenGLShaderProgram *boundProgram = nullptr;
    for (int index = 0; index < strokes.size(); ++index) {
        const ViewportSceneStroke &stroke = strokes[index];
        const auto range = strokeRanges_[index];
        if (range.second == 0) {
            continue;
        }
        QOpenGLShaderProgram *nextProgram = stroke.pointDiameter > 0.0f
                                                ? &pointProgram_ : &program_;
        if (nextProgram != boundProgram) {
            if (boundProgram != nullptr) {
                boundProgram->release();
            }
            nextProgram->bind();
            nextProgram->setUniformValue("uViewProjection", viewProjection);
            if (nextProgram == &program_) {
                nextProgram->setUniformValue(
                    "uViewportSize",
                    QVector2D(pixelSize.width(), pixelSize.height()));
            }
            boundProgram = nextProgram;
        }
        boundProgram->setUniformValue(
            "uColor", QVector4D(stroke.color.redF(), stroke.color.greenF(),
                                  stroke.color.blueF(), stroke.color.alphaF()));
        if (stroke.pointDiameter > 0.0f) {
            boundProgram->setUniformValue("uPointSize",
                                          stroke.pointDiameter * float(dpr));
            boundProgram->setUniformValue("uPointOutline",
                                          stroke.pointOutline);
            glDrawArrays(GL_POINTS, range.first, range.second);
        } else {
            const float widthPixels = stroke.width * float(dpr);
            const ViewportSceneStrokePattern pattern =
                viewportSceneStrokePattern(stroke, widthPixels);
            boundProgram->setUniformValue("uWidth", widthPixels);
            boundProgram->setUniformValue(
                "uLineStyle", static_cast<int>(pattern.style));
            boundProgram->setUniformValue("uPatternPeriod", pattern.periodPixels);
            boundProgram->setUniformValue("uPatternOnLength",
                                          pattern.onLengthPixels);
            boundProgram->setUniformValue("uPatternSegmentCount",
                                          pattern.segmentCount);
            if (pattern.segmentCount > 0) {
                boundProgram->setUniformValueArray(
                    "uPatternSegments", pattern.segmentsPixels.data(),
                    pattern.segmentCount, 1);
            }
            glDrawArrays(GL_LINES, range.first, range.second);
        }
    }
    vertexArray_.release();
    if (boundProgram != nullptr) {
        boundProgram->release();
    }
    glDisable(GL_PROGRAM_POINT_SIZE);
    glDisable(GL_BLEND);
    return true;
}

bool ViewportSceneRenderer::drawArcToolOverlay(
    const ViewportTransform &transform,
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
    bool snapLabelsVisible)
{
    if (QOpenGLContext::currentContext() == nullptr ||
        viewportSize.width() <= 0 || viewportSize.height() <= 0 ||
        !initializeOverlay()) {
        return false;
    }

    const QColor resolvedCurveColor = curveColor.isValid()
                                          ? curveColor
                                          : QColor(QStringLiteral("#d28b45"));
    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    QVector<OverlayVertex> overlayVertices;
    overlayVertices.reserve(4500);
    const WorkPlaneFrame frame = isValidWorkPlaneFrame(workPlaneFrame)
                                     ? workPlaneFrame
                                     : transform.workPlaneFrame();

    const auto projectFramePoint = [&](const QPointF &localPoint,
                                       QPointF *screenPoint) {
        return isValidWorkPlaneFrame(frame) &&
            transform.worldPointToScreen(
                workPlaneFramePointToWorld(localPoint, frame),
                viewportSize,
                screenPoint);
    };

    const auto appendCompass = [&](const QPointF &localCenter,
                                   qreal rotation) {
        if (!isValidWorkPlaneFrame(frame)) {
            return;
        }
        constexpr int ringSamples = 72;
        constexpr int angleIncrementDegrees = 15;
        constexpr qreal compassRadiusPixels = 62.5;
        const Point3D centerWorld =
            workPlaneFramePointToWorld(localCenter, frame);
        QPointF centerScreen;
        const Point3D viewDirection = transform.viewDirection();
        const Point3D viewUp = transform.viewUp();
        const Point3D rightUnnormalized = {
            viewUp.y * viewDirection.z - viewUp.z * viewDirection.y,
            viewUp.z * viewDirection.x - viewUp.x * viewDirection.z,
            viewUp.x * viewDirection.y - viewUp.y * viewDirection.x};
        const qreal rightLength = std::sqrt(
            rightUnnormalized.x * rightUnnormalized.x +
            rightUnnormalized.y * rightUnnormalized.y +
            rightUnnormalized.z * rightUnnormalized.z);
        if (rightLength <= 1.0e-12 ||
            !transform.worldPointToScreenUnclipped(centerWorld,
                                                    viewportSize,
                                                    &centerScreen)) {
            return;
        }
        const Point3D cameraRight = {rightUnnormalized.x / rightLength,
                                     rightUnnormalized.y / rightLength,
                                     rightUnnormalized.z / rightLength};
        const Point3D cameraRightProbe = {
            centerWorld.x + cameraRight.x,
            centerWorld.y + cameraRight.y,
            centerWorld.z + cameraRight.z};
        QPointF cameraRightScreen;
        if (!transform.worldPointToScreenUnclipped(cameraRightProbe,
                                                    viewportSize,
                                                    &cameraRightScreen)) {
            return;
        }
        const qreal pixelsPerWorldUnit = std::hypot(
            cameraRightScreen.x() - centerScreen.x(),
            cameraRightScreen.y() - centerScreen.y());
        if (!std::isfinite(pixelsPerWorldUnit) ||
            pixelsPerWorldUnit <= 1.0e-9) {
            return;
        }
        const qreal compassRadiusWorld =
            compassRadiusPixels / pixelsPerWorldUnit;
        constexpr qreal outerRadius = 1.0;
        const qreal innerRadius = outerRadius * (80.0 / 120.0);
        const qreal tickLength = outerRadius * (10.0 / 120.0);
        const qreal crossLength = outerRadius * (10.0 / 120.0);
        const qreal cosine = std::cos(rotation);
        const qreal sine = std::sin(rotation);
        const auto rotated = [cosine, sine](qreal x, qreal y) {
            return QPointF(x * cosine - y * sine,
                           x * sine + y * cosine);
        };
        const auto projectCompassPoint = [&](qreal x,
                                             qreal y,
                                             bool rotatePoint,
                                             QPointF *screenPoint) {
            if (screenPoint == nullptr) {
                return false;
            }
            const QPointF local = rotatePoint ? rotated(x, y) : QPointF(x, y);
            const Point3D worldPoint = {
                centerWorld.x + compassRadiusWorld *
                    (frame.xAxis.x * local.x() + frame.yAxis.x * local.y()),
                centerWorld.y + compassRadiusWorld *
                    (frame.xAxis.y * local.x() + frame.yAxis.y * local.y()),
                centerWorld.z + compassRadiusWorld *
                    (frame.xAxis.z * local.x() + frame.yAxis.z * local.y())};
            return transform.worldPointToScreenUnclipped(worldPoint,
                                                          viewportSize,
                                                          screenPoint);
        };
        const QColor color = arcCompassColor(frame.normal);
        const auto appendCompassLine = [&](qreal firstX,
                                           qreal firstY,
                                           qreal secondX,
                                           qreal secondY,
                                           bool rotateLine) {
            QPointF first;
            QPointF second;
            if (projectCompassPoint(firstX, firstY, rotateLine, &first) &&
                projectCompassPoint(secondX, secondY, rotateLine, &second)) {
                appendOverlayLine(overlayVertices, first, second, color, 1.0);
            }
        };

        for (int sample = 0; sample < ringSamples; ++sample) {
            const qreal firstAngle = twoPi * sample / ringSamples;
            const qreal secondAngle = twoPi * (sample + 1) / ringSamples;
            appendCompassLine(outerRadius * std::cos(firstAngle),
                              outerRadius * std::sin(firstAngle),
                              outerRadius * std::cos(secondAngle),
                              outerRadius * std::sin(secondAngle),
                              true);
        }
        constexpr int tickCount = 360 / angleIncrementDegrees;
        for (int tick = 0; tick < tickCount; ++tick) {
            const qreal angle = twoPi * tick / tickCount;
            appendCompassLine(outerRadius * std::cos(angle),
                              outerRadius * std::sin(angle),
                              (outerRadius - tickLength) * std::cos(angle),
                              (outerRadius - tickLength) * std::sin(angle),
                              true);
        }
        const auto appendProtractorArc = [&](qreal startAngle,
                                             qreal endAngle) {
            constexpr int arcSamples = 48;
            QPointF first;
            QPointF previous;
            if (!projectCompassPoint(innerRadius * std::cos(startAngle),
                                     innerRadius * std::sin(startAngle),
                                     true,
                                     &first)) {
                return;
            }
            previous = first;
            bool previousVisible = true;
            for (int sample = 1; sample <= arcSamples; ++sample) {
                const qreal fraction = static_cast<qreal>(sample) / arcSamples;
                const qreal angle = startAngle +
                    (endAngle - startAngle) * fraction;
                QPointF next;
                const bool nextVisible = projectCompassPoint(
                    innerRadius * std::cos(angle),
                    innerRadius * std::sin(angle),
                    true,
                    &next);
                if (previousVisible && nextVisible) {
                    appendOverlayLine(overlayVertices,
                                      previous,
                                      next,
                                      color,
                                      1.0);
                }
                previous = next;
                previousVisible = nextVisible;
            }
            QPointF arcEnd;
            if (projectCompassPoint(innerRadius * std::cos(endAngle),
                                    innerRadius * std::sin(endAngle),
                                    true,
                                    &arcEnd)) {
                appendOverlayLine(overlayVertices,
                                  first,
                                  arcEnd,
                                  color,
                                  1.0);
            }
        };
        appendProtractorArc(200.0 * pi / 180.0,
                            340.0 * pi / 180.0);
        appendProtractorArc(20.0 * pi / 180.0,
                            160.0 * pi / 180.0);
        appendCompassLine(-crossLength, 0.0, crossLength, 0.0, false);
        appendCompassLine(0.0, -crossLength, 0.0, crossLength, false);
    };

    if (arcMode == ArcMode::OnePoint && isValidWorkPlaneFrame(frame)) {
        const QPointF compassCenter = pendingPoints.isEmpty()
                                          ? cursorWorld
                                          : pendingPoints.first();
        if (pendingPoints.isEmpty() ? cursorValid : true) {
            appendCompass(compassCenter, compassRotation);
        }
    }

    const QColor startColor(204, 204, 51);
    const QColor endColor(51, 204, 51);
    const QColor pointColor(QStringLiteral("#f0a45a"));
    if ((arcMode == ArcMode::TwoPoint || arcMode == ArcMode::ThreePoint) &&
        isValidWorkPlaneFrame(frame)) {
        if (pendingPoints.size() == 1 && cursorValid) {
            QPointF firstScreen;
            QPointF cursorScreen;
            if (projectFramePoint(pendingPoints.first(), &firstScreen) &&
                projectFramePoint(cursorWorld, &cursorScreen)) {
                appendOverlayLine(
                    overlayVertices, firstScreen, cursorScreen,
                    twoPointArcGuideColor(cursorWorld - pendingPoints.first(), frame),
                    1.0);
            }
        } else if (pendingPoints.size() >= 2) {
            const QPointF first = pendingPoints[0];
            const QPointF second = pendingPoints[1];
            const QPointF chord = second - first;
            const qreal chordLength = std::hypot(chord.x(), chord.y());
            QPointF firstScreen;
            QPointF secondScreen;
            const bool firstVisible = projectFramePoint(first, &firstScreen);
            const bool secondVisible = projectFramePoint(second, &secondScreen);
            if (firstVisible && secondVisible) {
                appendOverlayLine(overlayVertices, firstScreen, secondScreen,
                                  twoPointArcGuideColor(chord, frame), 1.0);
            }

            if (cursorValid && chordLength > 1.0e-9) {
                if (arcMode == ArcMode::TwoPoint) {
                    const QPointF midpoint = (first + second) * 0.5;
                    QPointF midpointScreen;
                    QPointF cursorScreen;
                    if (projectFramePoint(midpoint, &midpointScreen) &&
                        projectFramePoint(cursorWorld, &cursorScreen)) {
                        appendOverlayLine(
                            overlayVertices, midpointScreen, cursorScreen,
                            twoPointArcGuideColor(cursorWorld - midpoint, frame),
                            1.0);
                    }
                }

                CircularArc2D arc;
                if (drawCurve &&
                    makeCircularArcThroughPoint(first, second, cursorWorld, &arc)) {
                    constexpr qreal twoPi = 6.28318530717958647692;
                    constexpr int samplesPerRevolution = 96;
                    const int sampleCount = std::max(
                        8,
                        static_cast<int>(std::ceil(
                            std::abs(arc.sweepAngle) *
                            samplesPerRevolution / twoPi)));
                    QPointF previous;
                    bool previousVisible = false;
                    for (int index = 0; index <= sampleCount; ++index) {
                        const qreal angle = arc.startAngle + arc.sweepAngle *
                            (static_cast<qreal>(index) / sampleCount);
                        QPointF projected;
                        const QPointF point = arc.center +
                            QPointF(arc.radius * std::cos(angle),
                                    arc.radius * std::sin(angle));
                        const bool visible = projectFramePoint(point, &projected);
                        if (previousVisible && visible) {
                            appendOverlayLine(overlayVertices,
                                              previous,
                                              projected,
                                              resolvedCurveColor,
                                              1.5);
                        }
                        previous = projected;
                        previousVisible = visible;
                    }
                }
            }
        }
    } else if (!pendingPoints.isEmpty() &&
               isValidWorkPlaneFrame(frame)) {
        QPointF centerScreen;
        if (projectFramePoint(pendingPoints.first(), &centerScreen)) {
            if (pendingPoints.size() == 1) {
                QPointF cursorScreen;
                if (cursorValid && projectFramePoint(cursorWorld, &cursorScreen)) {
                    appendOverlayLine(overlayVertices,
                                      centerScreen,
                                      cursorScreen,
                                      startColor,
                                      1.0);
                }
            } else {
                const QPointF center = pendingPoints[0];
                const QPointF radiusVector = pendingPoints[1] - center;
                const qreal radius = std::hypot(radiusVector.x(),
                                                radiusVector.y());
                if (radius > 1.0e-9) {
                    const qreal startAngle = std::atan2(radiusVector.y(),
                                                        radiusVector.x());
                    QPointF startScreen;
                    if (projectFramePoint(pendingPoints[1], &startScreen)) {
                        appendOverlayLine(overlayVertices,
                                          centerScreen,
                                          startScreen,
                                          startColor,
                                          1.0);
                    }
                    const qreal displayedSweep = std::clamp(
                        arcSweep, -twoPi + 1.0e-6, twoPi - 1.0e-6);
                    if (drawCurve && std::abs(displayedSweep) > 1.0e-12) {
                        constexpr int samplesPerRevolution = 96;
                        const int sampleCount = std::max(
                            8,
                            static_cast<int>(std::ceil(
                                std::abs(displayedSweep) *
                                samplesPerRevolution / twoPi)));
                        QPointF previous;
                        bool previousValid = false;
                        for (int index = 0; index <= sampleCount; ++index) {
                            const qreal angle = startAngle + displayedSweep *
                                (static_cast<qreal>(index) / sampleCount);
                            QPointF projected;
                            const bool projectedValid = projectFramePoint(
                                center + QPointF(radius * std::cos(angle),
                                                 radius * std::sin(angle)),
                                &projected);
                            if (previousValid && projectedValid) {
                                appendOverlayLine(overlayVertices,
                                                  previous,
                                                  projected,
                                                  resolvedCurveColor,
                                                  1.0);
                                appendOverlayFilledCircle(overlayVertices,
                                                          projected,
                                                          2.0,
                                                          resolvedCurveColor,
                                                          12);
                            }
                            previous = projected;
                            previousValid = projectedValid;
                        }
                    }
                    const qreal endAngle = startAngle + displayedSweep;
                    QPointF endScreen;
                    if (projectFramePoint(
                            center + QPointF(radius * std::cos(endAngle),
                                             radius * std::sin(endAngle)),
                            &endScreen)) {
                        appendOverlayLine(overlayVertices,
                                          centerScreen,
                                          endScreen,
                                          endColor,
                                          1.0);
                    }
                }
            }
        }
    }
    if (isValidWorkPlaneFrame(frame)) {
        for (const QPointF &point : pendingPoints) {
            QPointF screen;
            if (projectFramePoint(point, &screen)) {
                appendOverlayFilledCircle(overlayVertices,
                                          screen,
                                          5.0,
                                          QColor(QStringLiteral("#282828")));
                appendOverlayCircle(overlayVertices, screen, 5.0,
                                    pointColor, 1.5);
            }
        }
        if (cursorValid &&
            (!pendingPoints.isEmpty() || arcMode == ArcMode::TwoPoint ||
             arcMode == ArcMode::ThreePoint)) {
            QPointF cursorScreen;
            if (projectFramePoint(cursorWorld, &cursorScreen)) {
                appendOverlayFilledCircle(overlayVertices,
                                          cursorScreen,
                                          4.0,
                                          pointColor);
            }
        }
    }

    QPointF snapScreen;
    const bool snapScreenValid = currentSnap.isValid();
    if (snapScreenValid) {
        snapScreen = transform.worldToScreen(currentSnap.point, viewportSize);
        const QColor snapColor(QStringLiteral("#63b5e8"));
        switch (currentSnap.type) {
        case SnapType::Endpoint:
            appendOverlayCircle(overlayVertices, snapScreen, 7.0,
                                snapColor, 2.0);
            break;
        case SnapType::Midpoint:
            appendOverlayRectangle(overlayVertices,
                                   QRectF(snapScreen - QPointF(6.0, 6.0),
                                          snapScreen + QPointF(6.0, 6.0)),
                                   snapColor,
                                   2.0);
            break;
        case SnapType::ControlPoint:
            appendOverlayRectangle(overlayVertices,
                                   QRectF(snapScreen - QPointF(7.0, 7.0),
                                          snapScreen + QPointF(7.0, 7.0)),
                                   snapColor,
                                   2.0);
            break;
        case SnapType::Intersection:
            appendOverlayLine(overlayVertices,
                              snapScreen - QPointF(7.0, 7.0),
                              snapScreen + QPointF(7.0, 7.0),
                              snapColor,
                              2.0);
            appendOverlayLine(overlayVertices,
                              snapScreen - QPointF(7.0, -7.0),
                              snapScreen + QPointF(7.0, -7.0),
                              snapColor,
                              2.0);
            break;
        case SnapType::Center:
            appendOverlayCircle(overlayVertices, snapScreen, 7.0,
                                snapColor, 2.0);
            appendOverlayLine(overlayVertices,
                              snapScreen - QPointF(9.0, 0.0),
                              snapScreen + QPointF(9.0, 0.0),
                              snapColor,
                              2.0);
            appendOverlayLine(overlayVertices,
                              snapScreen - QPointF(0.0, 9.0),
                              snapScreen + QPointF(0.0, 9.0),
                              snapColor,
                              2.0);
            break;
        case SnapType::Perpendicular: {
            const QPointF corner = snapScreen + QPointF(-2.0, 3.0);
            appendOverlayLine(overlayVertices,
                              snapScreen + QPointF(-8.0, 3.0),
                              corner,
                              snapColor,
                              2.0);
            appendOverlayLine(overlayVertices,
                              corner,
                              snapScreen + QPointF(-2.0, -5.0),
                              snapColor,
                              2.0);
            break;
        }
        case SnapType::Tangent:
            appendOverlayCircle(overlayVertices, snapScreen, 6.0,
                                snapColor, 2.0);
            appendOverlayLine(overlayVertices,
                              snapScreen + QPointF(-7.0, 4.0),
                              snapScreen + QPointF(7.0, -4.0),
                              snapColor,
                              2.0);
            break;
        case SnapType::Near:
            appendOverlayDiamond(overlayVertices,
                                 snapScreen,
                                 7.0,
                                 snapColor,
                                 2.0);
            break;
        case SnapType::None:
            break;
        }
    }

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glEnable(GL_MULTISAMPLE);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    overlayProgram_.bind();
    overlayProgram_.setUniformValue(
        "uViewportSize",
        QVector2D(static_cast<float>(viewportSize.width()),
                  static_cast<float>(viewportSize.height())));
    overlayProgram_.setUniformValue("uUseTexture", 0);

    const auto drawVertices = [&](const QVector<OverlayVertex> &vertices,
                                  bool useTexture,
                                  const QImage &textureImage) {
        if (vertices.isEmpty()) {
            return;
        }
        overlayVertexArray_.bind();
        overlayVertexBuffer_.bind();
        overlayVertexBuffer_.allocate(
            vertices.constData(),
            static_cast<int>(vertices.size() * sizeof(OverlayVertex)));
        overlayProgram_.setUniformValue("uUseTexture", useTexture ? 1 : 0);
        if (useTexture) {
            if (overlayTextTexture_ == 0) {
                glGenTextures(1, &overlayTextTexture_);
            }
            const QImage rgbaImage =
                textureImage.convertToFormat(QImage::Format_RGBA8888);
            GLint previousUnpackAlignment = 4;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, overlayTextTexture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA8,
                         rgbaImage.width(),
                         rgbaImage.height(),
                         0,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         rgbaImage.constBits());
            glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
            overlayProgram_.setUniformValue("uTextTexture", 0);
        }
        glDrawArrays(GL_TRIANGLES, 0, vertices.size());
        if (useTexture) {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        overlayVertexBuffer_.release();
        overlayVertexArray_.release();
    };

    drawVertices(overlayVertices, false, {});

    const auto makeTextureQuad = [](const QRectF &rect) {
        QVector<OverlayVertex> vertices;
        const QColor white(Qt::white);
        appendOverlayVertex(vertices, rect.topLeft(), white, 0.0, 0.0);
        appendOverlayVertex(vertices, rect.bottomLeft(), white, 0.0, 1.0);
        appendOverlayVertex(vertices, rect.bottomRight(), white, 1.0, 1.0);
        appendOverlayVertex(vertices, rect.topLeft(), white, 0.0, 0.0);
        appendOverlayVertex(vertices, rect.bottomRight(), white, 1.0, 1.0);
        appendOverlayVertex(vertices, rect.topRight(), white, 1.0, 0.0);
        return vertices;
    };

    if (snapLabelsVisible && snapScreenValid) {
        const QImage label = snapLabelImage(currentSnap.type, dpr);
        const QFont labelFont(QStringLiteral("Sans"), 9, QFont::Bold);
        const QFontMetricsF labelMetrics(labelFont);
        const QRectF labelRect(snapScreen.x() + 10.0,
                               snapScreen.y() - 10.0 - labelMetrics.ascent(),
                               label.width() / dpr,
                               label.height() / dpr);
        drawVertices(makeTextureQuad(labelRect), true, label);
    }

    const qreal panelWidth = std::max<qreal>(
        1.0,
        std::min<qreal>(hudPanelWidth, viewportSize.width() - 24.0));
    const QRectF panel(12.0,
                       std::max<qreal>(12.0, viewportSize.height() - 58.0),
                       panelWidth,
                       46.0);
    QVector<OverlayVertex> panelVertices;
    panelVertices.reserve(72);
    appendOverlayRoundedRect(panelVertices,
                             panel,
                             4.0,
                             QColor(20, 20, 20, 170));
    drawVertices(panelVertices, false, {});
    if (!hudText.isNull()) {
        drawVertices(makeTextureQuad(panel), true, hudText);
    }

    overlayProgram_.release();
    glDisable(GL_BLEND);
    return true;
}

bool ViewportSceneRenderer::drawPicture(const Shape &picture,
                                        const ViewportTransform &transform,
                                        const QSize &viewportSize,
                                        qreal devicePixelRatio,
                                        float opacity)
{
    if (picture.pictureImage.isNull() ||
        picture.geometryType != GeometryType::Picture ||
        QOpenGLContext::currentContext() == nullptr || !initializePicture()) {
        return false;
    }

    const QVector<QPointF> corners = pictureFrameCorners(picture);
    if (corners.size() != 4) {
        return false;
    }

    QImage image = picture.pictureImage.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        return false;
    }
    const qint64 imageCacheKey = picture.pictureImage.cacheKey();
    auto texture = pictureTextures_.find(imageCacheKey);
    if (texture == pictureTextures_.end()) {
        PictureTexture newTexture;
        glGenTextures(1, &newTexture.texture);
        if (newTexture.texture == 0) {
            return false;
        }
        texture = pictureTextures_.insert(imageCacheKey, newTexture);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (texture->size != image.size()) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     image.width(),
                     image.height(),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     image.constBits());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        texture->size = image.size();
    }

    std::array<PictureVertex, 6> vertices{};
    const std::array<QVector2D, 4> textureCoordinates{
        QVector2D(0.0f, 0.0f),
        QVector2D(1.0f, 0.0f),
        QVector2D(1.0f, 1.0f),
        QVector2D(0.0f, 1.0f),
    };
    const std::array<int, 6> cornerIndices{0, 1, 2, 0, 2, 3};
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const int cornerIndex = cornerIndices[index];
        const Point3D world = shapePointToWorld(picture, corners[cornerIndex]);
        vertices[index] = {
            QVector3D(static_cast<float>(world.x),
                      static_cast<float>(world.y),
                      static_cast<float>(world.z)),
            textureCoordinates[cornerIndex],
        };
    }

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    pictureVertexArray_.bind();
    pictureVertexBuffer_.bind();
    pictureVertexBuffer_.allocate(vertices.data(), sizeof(vertices));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(PictureVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          sizeof(PictureVertex),
                          reinterpret_cast<const void *>(
                              offsetof(PictureVertex, textureCoordinate)));
    pictureProgram_.bind();
    pictureProgram_.setUniformValue(
        "uViewProjection", viewportViewProjection(transform, viewportSize));
    pictureProgram_.setUniformValue("uPicture", 0);
    pictureProgram_.setUniformValue("uOpacity", std::clamp(opacity, 0.0f, 1.0f));
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    pictureProgram_.release();
    pictureVertexBuffer_.release();
    pictureVertexArray_.release();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    return true;
}

} // namespace classiCAD
