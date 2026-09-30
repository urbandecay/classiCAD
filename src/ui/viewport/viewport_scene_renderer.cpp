/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_scene_renderer.h"

#include "blender_grid_renderer.h"
#include "viewport_depth_geometry.h"

#include <QImage>
#include <QDebug>
#include <QOpenGLContext>
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
            const Point3D world = workPlanePointToWorld(
                point, shape.workPlane, shape.workPlaneOffset);
            vertices.append(QVector3D(world.x, world.y, world.z));
        }
    }
    return vertices;
}

} // namespace

ViewportSceneRenderer::~ViewportSceneRenderer()
{
    if (QOpenGLContext::currentContext() != nullptr) {
        if (vertexBuffer_.isCreated()) {
            vertexBuffer_.destroy();
        }
        if (vertexArray_.isCreated()) {
            vertexArray_.destroy();
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
        program_.removeAllShaders();
        pointProgram_.removeAllShaders();
        pictureProgram_.removeAllShaders();
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
        !pointProgram_.link() || !vertexArray_.create() || !vertexBuffer_.create()) {
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

bool ViewportSceneRenderer::draw(
    const QVector<ViewportSceneStroke> &strokes,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio)
{
    if (strokes.isEmpty()) {
        return true;
    }
    if (QOpenGLContext::currentContext() == nullptr || !initialize()) {
        return false;
    }

    QVector<QByteArray> geometryKeys;
    geometryKeys.reserve(strokes.size());
    for (const ViewportSceneStroke &stroke : strokes) {
        QByteArray key = stroke.shape == nullptr
                             ? QByteArray()
                             : viewportDepthGeometryCacheKey(
                                   QVector<Shape>{*stroke.shape});
        key.append(stroke.controlGuide ? '\1' : '\0');
        geometryKeys.append(std::move(key));
    }
    const bool geometryChanged = geometryKeys != strokeGeometryKeys_;
    if (geometryChanged) {
        strokeGeometryKeys_ = std::move(geometryKeys);
        strokeRanges_.clear();
        cachedVertices_.clear();
        strokeRanges_.reserve(strokes.size());
        for (const ViewportSceneStroke &stroke : strokes) {
            const int first = cachedVertices_.size();
            if (stroke.shape != nullptr) {
                if (stroke.controlGuide) {
                    cachedVertices_ += controlGuideVertices(*stroke.shape);
                } else {
                    const ViewportDepthGeometry geometry =
                        buildViewportDepthGeometry(*stroke.shape);
                    cachedVertices_ += stroke.pointDiameter > 0.0f
                                           ? geometry.pointVertices
                                           : geometry.lineVertices;
                }
            }
            strokeRanges_.append({first, cachedVertices_.size() - first});
        }
    }
    if (cachedVertices_.isEmpty()) {
        return true;
    }

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
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
    QOpenGLShaderProgram *boundProgram = nullptr;
    const QMatrix4x4 viewProjection =
        viewportViewProjection(transform, viewportSize);
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
            glDrawArrays(GL_LINES, range.first, range.second);
        }
    }
    vertexBuffer_.release();
    vertexArray_.release();
    if (boundProgram != nullptr) {
        boundProgram->release();
    }
    glDisable(GL_PROGRAM_POINT_SIZE);
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
        const Point3D world = workPlanePointToWorld(
            corners[cornerIndex], picture.workPlane, picture.workPlaneOffset);
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
