/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_control_point_renderer.h"

#include "blender_grid_renderer.h"

#include <QDebug>
#include <QOpenGLContext>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace classiCAD {

namespace {

struct MarkerVertex {
    float position[3];
    float fill[4];
    float outline[4];
    float diameterPixels;
    float outlineWidthPixels;
    float shape;
};

static_assert(std::is_standard_layout<MarkerVertex>::value,
              "OpenGL marker vertex attributes require standard layout");

constexpr char kVertexShader[] = R"glsl(
#version 330 core
layout(location = 0) in vec3 aWorldPosition;
layout(location = 1) in vec4 aFillColor;
layout(location = 2) in vec4 aOutlineColor;
layout(location = 3) in float aDiameterPixels;
layout(location = 4) in float aOutlineWidthPixels;
layout(location = 5) in float aShape;

uniform mat4 uViewProjection;
uniform float uDevicePixelRatio;

out vec4 vFillColor;
out vec4 vOutlineColor;
out float vDiameterPixels;
out float vOutlineWidthPixels;
out float vShape;

void main()
{
    gl_Position = uViewProjection * vec4(aWorldPosition, 1.0);
    gl_PointSize = max(aDiameterPixels * uDevicePixelRatio, 1.0);
    vFillColor = aFillColor;
    vOutlineColor = aOutlineColor;
    vDiameterPixels = gl_PointSize;
    vOutlineWidthPixels = aOutlineWidthPixels * uDevicePixelRatio;
    vShape = aShape;
}
)glsl";

constexpr char kFragmentShader[] = R"glsl(
#version 330 core
in vec4 vFillColor;
in vec4 vOutlineColor;
in float vDiameterPixels;
in float vOutlineWidthPixels;
in float vShape;

out vec4 fragmentColor;

void main()
{
    vec2 fromCenter = abs(gl_PointCoord - vec2(0.5)) * vDiameterPixels;
    float halfSize = 0.5 * vDiameterPixels;
    float signedDistance = vShape < 0.5
                               ? length(fromCenter) - halfSize
                               : max(fromCenter.x, fromCenter.y) - halfSize;
    float antialiasWidth = max(fwidth(signedDistance), 0.65);
    float coverage = 1.0 - smoothstep(-antialiasWidth,
                                      antialiasWidth,
                                      signedDistance);
    if (coverage <= 0.0) {
        discard;
    }

    float outlineMix = 0.0;
    if (vOutlineWidthPixels > 0.0) {
        outlineMix = smoothstep(-vOutlineWidthPixels - antialiasWidth,
                                -vOutlineWidthPixels + antialiasWidth,
                                signedDistance);
    }
    vec4 color = mix(vFillColor, vOutlineColor, outlineMix);
    fragmentColor = vec4(color.rgb, color.a * coverage);
}
)glsl";

void setColor(float (&destination)[4], const QColor &color)
{
    destination[0] = static_cast<float>(color.redF());
    destination[1] = static_cast<float>(color.greenF());
    destination[2] = static_cast<float>(color.blueF());
    destination[3] = static_cast<float>(color.alphaF());
}

} // namespace

ViewportControlPointRenderer::~ViewportControlPointRenderer()
{
    release();
}

bool ViewportControlPointRenderer::initialize()
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr ||
        (ownerContext_ != nullptr && ownerContext_ != context)) {
        return false;
    }
    if (initialized_) {
        return true;
    }

    ownerContext_ = context;
    if (!initializeOpenGLFunctions() ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                          kVertexShader) ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                          kFragmentShader) ||
        !program_.link() || !vertexArray_.create() ||
        !vertexBuffer_.create()) {
        qWarning().noquote() << "Viewport control-point shader setup failed:"
                             << program_.log();
        if (vertexBuffer_.isCreated()) {
            vertexBuffer_.destroy();
        }
        if (vertexArray_.isCreated()) {
            vertexArray_.destroy();
        }
        program_.removeAllShaders();
        ownerContext_ = nullptr;
        return false;
    }

    vertexBuffer_.setUsagePattern(QOpenGLBuffer::StreamDraw);
    initialized_ = true;
    return true;
}

bool ViewportControlPointRenderer::draw(
    const QVector<ViewportControlPointHandle> &handles,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio)
{
    if (handles.isEmpty() || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0) {
        return true;
    }
    if (!initialize()) {
        return false;
    }

    if (handles.size() > std::numeric_limits<int>::max() /
                             static_cast<int>(sizeof(MarkerVertex))) {
        return false;
    }

    const float dpr = static_cast<float>(
        std::isfinite(devicePixelRatio) ? std::max<qreal>(devicePixelRatio, 1.0)
                                        : 1.0);
    QVector<MarkerVertex> vertices;
    vertices.reserve(handles.size());
    for (const ViewportControlPointHandle &handle : handles) {
        if (!std::isfinite(handle.worldPosition.x()) ||
            !std::isfinite(handle.worldPosition.y()) ||
            !std::isfinite(handle.worldPosition.z())) {
            continue;
        }
        if (!handle.fillColor.isValid() || !handle.outlineColor.isValid() ||
            !std::isfinite(handle.diameterPixels) ||
            !std::isfinite(handle.outlineWidthPixels) ||
            handle.diameterPixels <= 0.0f || handle.outlineWidthPixels < 0.0f) {
            continue;
        }

        MarkerVertex vertex{};
        vertex.position[0] = handle.worldPosition.x();
        vertex.position[1] = handle.worldPosition.y();
        vertex.position[2] = handle.worldPosition.z();
        setColor(vertex.fill, handle.fillColor);
        setColor(vertex.outline, handle.outlineColor);
        vertex.diameterPixels = handle.diameterPixels;
        vertex.outlineWidthPixels = handle.outlineWidthPixels;
        vertex.shape = handle.shape == ViewportControlPointShape::Circle ? 0.0f
                                                                         : 1.0f;
        vertices.append(vertex);
    }
    if (vertices.isEmpty()) {
        return true;
    }

    GLfloat pointSizeRange[2] = {1.0f, 1.0f};
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, pointSizeRange);
    const float maximumPointSize = std::max(pointSizeRange[1], 1.0f);
    for (MarkerVertex &vertex : vertices) {
        vertex.diameterPixels = std::min(vertex.diameterPixels,
                                         maximumPointSize / dpr);
    }

    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    GLint previousBlendSrcRgb = GL_ONE;
    GLint previousBlendDstRgb = GL_ZERO;
    GLint previousBlendSrcAlpha = GL_ONE;
    GLint previousBlendDstAlpha = GL_ZERO;
    GLint previousBlendEquationRgb = GL_FUNC_ADD;
    GLint previousBlendEquationAlpha = GL_FUNC_ADD;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    const GLboolean pointSizeWasEnabled = glIsEnabled(GL_PROGRAM_POINT_SIZE);

    const qreal safeDpr = dpr;
    const GLsizei pixelWidth = static_cast<GLsizei>(
        std::max(1, qRound(viewportSize.width() * safeDpr)));
    const GLsizei pixelHeight = static_cast<GLsizei>(
        std::max(1, qRound(viewportSize.height() * safeDpr)));
    glViewport(0, 0, pixelWidth, pixelHeight);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);

    vertexArray_.bind();
    vertexBuffer_.bind();
    vertexBuffer_.allocate(vertices.constData(),
                           vertices.size() * static_cast<int>(sizeof(MarkerVertex)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MarkerVertex),
                          reinterpret_cast<const void *>(
                              offsetof(MarkerVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(MarkerVertex),
                          reinterpret_cast<const void *>(
                              offsetof(MarkerVertex, fill)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(MarkerVertex),
                          reinterpret_cast<const void *>(
                              offsetof(MarkerVertex, outline)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(MarkerVertex),
                          reinterpret_cast<const void *>(
                              offsetof(MarkerVertex, diameterPixels)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(MarkerVertex),
                          reinterpret_cast<const void *>(
                              offsetof(MarkerVertex, outlineWidthPixels)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(MarkerVertex),
                          reinterpret_cast<const void *>(
                              offsetof(MarkerVertex, shape)));

    bool drawn = false;
    if (program_.bind()) {
        program_.setUniformValue("uViewProjection",
                                 viewportViewProjection(transform, viewportSize));
        program_.setUniformValue("uDevicePixelRatio", dpr);
        glDrawArrays(GL_POINTS, 0, vertices.size());
        program_.release();
        drawn = true;
    }

    vertexBuffer_.release();
    vertexArray_.release();
    glUseProgram(static_cast<GLuint>(previousProgram));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glBlendFuncSeparate(static_cast<GLenum>(previousBlendSrcRgb),
                        static_cast<GLenum>(previousBlendDstRgb),
                        static_cast<GLenum>(previousBlendSrcAlpha),
                        static_cast<GLenum>(previousBlendDstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                            static_cast<GLenum>(previousBlendEquationAlpha));
    if (depthWasEnabled) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (blendWasEnabled) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (pointSizeWasEnabled) {
        glEnable(GL_PROGRAM_POINT_SIZE);
    } else {
        glDisable(GL_PROGRAM_POINT_SIZE);
    }
    return drawn;
}

bool ViewportControlPointRenderer::release()
{
    if (ownerContext_ == nullptr) {
        return true;
    }
    if (QOpenGLContext::currentContext() != ownerContext_) {
        return false;
    }

    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.destroy();
    }
    if (vertexArray_.isCreated()) {
        vertexArray_.destroy();
    }
    program_.removeAllShaders();
    initialized_ = false;
    ownerContext_ = nullptr;
    return true;
}

} // namespace classiCAD
