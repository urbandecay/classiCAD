/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_surface_renderer.h"

#include "viewport_depth_geometry.h"
#include "blender_grid_renderer.h"
#include "viewport_line_weight.h"
#include "workbench_lighting.h"

#include <QDataStream>
#include <QIODevice>
#include <QOpenGLContext>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>

namespace classiCAD {
namespace {

constexpr int kWorkbenchShadowMapSize = 1024;

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
    if (outlineVertexArray_.isCreated()) {
        outlineVertexArray_.destroy();
    }
    if (vertexArray_.isCreated()) {
        vertexArray_.destroy();
    }
    if (matcapTexture_ != 0) {
        glDeleteTextures(1, &matcapTexture_);
        matcapTexture_ = 0;
    }
    if (agxDisplayTexture_ != 0) {
        glDeleteTextures(1, &agxDisplayTexture_);
        agxDisplayTexture_ = 0;
    }
    if (shadowTexture_ != 0) {
        glDeleteTextures(1, &shadowTexture_);
        shadowTexture_ = 0;
    }
    if (shadowFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &shadowFramebuffer_);
        shadowFramebuffer_ = 0;
    }
    if (objectIdFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &objectIdFramebuffer_);
        objectIdFramebuffer_ = 0;
    }
    if (objectIdDepthStencil_ != 0) {
        glDeleteRenderbuffers(1, &objectIdDepthStencil_);
        objectIdDepthStencil_ = 0;
    }
    if (objectIdTexture_ != 0) {
        glDeleteTextures(1, &objectIdTexture_);
        objectIdTexture_ = 0;
    }
    program_.removeAllShaders();
    outlineProgram_.removeAllShaders();
    objectIdProgram_.removeAllShaders();
    shadowProgram_.removeAllShaders();
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
        uniform mat4 uShadowViewProjection;
        out vec3 vNormalView;
        out vec3 vWorldPosition;
        out vec4 vShadowPosition;
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
            vShadowPosition = uShadowViewProjection * vec4(position, 1.0);
            gl_Position = uViewProjection * vec4(position, 1.0);
        }
    )glsl";
    static constexpr const char *fragmentShader = R"glsl(
        #version 330 core
        in vec3 vNormalView;
        in vec3 vWorldPosition;
        in vec4 vShadowPosition;
        uniform vec4 uBaseColor;
        uniform bool uEditModeSelected;
        uniform vec3 uEditSelectionColor;
        uniform float uEditSelectionMix;
        uniform vec3 uCameraPosition;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        uniform bool uPerspective;
        uniform int uLightingMode;
        uniform sampler2DArray uMatcapTexture;
        uniform bool uMatcapAvailable;
        uniform sampler3D uAgxDisplayTexture;
        uniform bool uAgxDisplayAvailable;
        uniform vec3 uLightDirection[4];
        uniform vec3 uLightDiffuse[4];
        uniform vec3 uLightSpecular[4];
        uniform float uLightWrap[4];
        uniform vec3 uAmbientColor;
        uniform float uRoughness;
        uniform float uMetallic;
        uniform bool uUseSpecular;
        uniform sampler2DShadow uShadowMap;
        uniform bool uUseShadows;
        uniform float uShadowIntensity;
        uniform float uShadowBias;
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

        vec3 srgbToDisplayLinear(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color / 12.92;
            vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
            return mix(high, low, lessThanEqual(color, vec3(0.04045)));
        }

        vec3 sceneLinearToAgxSrgb(vec3 color)
        {
            if (!uAgxDisplayAvailable) {
                return sceneLinearToSrgb(color);
            }
            vec3 bounded = clamp(color, vec3(0.0), vec3(2.0));
            vec3 coordinate = (bounded * 32.0 + 0.5) / 65.0;
            return texture(uAgxDisplayTexture, coordinate).rgb;
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
                vec3 specular = uUseSpecular
                                    ? texture(uMatcapTexture,
                                              vec3(uv, 1.0)).rgb
                                    : vec3(0.0);
                color = diffuse * uBaseColor.rgb + specular;
            }
            if (uUseShadows) {
                vec3 shadowCoordinate =
                    vShadowPosition.xyz / vShadowPosition.w * 0.5 + 0.5;
                float visibility = 1.0;
                if (all(greaterThanEqual(shadowCoordinate.xy, vec2(0.0))) &&
                    all(lessThanEqual(shadowCoordinate.xy, vec2(1.0))) &&
                    shadowCoordinate.z >= 0.0 && shadowCoordinate.z <= 1.0) {
                    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
                    visibility = 0.0;
                    for (int y = -1; y <= 1; ++y) {
                        for (int x = -1; x <= 1; ++x) {
                            visibility += texture(
                                uShadowMap,
                                vec3(shadowCoordinate.xy + vec2(x, y) * texel,
                                     shadowCoordinate.z - uShadowBias));
                        }
                    }
                    visibility /= 9.0;
                }
                color *= mix(1.0, visibility,
                             clamp(uShadowIntensity, 0.0, 1.0));
            }
            color = sceneLinearToAgxSrgb(color);
            // Blender composites Edit Mode overlays after the view transform,
            // in linear display space.
            if (uEditModeSelected) {
                vec3 displayLinear = srgbToDisplayLinear(color);
                color = sceneLinearToSrgb(
                    mix(displayLinear, uEditSelectionColor,
                        uEditSelectionMix));
            }
            fragmentColor = vec4(color,
                                 uBaseColor.a);
        }
    )glsl";

    static constexpr const char *objectIdVertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        uniform mat4 uViewProjection;
        uniform vec3 uWorldOffset;
        void main()
        {
            gl_Position = uViewProjection *
                          vec4(aPosition + uWorldOffset, 1.0);
        }
    )glsl";
    static constexpr const char *objectIdFragmentShader = R"glsl(
        #version 330 core
        uniform uint uObjectId;
        layout(location = 0) out uint fragmentObjectId;
        void main() { fragmentObjectId = uObjectId; }
    )glsl";
    static constexpr const char *outlineVertexShader = R"glsl(
        #version 330 core
        void main()
        {
            vec2 positions[3] = vec2[3](vec2(-1.0, -1.0),
                                         vec2(3.0, -1.0),
                                         vec2(-1.0, 3.0));
            gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
        }
    )glsl";
    static constexpr const char *outlineFragmentShader = R"glsl(
        #version 330 core
        uniform usampler2D uObjectIdBuffer;
        uniform vec4 uOutlineColor;
        uniform vec4 uSelectionColor;
        uniform int uPixelOffset;
        layout(location = 0) out vec4 fragmentColor;
        void main()
        {
            ivec2 size = textureSize(uObjectIdBuffer, 0);
            ivec2 pixel = ivec2(gl_FragCoord.xy);
            ivec2 dx = ivec2(uPixelOffset, 0);
            ivec2 dy = ivec2(0, uPixelOffset);
            uint centerId = texelFetch(uObjectIdBuffer, pixel, 0).r;
            uvec4 adjacentIds = uvec4(
                texelFetch(uObjectIdBuffer, clamp(pixel + dx, ivec2(0), size - 1), 0).r,
                texelFetch(uObjectIdBuffer, clamp(pixel - dx, ivec2(0), size - 1), 0).r,
                texelFetch(uObjectIdBuffer, clamp(pixel + dy, ivec2(0), size - 1), 0).r,
                texelFetch(uObjectIdBuffer, clamp(pixel - dy, ivec2(0), size - 1), 0).r);
            float opacity = 1.0 - dot(vec4(equal(uvec4(centerId), adjacentIds)),
                                     vec4(0.25));
            if (opacity <= 0.0) {
                discard;
            }
            const uint selectedBit = 0x80000000u;
            bool selected = (centerId & selectedBit) != 0u;
            for (int index = 0; index < 4; ++index) {
                selected = selected || ((adjacentIds[index] & selectedBit) != 0u);
            }
            vec4 color = selected ? uSelectionColor : uOutlineColor;
            fragmentColor = vec4(color.rgb, color.a * opacity);
        }
    )glsl";
    static constexpr const char *shadowVertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        uniform mat4 uShadowViewProjection;
        uniform vec3 uWorldOffset;
        void main()
        {
            gl_Position = uShadowViewProjection *
                          vec4(aPosition + uWorldOffset, 1.0);
        }
    )glsl";
    static constexpr const char *shadowFragmentShader = R"glsl(
        #version 330 core
        void main() { }
    )glsl";

    if (QOpenGLContext::currentContext() == nullptr ||
        !initializeOpenGLFunctions() ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader) ||
        !program_.link() || !vertexArray_.create() || !vertexBuffer_.create() ||
        !objectIdProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                  objectIdVertexShader) ||
        !objectIdProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                  objectIdFragmentShader) ||
        !objectIdProgram_.link() ||
        !outlineProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                 outlineVertexShader) ||
        !outlineProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                 outlineFragmentShader) ||
        !outlineProgram_.link() || !outlineVertexArray_.create() ||
        !shadowProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                shadowVertexShader) ||
        !shadowProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                shadowFragmentShader) ||
        !shadowProgram_.link()) {
        qWarning().noquote()
            << "Viewport shader setup failed. Surface:" << program_.log()
            << "Object ID:" << objectIdProgram_.log()
            << "Outline:" << outlineProgram_.log()
            << "Shadow:" << shadowProgram_.log();
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
        for (int index = 0; index < objects.size(); ++index) {
            const ViewportRenderObject &object = objects[index];
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
                            object.selected,
                            object.layerColor,
                            object.objectId.value(),
                            object.outlineHandledBySceneStroke});
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
                        object.selected, object.layerColor,
                        object.objectId.value(),
                        object.outlineHandledBySceneStroke});
    }
}

bool ViewportSurfaceRenderer::ensureShadowMap()
{
    if (shadowTexture_ != 0 && shadowFramebuffer_ != 0) {
        return true;
    }
    GLint previousTexture = 0;
    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGenTextures(1, &shadowTexture_);
    glBindTexture(GL_TEXTURE_2D, shadowTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 kWorkbenchShadowMapSize, kWorkbenchShadowMapSize,
                 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE,
                    GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    const GLfloat borderColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glGenFramebuffers(1, &shadowFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, shadowTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                          GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(previousFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    if (!complete) {
        qWarning() << "Workbench shadow depth framebuffer is incomplete";
        glDeleteFramebuffers(1, &shadowFramebuffer_);
        glDeleteTextures(1, &shadowTexture_);
        shadowFramebuffer_ = 0;
        shadowTexture_ = 0;
    }
    return complete;
}

bool ViewportSurfaceRenderer::ensureObjectIdFramebuffer(
    const QSize &pixelSize)
{
    if (objectIdFramebufferAttempted_ &&
        objectIdFramebufferSize_ == pixelSize) {
        return objectIdFramebuffer_ != 0;
    }

    if (objectIdFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &objectIdFramebuffer_);
        objectIdFramebuffer_ = 0;
    }
    if (objectIdDepthStencil_ != 0) {
        glDeleteRenderbuffers(1, &objectIdDepthStencil_);
        objectIdDepthStencil_ = 0;
    }
    if (objectIdTexture_ != 0) {
        glDeleteTextures(1, &objectIdTexture_);
        objectIdTexture_ = 0;
    }

    objectIdFramebufferAttempted_ = true;
    objectIdFramebufferSize_ = pixelSize;

    GLint previousFramebuffer = 0;
    GLint previousRenderbuffer = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glGenTextures(1, &objectIdTexture_);
    glBindTexture(GL_TEXTURE_2D, objectIdTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &objectIdDepthStencil_);
    glBindRenderbuffer(GL_RENDERBUFFER, objectIdDepthStencil_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          pixelSize.width(), pixelSize.height());

    glGenFramebuffers(1, &objectIdFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, objectIdFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, objectIdTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, objectIdDepthStencil_);
    const GLenum colorAttachment = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &colorAttachment);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(previousFramebuffer));
    glBindRenderbuffer(GL_RENDERBUFFER,
                       static_cast<GLuint>(previousRenderbuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Object-ID outline framebuffer is incomplete; status:"
                   << Qt::hex << status;
        glDeleteFramebuffers(1, &objectIdFramebuffer_);
        glDeleteRenderbuffers(1, &objectIdDepthStencil_);
        glDeleteTextures(1, &objectIdTexture_);
        objectIdFramebuffer_ = 0;
        objectIdDepthStencil_ = 0;
        objectIdTexture_ = 0;
        return false;
    }
    return true;
}

bool ViewportSurfaceRenderer::renderShadowMap(
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const ViewportShadingSettings &settings)
{
    if (vertices_.isEmpty() || !ensureShadowMap()) {
        return false;
    }

    QVector3D minimum(std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum(std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest());
    bool hasVertex = false;
    for (const DrawRange &range : ranges_) {
        const QVector3D offset(static_cast<float>(range.offset.x),
                               static_cast<float>(range.offset.y),
                               static_cast<float>(range.offset.z));
        for (int index = range.first; index < range.first + range.count; ++index) {
            const SurfaceVertex &vertex = vertices_[index];
            const QVector3D position(vertex.x, vertex.y, vertex.z);
            minimum.setX(std::min(minimum.x(), position.x() + offset.x()));
            minimum.setY(std::min(minimum.y(), position.y() + offset.y()));
            minimum.setZ(std::min(minimum.z(), position.z() + offset.z()));
            maximum.setX(std::max(maximum.x(), position.x() + offset.x()));
            maximum.setY(std::max(maximum.y(), position.y() + offset.y()));
            maximum.setZ(std::max(maximum.z(), position.z() + offset.z()));
            hasVertex = true;
        }
    }
    if (!hasVertex) {
        return false;
    }

    const QVector3D center = (minimum + maximum) * 0.5f;
    float radius = std::max((maximum - minimum).length() * 0.5f, 0.5f);
    radius *= 1.0f + std::clamp(static_cast<float>(settings.shadowFocus),
                                0.0f, 1.0f);
    QVector3D lightDirection = settings.shadowDirection;
    if (lightDirection.lengthSquared() <= 1.0e-10f) {
        lightDirection = QVector3D(0.57735026f, 0.57735026f, 0.57735026f);
    }
    lightDirection.normalize();
    const QVector3D eye = center + lightDirection * (radius * 2.5f);
    QVector3D up(0.0f, 1.0f, 0.0f);
    if (std::abs(QVector3D::dotProduct(lightDirection, up)) > 0.95f) {
        up = QVector3D(1.0f, 0.0f, 0.0f);
    }
    QMatrix4x4 lightView;
    lightView.lookAt(eye, center, up);
    QMatrix4x4 lightProjection;
    lightProjection.ortho(-radius, radius, -radius, radius,
                          0.01f, radius * 6.0f);
    shadowViewProjection_ = lightProjection * lightView;

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousDepthFunction = GL_LESS;
    GLint previousProgram = 0;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previousDepthMask = GL_TRUE;
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glViewport(0, 0, kWorkbenchShadowMapSize, kWorkbenchShadowMapSize);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);

    vertexArray_.bind();
    vertexBuffer_.bind();
    if (geometryDirty_) {
        vertexBuffer_.allocate(vertices_.constData(),
                               vertices_.size() *
                                   static_cast<int>(sizeof(SurfaceVertex)));
        geometryDirty_ = false;
    }
    shadowProgram_.bind();
    shadowProgram_.setUniformValue("uShadowViewProjection",
                                   shadowViewProjection_);
    for (const DrawRange &range : ranges_) {
        if (range.count <= 0) {
            continue;
        }
        shadowProgram_.setUniformValue(
            "uWorldOffset",
            QVector3D(static_cast<float>(range.offset.x),
                      static_cast<float>(range.offset.y),
                      static_cast<float>(range.offset.z)));
        glDrawArrays(GL_TRIANGLES, range.first, range.count);
    }
    shadowProgram_.release();
    vertexBuffer_.release();
    vertexArray_.release();

    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(previousFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glUseProgram(static_cast<GLuint>(previousProgram));
    Q_UNUSED(transform);
    Q_UNUSED(viewportSize);
    return true;
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

bool ViewportSurfaceRenderer::ensureAgxDisplayTexture()
{
    if (agxDisplayTexture_ != 0) {
        return true;
    }
    const QVector<float> &lut = workbenchAgxDisplayLutData();
    if (lut.size() != workbenchAgxDisplayLutSize *
                          workbenchAgxDisplayLutSize *
                          workbenchAgxDisplayLutSize * 3) {
        return false;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &previousBinding);
    glGenTextures(1, &agxDisplayTexture_);
    glBindTexture(GL_TEXTURE_3D, agxDisplayTexture_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F,
                 workbenchAgxDisplayLutSize,
                 workbenchAgxDisplayLutSize,
                 workbenchAgxDisplayLutSize, 0, GL_RGB, GL_FLOAT,
                 lut.constData());
    glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(previousBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    return agxDisplayTexture_ != 0;
}

bool ViewportSurfaceRenderer::draw(
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    bool previewOverlay,
    bool clearDepth,
    bool selectionOverlay)
{
    if (QOpenGLContext::currentContext() == nullptr || viewportSize.isEmpty() ||
        !initialize()) {
        return false;
    }

    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
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
    const bool shadowMapAvailable =
        settings.mode == ViewportShadingMode::Solid && settings.shadows &&
        !settings.xrayEnabled() && !previewOverlay &&
        renderShadowMap(transform, viewportSize, settings);
    if (settings.mode == ViewportShadingMode::Solid &&
        settings.backfaceCulling && !settings.xrayEnabled()) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (selectionOverlay) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else if (settings.mode == ViewportShadingMode::Wireframe &&
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

    // Solid X-Ray keeps the faces translucent, but their depth still defines
    // which scene strokes are behind a surface. Without this depth-only pass,
    // the later stroke pass draws back edges through every face.
    const bool xraySurfaceDepthAvailable =
        settings.mode == ViewportShadingMode::Solid &&
        settings.xrayEnabled() && !previewOverlay && !vertices_.isEmpty();
    if (xraySurfaceDepthAvailable) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        glViewport(0, 0, qRound(viewportSize.width() * dpr),
                   qRound(viewportSize.height() * dpr));
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        vertexArray_.bind();
        vertexBuffer_.bind();
        if (geometryDirty_) {
            vertexBuffer_.allocate(vertices_.constData(),
                                   vertices_.size() *
                                       static_cast<int>(sizeof(SurfaceVertex)));
            geometryDirty_ = false;
        }
        shadowProgram_.bind();
        shadowProgram_.setUniformValue(
            "uShadowViewProjection",
            viewportViewProjection(transform, viewportSize));
        for (const DrawRange &range : ranges_) {
            if (range.count <= 0) {
                continue;
            }
            shadowProgram_.setUniformValue(
                "uWorldOffset",
                QVector3D(static_cast<float>(range.offset.x),
                          static_cast<float>(range.offset.y),
                          static_cast<float>(range.offset.z)));
            glDrawArrays(GL_TRIANGLES, range.first, range.count);
        }
        shadowProgram_.release();
        vertexBuffer_.release();
        vertexArray_.release();

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
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
            workbenchStudioLightingPreset(settings.studioLightPreset);
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
        program_.setUniformValue("uUseSpecular",
                                 settings.specularLighting &&
                                     lighting.useSpecular);
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
        const bool agxDisplayAvailable = ensureAgxDisplayTexture();
        program_.setUniformValue("uAgxDisplayAvailable", agxDisplayAvailable);
        program_.setUniformValue("uUseShadows", shadowMapAvailable);
        program_.setUniformValue("uShadowIntensity",
                                 static_cast<float>(settings.shadowIntensity));
        program_.setUniformValue(
            "uShadowBias",
            0.0005f + static_cast<float>(settings.shadowOffset) * 0.003f);
        program_.setUniformValue("uShadowViewProjection",
                                 shadowViewProjection_);
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousMatcapTextureBinding = 0;
        GLint previousAgxTextureBinding = 0;
        GLint previousShadowTextureBinding = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        if (matcapAvailable) {
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,
                          &previousMatcapTextureBinding);
            glBindTexture(GL_TEXTURE_2D_ARRAY, matcapTexture_);
            program_.setUniformValue("uMatcapTexture", 0);
        }
        glActiveTexture(GL_TEXTURE1);
        if (agxDisplayAvailable) {
            glGetIntegerv(GL_TEXTURE_BINDING_3D,
                          &previousAgxTextureBinding);
            glBindTexture(GL_TEXTURE_3D, agxDisplayTexture_);
            program_.setUniformValue("uAgxDisplayTexture", 1);
        }
        glActiveTexture(GL_TEXTURE2);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousShadowTextureBinding);
        if (shadowMapAvailable) {
            glBindTexture(GL_TEXTURE_2D, shadowTexture_);
        }
        program_.setUniformValue("uShadowMap", 2);
        // Blender 5.2's 3D View face_select color is #FFA300 with alpha 0x33.
        const QColor editSelectionColor(QStringLiteral("#ffa300"));
        // Theme colors are sRGB; Blender's overlay framebuffer blends in
        // linear display space after the view transform.
        const QVector3D editSelectionLinear = workbenchSrgbToSceneLinear(
            QVector3D(editSelectionColor.redF(), editSelectionColor.greenF(),
                      editSelectionColor.blueF()));
        program_.setUniformValue(
            "uEditSelectionColor",
            editSelectionLinear);
        program_.setUniformValue("uEditSelectionMix", 51.0f / 255.0f);
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
            const quint32 objectSeed =
                static_cast<quint32>(range.objectSeed);
            const QVector3D offset(static_cast<float>(range.offset.x),
                                   static_cast<float>(range.offset.y),
                                   static_cast<float>(range.offset.z));
            program_.setUniformValue("uWorldOffset", offset);
            program_.setUniformValue("uEditModeSelected",
                                     range.selected && !previewOverlay);
            const QColor baseColor = previewOverlay
                                         ? QColor(QStringLiteral("#d89a4e"))
                                         : settings.colorMode ==
                                                   ViewportColorMode::Custom
                                               ? settings.customColor
                                               : settings.colorMode ==
                                                         ViewportColorMode::Object &&
                                                     range.objectColor.isValid()
                                                     ? range.objectColor
                                                     : settings.colorMode ==
                                                               ViewportColorMode::Random
                                                           ? QColor::fromHsv(
                                                                 static_cast<int>(
                                                                     (objectSeed *
                                                                      2654435761u) % 360u),
                                                                 96, 204)
                                                           : QColor(Qt::white);
            const float alpha = (previewOverlay || settings.xrayEnabled())
                                    ? static_cast<float>(settings.xrayEnabled()
                                                             ? settings.xrayAlpha
                                                             : 0.5)
                                    : 1.0f;
            const bool explicitColor = previewOverlay ||
                settings.colorMode == ViewportColorMode::Custom ||
                settings.colorMode == ViewportColorMode::Random ||
                (settings.colorMode == ViewportColorMode::Object &&
                 range.objectColor.isValid());
            const QVector3D baseColorLinear = explicitColor
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
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY,
                          static_cast<GLuint>(previousMatcapTextureBinding));
        }
        if (agxDisplayAvailable) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_3D,
                          static_cast<GLuint>(previousAgxTextureBinding));
        }
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousShadowTextureBinding));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    }

    if (settings.mode == ViewportShadingMode::Solid &&
        settings.outline && !previewOverlay && !selectionOverlay &&
        !settings.xrayEnabled() &&
        !vertices_.isEmpty()) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        const QSize pixelSize(qRound(viewportSize.width() * dpr),
                              qRound(viewportSize.height() * dpr));
        if (ensureObjectIdFramebuffer(pixelSize)) {
            GLint previousFramebuffer = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            GLint previousActiveTexture = GL_TEXTURE0;
            GLint previousTextureBinding = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);

            glBindFramebuffer(GL_FRAMEBUFFER, objectIdFramebuffer_);
            glViewport(0, 0, pixelSize.width(), pixelSize.height());
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDisable(GL_BLEND);
            if (settings.backfaceCulling) {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            } else {
                glDisable(GL_CULL_FACE);
            }
            const GLuint emptyId = 0;
            glClearBufferuiv(GL_COLOR, 0, &emptyId);
            glClearDepth(1.0);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            objectIdProgram_.bind();
            const GLint objectIdUniformLocation =
                objectIdProgram_.uniformLocation("uObjectId");
            objectIdProgram_.setUniformValue(
                "uViewProjection", viewportViewProjection(transform, viewportSize));
            vertexArray_.bind();
            vertexBuffer_.bind();
            for (int rangeIndex = 0; rangeIndex < ranges_.size(); ++rangeIndex) {
                const DrawRange &range = ranges_[rangeIndex];
                if (range.count <= 0) {
                    continue;
                }
                GLuint objectId = static_cast<GLuint>(rangeIndex + 1) &
                                  0x7fffffffu;
                if (range.outlineHandledBySceneStroke) {
                    // Keep writing depth for this surface so its filled face
                    // still occludes outlines behind it, but omit its ID from
                    // the postprocessed outline mask. Its perimeter is drawn
                    // with the shared scene-stroke renderer instead.
                    objectId = 0u;
                } else if (range.selected) {
                    objectId |= 0x80000000u;
                }
                glUniform1ui(objectIdUniformLocation, objectId);
                objectIdProgram_.setUniformValue(
                    "uWorldOffset",
                    QVector3D(static_cast<float>(range.offset.x),
                              static_cast<float>(range.offset.y),
                              static_cast<float>(range.offset.z)));
                glDrawArrays(GL_TRIANGLES, range.first, range.count);
            }
            vertexBuffer_.release();
            vertexArray_.release();
            objectIdProgram_.release();

            glBindFramebuffer(GL_FRAMEBUFFER,
                              static_cast<GLuint>(previousFramebuffer));
            glViewport(previousViewport[0], previousViewport[1],
                       previousViewport[2], previousViewport[3]);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding);
            glBindTexture(GL_TEXTURE_2D, objectIdTexture_);

            outlineProgram_.bind();
            outlineProgram_.setUniformValue("uObjectIdBuffer", 0);
            outlineProgram_.setUniformValue(
                "uOutlineColor",
                QVector4D(settings.outlineColor.redF(),
                          settings.outlineColor.greenF(),
                          settings.outlineColor.blueF(), 1.0f));
            const QColor selectionColor = viewportSelectionColor();
            outlineProgram_.setUniformValue(
                "uSelectionColor",
                QVector4D(selectionColor.redF(), selectionColor.greenF(),
                          selectionColor.blueF(), 1.0f));
            outlineProgram_.setUniformValue(
                "uPixelOffset", std::max(1, qRound(dpr)));
            outlineVertexArray_.bind();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            outlineVertexArray_.release();
            outlineProgram_.release();
            glBindTexture(GL_TEXTURE_2D,
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
    if (previousCullFace) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
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
