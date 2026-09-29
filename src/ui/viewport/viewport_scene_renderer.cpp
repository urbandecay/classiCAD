/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_scene_renderer.h"

#include "blender_grid_renderer.h"
#include "viewport_depth_geometry.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QVector4D>

#include <algorithm>

namespace classiCAD {

namespace {

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
        program_.removeAllShaders();
        pointProgram_.removeAllShaders();
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
            glDrawArrays(GL_POINTS, range.first, range.second);
        } else {
            boundProgram->setUniformValue("uWidth", stroke.width * float(dpr));
            boundProgram->setUniformValue("uDashed", stroke.controlGuide ? 1 : 0);
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

} // namespace classiCAD
