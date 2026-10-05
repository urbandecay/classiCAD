/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_surface_renderer.h"

#include "viewport_depth_geometry.h"
#include "blender_grid_renderer.h"
#include "workbench_lighting.h"

#include <QDataStream>
#include <QIODevice>
#include <QOpenGLContext>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace classiCAD {
namespace {

Point3D normalized(Point3D value, const Point3D &fallback)
{
    const qreal length = std::sqrt(value.x * value.x + value.y * value.y +
                                    value.z * value.z);
    if (!std::isfinite(length) || length <= 1.0e-12) {
        return fallback;
    }
    value.x /= length;
    value.y /= length;
    value.z /= length;
    return value;
}

Point3D cross(const Point3D &a, const Point3D &b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

QVector3D asVector(const Point3D &point)
{
    return {static_cast<float>(point.x),
            static_cast<float>(point.y),
            static_cast<float>(point.z)};
}

bool hasNurbsSurfaceGeometry(const ViewportRenderObject &object)
{
    return object.shape.geometryType == GeometryType::NurbsSurface ||
           object.shape.geometryType == GeometryType::NurbsSolid;
}

} // namespace

ViewportSurfaceRenderer::~ViewportSurfaceRenderer()
{
    if (QOpenGLContext::currentContext() == nullptr) {
        return;
    }
    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.destroy();
    }
    if (vertexArray_.isCreated()) {
        vertexArray_.destroy();
    }
    if (matcapTexture_ != 0) {
        glDeleteTextures(1, &matcapTexture_);
        matcapTexture_ = 0;
    }
    program_.removeAllShaders();
}

bool ViewportSurfaceRenderer::initialize()
{
    if (initializationAttempted_) {
        return initialized_;
    }
    initializationAttempted_ = true;
    static constexpr const char *vertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec3 aNormal;
        uniform mat4 uViewProjection;
        uniform vec3 uWorldOffset;
        out vec3 vNormalView;
        out vec3 vWorldPosition;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        void main()
        {
            vec3 position = aPosition + uWorldOffset;
            vNormalView = vec3(dot(aNormal, uViewRight),
                               dot(aNormal, uViewUp),
                               dot(aNormal, uViewFacing));
            vWorldPosition = position;
            gl_Position = uViewProjection * vec4(position, 1.0);
        }
    )glsl";
    static constexpr const char *fragmentShader = R"glsl(
        #version 330 core
        in vec3 vNormalView;
        in vec3 vWorldPosition;
        uniform vec4 uBaseColor;
        uniform vec3 uCameraPosition;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        uniform bool uPerspective;
        uniform int uLightingMode;
        uniform sampler2DArray uMatcapTexture;
        uniform bool uMatcapAvailable;
        uniform vec3 uLightDirection[4];
        uniform vec3 uLightDiffuse[4];
        uniform vec3 uLightSpecular[4];
        uniform float uLightWrap[4];
        uniform vec3 uAmbientColor;
        uniform float uRoughness;
        uniform float uMetallic;
        uniform bool uUseSpecular;
        out vec4 fragmentColor;

        float fastReciprocal(float value)
        {
            return intBitsToFloat(0x7eef370b - floatBitsToInt(value));
        }

        float wrappedLighting(float normalLight, float wrap)
        {
            float wrapPlusOne = wrap + 1.0;
            return clamp((normalLight + wrap) *
                         fastReciprocal(wrapPlusOne * wrapPlusOne),
                         0.0, 1.0);
        }

        vec3 workbenchStudioLighting(vec3 baseColor,
                                     float roughness,
                                     float metallic,
                                     vec3 normal,
                                     vec3 incident)
        {
            vec3 diffuseColor;
            vec3 specularColor;
            if (uUseSpecular) {
                diffuseColor = mix(baseColor, vec3(0.0), metallic);
                specularColor = mix(vec3(0.05), baseColor, metallic);
            } else {
                diffuseColor = baseColor;
                specularColor = vec3(0.0);
            }

            vec3 specularLight = uAmbientColor;
            vec3 diffuseLight = uAmbientColor;
            vec3 reflectedIncident = -reflect(incident, normal);

            if (uUseSpecular) {
                for (int index = 0; index < 4; ++index) {
                    vec3 light = uLightDirection[index];
                    vec3 halfDirection = normalize(light + incident);
                    float wrappedNormalLight = dot(light, reflectedIncident);
                    float specularAngle = clamp(dot(halfDirection, normal),
                                               0.0, 1.0);
                    float specularNormalLight = clamp(dot(light, normal),
                                                      0.0, 1.0);
                    float gloss = (1.0 - roughness) *
                                  (1.0 - uLightWrap[index]);
                    float shininess = exp2(10.0 * gloss + 1.0);
                    float specularAmount = pow(specularAngle, shininess) *
                        specularNormalLight * (shininess * 0.125 + 1.0);
                    float environmentWrap = mix(uLightWrap[index], 1.0,
                                                roughness);
                    float environment = wrappedLighting(wrappedNormalLight,
                                                        environmentWrap);
                    specularAmount = mix(specularAmount, environment,
                                         uLightWrap[index] * uLightWrap[index]);
                    specularLight += specularAmount * uLightSpecular[index];
                }
                float normalView = clamp(dot(normal, incident), 0.0, 1.0);
                float fresnel = exp2(-8.35 * normalView) * (1.0 - roughness);
                specularColor = mix(specularColor, vec3(1.0), fresnel);
                specularLight *= specularColor;
            }

            for (int index = 0; index < 4; ++index) {
                float normalLight = dot(uLightDirection[index], normal);
                diffuseLight += wrappedLighting(normalLight, uLightWrap[index]) *
                                uLightDiffuse[index];
            }

            if (uUseSpecular) {
                float specularEnergy = dot(specularColor, vec3(0.33333));
                diffuseLight *= diffuseColor * (1.0 - specularEnergy);
            } else {
                diffuseLight *= diffuseColor;
            }
            return diffuseLight + specularLight;
        }

        vec3 sceneLinearToSrgb(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color * 12.92;
            vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
            return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
        }

        vec2 workbenchMatcapUv(vec3 incident, vec3 normal)
        {
            float a = 1.0 / (1.0 + incident.z);
            float b = -incident.x * incident.y * a;
            vec3 basis1 = vec3(1.0 - incident.x * incident.x * a,
                               b,
                               -incident.x);
            vec3 basis2 = vec3(b,
                               1.0 - incident.y * incident.y * a,
                               -incident.y);
            return vec2(dot(basis1, normal), dot(basis2, normal)) * 0.496 + 0.5;
        }

        void main()
        {
            vec3 normal = normalize(vNormalView);
            vec3 incident = vec3(0.0, 0.0, 1.0);
            if (uPerspective) {
                vec3 incidentWorld = normalize(uCameraPosition - vWorldPosition);
                // Studio lights and normals share the same camera-following
                // view-space frame used by Blender Workbench.
                incident = normalize(vec3(dot(incidentWorld, uViewRight),
                                          dot(incidentWorld, uViewUp),
                                          dot(incidentWorld, uViewFacing)));
            }
            vec3 color = uBaseColor.rgb;
            if (uLightingMode == 0) {
                color = workbenchStudioLighting(uBaseColor.rgb,
                                                uRoughness,
                                                uMetallic,
                                                normal,
                                                incident);
            } else if (uLightingMode == 1 && uMatcapAvailable) {
                vec2 uv = workbenchMatcapUv(incident, normal);
                vec3 diffuse = texture(uMatcapTexture, vec3(uv, 0.0)).rgb;
                vec3 specular = texture(uMatcapTexture, vec3(uv, 1.0)).rgb;
                color = diffuse * uBaseColor.rgb + specular;
            }
            color = sceneLinearToSrgb(color);
            fragmentColor = vec4(color,
                                 uBaseColor.a);
        }
    )glsl";

    if (QOpenGLContext::currentContext() == nullptr ||
        !initializeOpenGLFunctions() ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader) ||
        !program_.link() || !vertexArray_.create() || !vertexBuffer_.create()) {
        qWarning().noquote() << "Viewport NURBS surface shader setup failed:"
                             << program_.log();
        return false;
    }

    vertexArray_.bind();
    vertexBuffer_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, nx)));
    vertexBuffer_.release();
    vertexArray_.release();
    initialized_ = true;
    return true;
}

void ViewportSurfaceRenderer::prepareGeometry(
    const QVector<ViewportRenderObject> &objects)
{
    QByteArray key;
    QDataStream keyStream(&key, QIODevice::WriteOnly);
    keyStream << qint32(objects.size());
    for (const ViewportRenderObject &object : objects) {
        const bool hasSurface = hasNurbsSurfaceGeometry(object);
        const ViewportDepthGeometry *geometry =
            hasSurface ? object.preparedDepthGeometry.data() : nullptr;
        keyStream << quint64(object.objectId.value())
                  << quint64(object.geometryRevision)
                  << quint8(hasSurface ? 1 : 0)
                  << quint64(reinterpret_cast<quintptr>(geometry))
                  << qint32(geometry != nullptr
                                ? geometry->surfaceVertices.size()
                                : 0);
    }
    const bool changed = key != geometryKey_;
    ranges_.clear();
    ranges_.reserve(objects.size());
    if (!changed) {
        for (const ViewportRenderObject &object : objects) {
            const ViewportDepthGeometry *geometry =
                hasNurbsSurfaceGeometry(object)
                    ? object.preparedDepthGeometry.data()
                    : nullptr;
            ranges_.append({ranges_.isEmpty()
                                ? 0
                                : ranges_.last().first + ranges_.last().count,
                            geometry != nullptr
                                ? static_cast<int>(std::min(
                                      geometry->surfaceVertices.size(),
                                      geometry->surfaceNormals.size()))
                                : 0,
                            object.preparedGeometryOffset,
                            object.selected});
        }
        return;
    }

    geometryKey_ = std::move(key);
    geometryDirty_ = true;
    vertices_.clear();
    for (const ViewportRenderObject &object : objects) {
        const ViewportDepthGeometry *geometry =
            hasNurbsSurfaceGeometry(object)
                ? object.preparedDepthGeometry.data()
                : nullptr;
        const int first = vertices_.size();
        const int count = geometry != nullptr
                              ? static_cast<int>(std::min(
                                    geometry->surfaceVertices.size(),
                                    geometry->surfaceNormals.size()))
                              : 0;
        for (int index = 0; index < count; ++index) {
            const QVector3D &position = geometry->surfaceVertices[index];
            const QVector3D &normal = geometry->surfaceNormals[index];
            vertices_.append({position.x(), position.y(), position.z(),
                              normal.x(), normal.y(), normal.z()});
        }
        ranges_.append({first, count, object.preparedGeometryOffset,
                        object.selected});
    }
}

bool ViewportSurfaceRenderer::ensureMatcapTexture(const QString &presetName)
{
    if (matcapTexture_ != 0 && matcapTextureName_ == presetName) {
        return true;
    }
    QImage diffuseImage = workbenchMatcapDiffuseImage(presetName);
    QImage specularImage = workbenchMatcapSpecularImage(presetName);
    if (diffuseImage.isNull() || specularImage.isNull() ||
        diffuseImage.size() != specularImage.size()) {
        return false;
    }
    diffuseImage = diffuseImage.convertToFormat(QImage::Format_RGBA8888)
                       .mirrored(false, true);
    specularImage = specularImage.convertToFormat(QImage::Format_RGBA8888)
                        .mirrored(false, true);
    QByteArray texturePixels;
    texturePixels.reserve(static_cast<qsizetype>(diffuseImage.sizeInBytes() +
                                                 specularImage.sizeInBytes()));
    texturePixels.append(reinterpret_cast<const char *>(diffuseImage.constBits()),
                         static_cast<qsizetype>(diffuseImage.sizeInBytes()));
    texturePixels.append(reinterpret_cast<const char *>(specularImage.constBits()),
                         static_cast<qsizetype>(specularImage.sizeInBytes()));

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousBinding);
    if (matcapTexture_ == 0) {
        glGenTextures(1, &matcapTexture_);
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, matcapTexture_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8,
                 diffuseImage.width(), diffuseImage.height(), 2, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texturePixels.constData());
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(previousBinding));
    matcapTextureName_ = presetName;
    return matcapTexture_ != 0;
}

bool ViewportSurfaceRenderer::draw(
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    bool previewOverlay,
    bool clearDepth)
{
    if (QOpenGLContext::currentContext() == nullptr || viewportSize.isEmpty() ||
        !initialize()) {
        return false;
    }

    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint previousDepthFunction = GL_LESS;
    GLint previousBlendSourceRgb = GL_ONE;
    GLint previousBlendDestinationRgb = GL_ZERO;
    GLint previousBlendSourceAlpha = GL_ONE;
    GLint previousBlendDestinationAlpha = GL_ZERO;
    GLint previousBlendEquationRgb = GL_FUNC_ADD;
    GLint previousBlendEquationAlpha = GL_FUNC_ADD;
    GLint previousProgram = 0;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSourceRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDestinationRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSourceAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDestinationAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);

    if (clearDepth) {
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    prepareGeometry(objects);
    if (settings.mode == ViewportShadingMode::Wireframe &&
        !settings.xrayEnabled()) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else if (settings.mode == ViewportShadingMode::Solid) {
        const bool xray = previewOverlay || settings.xrayEnabled();
        if (xray) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
    }

    const bool drawColor = settings.mode == ViewportShadingMode::Solid;
    if (!vertices_.isEmpty() &&
        (drawColor || !settings.xrayEnabled())) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        glViewport(0, 0, qRound(viewportSize.width() * dpr),
                   qRound(viewportSize.height() * dpr));
        const QMatrix4x4 viewProjection =
            viewportViewProjection(transform, viewportSize);
        program_.bind();
        program_.setUniformValue("uViewProjection", viewProjection);
        const Point3D viewDirection =
            normalized(transform.viewDirection(), {0.0, 0.0, 1.0});
        const Point3D viewUp = normalized(transform.viewUp(), {0.0, 1.0, 0.0});
        const Point3D viewRight = normalized(cross(viewUp, viewDirection),
                                             {1.0, 0.0, 0.0});
        const Point3D viewFacing = viewDirection;
        const Point3D cameraPosition = transform.cameraPosition(viewportSize);
        program_.setUniformValue("uViewRight", asVector(viewRight));
        program_.setUniformValue("uViewUp", asVector(viewUp));
        program_.setUniformValue("uViewFacing", asVector(viewFacing));
        program_.setUniformValue("uCameraPosition", asVector(cameraPosition));
        program_.setUniformValue("uPerspective",
                                 transform.isPerspectiveEnabled());
        const WorkbenchStudioLighting &lighting =
            settings.hasCustomStudioLighting
                ? settings.customStudioLighting
                : workbenchStudioLightingPreset(settings.studioLightPreset);
        const WorkbenchStudioLighting viewLighting =
            workbenchStudioLightingForView(
                lighting, settings.studioLightRotationDegrees,
                settings.worldSpaceLighting, asVector(
                    Point3D{viewRight.x, viewRight.y, viewRight.z}),
                asVector(Point3D{viewUp.x, viewUp.y, viewUp.z}),
                asVector(Point3D{viewFacing.x, viewFacing.y, viewFacing.z}));
        QVector3D lightDirections[4];
        QVector3D lightDiffuse[4];
        QVector3D lightSpecular[4];
        float lightWrap[4];
        for (int index = 0; index < 4; ++index) {
            const WorkbenchStudioLight &light = viewLighting.lights[index];
            lightDirections[index] = light.enabled
                                         ? light.direction
                                         : QVector3D(1.0f, 0.0f, 0.0f);
            lightDiffuse[index] = light.enabled
                                      ? light.diffuseColor
                                      : QVector3D();
            lightSpecular[index] = light.enabled
                                       ? light.specularColor
                                       : QVector3D();
            lightWrap[index] = light.enabled ? light.wrap : 0.0f;
        }
        program_.setUniformValueArray("uLightDirection", lightDirections, 4);
        program_.setUniformValueArray("uLightDiffuse", lightDiffuse, 4);
        program_.setUniformValueArray("uLightSpecular", lightSpecular, 4);
        program_.setUniformValueArray("uLightWrap[0]", lightWrap, 4, 1);
        program_.setUniformValue("uAmbientColor", lighting.ambientColor);
        // Workbench's object-color fallback packs source roughness 0.4 through
        // sqrt/8-bit quantization before its resolve shader reads it.
        program_.setUniformValue("uRoughness", 161.0f / 255.0f);
        program_.setUniformValue("uMetallic", 0.0f);
        program_.setUniformValue("uUseSpecular", lighting.useSpecular);
        const int lightingMode = settings.lightingMode == ViewportLightingMode::Studio
                                     ? 0
                                     : settings.lightingMode == ViewportLightingMode::MatCap
                                           ? 1
                                           : 2;
        program_.setUniformValue("uLightingMode", lightingMode);
        const bool matcapAvailable =
            settings.lightingMode == ViewportLightingMode::MatCap &&
            ensureMatcapTexture(settings.matcapPreset);
        program_.setUniformValue("uMatcapAvailable", matcapAvailable);
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousTextureBinding = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        if (matcapAvailable) {
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,
                          &previousTextureBinding);
            glBindTexture(GL_TEXTURE_2D_ARRAY, matcapTexture_);
            program_.setUniformValue("uMatcapTexture", 0);
        }
        vertexArray_.bind();
        vertexBuffer_.bind();
        if (geometryDirty_) {
            vertexBuffer_.allocate(vertices_.constData(),
                                   vertices_.size() *
                                       static_cast<int>(sizeof(SurfaceVertex)));
            geometryDirty_ = false;
        }
        for (const DrawRange &range : ranges_) {
            if (range.count <= 0) {
                continue;
            }
            const QVector3D offset(static_cast<float>(range.offset.x),
                                   static_cast<float>(range.offset.y),
                                   static_cast<float>(range.offset.z));
            program_.setUniformValue("uWorldOffset", offset);
            const QColor baseColor = previewOverlay
                                         ? QColor(QStringLiteral("#d89a4e"))
                                         : range.selected
                                               ? QColor(QStringLiteral("#6d9fc7"))
                                               : QColor(Qt::white);
            const float alpha = (previewOverlay || settings.xrayEnabled())
                                    ? 0.5f
                                    : 1.0f;
            const QVector3D baseColorLinear = previewOverlay || range.selected
                ? workbenchSrgbToSceneLinear(
                      QVector3D(baseColor.redF(), baseColor.greenF(),
                                baseColor.blueF()))
                : workbenchDefaultSolidMaterialDiffuseColor();
            program_.setUniformValue(
                "uBaseColor",
                QVector4D(baseColorLinear.x(), baseColorLinear.y(),
                          baseColorLinear.z(), alpha));
            glDrawArrays(GL_TRIANGLES, range.first, range.count);
        }
        vertexBuffer_.release();
        vertexArray_.release();
        program_.release();
        if (matcapAvailable) {
            glBindTexture(GL_TEXTURE_2D_ARRAY,
                          static_cast<GLuint>(previousTextureBinding));
            glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        }
    }

    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (previousBlend) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                            static_cast<GLenum>(previousBlendEquationAlpha));
    glBlendFuncSeparate(static_cast<GLenum>(previousBlendSourceRgb),
                        static_cast<GLenum>(previousBlendDestinationRgb),
                        static_cast<GLenum>(previousBlendSourceAlpha),
                        static_cast<GLenum>(previousBlendDestinationAlpha));
    glUseProgram(static_cast<GLuint>(previousProgram));
    return true;
}

} // namespace classiCAD
