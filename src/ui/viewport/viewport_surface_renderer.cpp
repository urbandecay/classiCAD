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
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>

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
    if (outlineVertexBuffer_.isCreated()) {
        outlineVertexBuffer_.destroy();
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
    program_.removeAllShaders();
    outlineProgram_.removeAllShaders();
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
            fragmentColor = vec4(color,
                                 uBaseColor.a);
        }
    )glsl";

    static constexpr const char *outlineVertexShader = R"glsl(
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
    static constexpr const char *outlineFragmentShader = R"glsl(
        #version 330 core
        uniform vec4 uOutlineColor;
        out vec4 fragmentColor;
        void main() { fragmentColor = uOutlineColor; }
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
        !outlineProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                 outlineVertexShader) ||
        !outlineProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                 outlineFragmentShader) ||
        !outlineProgram_.link() || !outlineVertexArray_.create() ||
        !outlineVertexBuffer_.create() ||
        !shadowProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                shadowVertexShader) ||
        !shadowProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                shadowFragmentShader) ||
        !shadowProgram_.link()) {
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

    outlineVertexArray_.bind();
    outlineVertexBuffer_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(QVector3D),
                          reinterpret_cast<const void *>(0));
    outlineVertexBuffer_.release();
    outlineVertexArray_.release();
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
        outlineRanges_.resize(objects.size());
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
                            object.objectId.value()});
            outlineRanges_[index].offset = object.preparedGeometryOffset;
            outlineRanges_[index].selected = object.selected;
        }
        return;
    }

    geometryKey_ = std::move(key);
    geometryDirty_ = true;
    vertices_.clear();
    outlineVertices_.clear();
    outlineRanges_.clear();
    silhouetteEdges_.clear();
    outlineRanges_.reserve(objects.size());
    silhouetteEdges_.reserve(objects.size());
    struct EdgeInfo {
        QVector3D start;
        QVector3D end;
        QVector3D firstNormal;
        QVector3D secondNormal;
        int count = 0;
        bool sharp = false;
    };
    const auto vertexBytes = [](const QVector3D &point) {
        const float coordinates[3] = {point.x(), point.y(), point.z()};
        QByteArray bytes(static_cast<int>(sizeof(coordinates)), Qt::Uninitialized);
        std::memcpy(bytes.data(), coordinates, sizeof(coordinates));
        return bytes;
    };
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
                        object.objectId.value()});

        const int outlineFirst = outlineVertices_.size();
        QVector<SilhouetteEdge> objectSilhouetteEdges;
        if (geometry != nullptr) {
            std::map<QByteArray, EdgeInfo> edges;
            for (int index = 0; index + 2 < count; index += 3) {
                const QVector3D &a = geometry->surfaceVertices[index];
                const QVector3D &b = geometry->surfaceVertices[index + 1];
                const QVector3D &c = geometry->surfaceVertices[index + 2];
                const QVector3D faceNormal = QVector3D::crossProduct(b - a,
                                                                     c - a)
                                                 .normalized();
                const QVector3D points[] = {a, b, c};
                for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
                    const QVector3D &start = points[edgeIndex];
                    const QVector3D &end = points[(edgeIndex + 1) % 3];
                    const QByteArray startKey = vertexBytes(start);
                    const QByteArray endKey = vertexBytes(end);
                    const bool ordered = startKey < endKey;
                    const QByteArray edgeKey = ordered
                        ? startKey + endKey
                        : endKey + startKey;
                    EdgeInfo &edge = edges[edgeKey];
                    if (edge.count == 0) {
                        edge.start = ordered ? start : end;
                        edge.end = ordered ? end : start;
                        edge.firstNormal = faceNormal;
                    } else {
                        if (edge.count == 1) {
                            edge.secondNormal = faceNormal;
                        }
                        if (QVector3D::dotProduct(edge.firstNormal,
                                                  faceNormal) < 0.75f) {
                            edge.sharp = true;
                        }
                    }
                    ++edge.count;
                }
            }
            for (const auto &entry : edges) {
                const EdgeInfo &edge = entry.second;
                if (edge.count == 1 || edge.sharp) {
                    outlineVertices_.append(edge.start);
                    outlineVertices_.append(edge.end);
                } else if (edge.count == 2) {
                    objectSilhouetteEdges.append(
                        {edge.start, edge.end, edge.firstNormal,
                         edge.secondNormal});
                }
            }
        }
        outlineRanges_.append({outlineFirst,
                               static_cast<int>(outlineVertices_.size()) -
                                   outlineFirst,
                               object.preparedGeometryOffset,
                               object.selected});
        silhouetteEdges_.append(std::move(objectSilhouetteEdges));
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
    bool clearDepth)
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
    GLfloat previousLineWidth = 1.0f;
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
    glGetFloatv(GL_LINE_WIDTH, &previousLineWidth);

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

    const bool hasSelectedSurface = std::any_of(
        ranges_.cbegin(), ranges_.cend(), [](const DrawRange &range) {
            return range.selected && range.count > 0;
        });
    if (settings.mode == ViewportShadingMode::Solid &&
        (settings.outline || settings.cavity || hasSelectedSurface) &&
        !previewOverlay &&
        (!outlineVertices_.isEmpty() || !silhouetteEdges_.isEmpty())) {
        glViewport(0, 0, qRound(viewportSize.width() *
                                std::max<qreal>(devicePixelRatio, 1.0)),
                   qRound(viewportSize.height() *
                          std::max<qreal>(devicePixelRatio, 1.0)));
        if (settings.xrayEnabled() && !xraySurfaceDepthAvailable) {
            glDisable(GL_DEPTH_TEST);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        QVector<QVector3D> frameOutlineVertices;
        QVector<OutlineRange> frameOutlineRanges;
        frameOutlineRanges.reserve(outlineRanges_.size());
        const Point3D cameraPosition = transform.cameraPosition(viewportSize);
        const QVector3D orthographicTowardCamera = workbenchNormalize(
            QVector3D(-static_cast<float>(transform.viewDirection().x),
                      -static_cast<float>(transform.viewDirection().y),
                      -static_cast<float>(transform.viewDirection().z)),
            QVector3D(0.0f, 0.0f, 1.0f));
        for (int objectIndex = 0; objectIndex < outlineRanges_.size();
             ++objectIndex) {
            const OutlineRange &baseRange = outlineRanges_[objectIndex];
            const int first = frameOutlineVertices.size();
            for (int vertexIndex = baseRange.first;
                 vertexIndex < baseRange.first + baseRange.count;
                 ++vertexIndex) {
                frameOutlineVertices.append(outlineVertices_[vertexIndex]);
            }
            if (objectIndex < silhouetteEdges_.size()) {
                const QVector3D offset(static_cast<float>(baseRange.offset.x),
                                       static_cast<float>(baseRange.offset.y),
                                       static_cast<float>(baseRange.offset.z));
                for (const SilhouetteEdge &edge : silhouetteEdges_[objectIndex]) {
                    const QVector3D midpoint =
                        (edge.start + edge.end) * 0.5f + offset;
                    const QVector3D towardCamera = transform.isPerspectiveEnabled()
                        ? workbenchNormalize(
                              QVector3D(static_cast<float>(cameraPosition.x),
                                        static_cast<float>(cameraPosition.y),
                                        static_cast<float>(cameraPosition.z)) -
                                  midpoint,
                              orthographicTowardCamera)
                        : orthographicTowardCamera;
                    const bool firstFacing =
                        QVector3D::dotProduct(edge.firstNormal,
                                              towardCamera) >= 0.0f;
                    const bool secondFacing =
                        QVector3D::dotProduct(edge.secondNormal,
                                              towardCamera) >= 0.0f;
                    if (firstFacing != secondFacing) {
                        frameOutlineVertices.append(edge.start);
                        frameOutlineVertices.append(edge.end);
                    }
                }
            }
            frameOutlineRanges.append(
                {first, static_cast<int>(frameOutlineVertices.size()) - first,
                 baseRange.offset, baseRange.selected});
        }
        if (frameOutlineVertices.isEmpty()) {
            glLineWidth(previousLineWidth);
        } else {
            outlineVertexArray_.bind();
            outlineVertexBuffer_.bind();
            outlineVertexBuffer_.allocate(
                frameOutlineVertices.constData(),
                frameOutlineVertices.size() *
                    static_cast<int>(sizeof(QVector3D)));
            outlineVertexBuffer_.release();
            outlineVertexArray_.release();
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        outlineProgram_.bind();
        outlineProgram_.setUniformValue(
            "uViewProjection", viewportViewProjection(transform, viewportSize));
        QColor outlineColor = settings.outline
                                  ? settings.outlineColor
                                  : QColor(Qt::black);
        outlineColor.setAlphaF(settings.xrayEnabled()
                                   ? std::clamp<qreal>(settings.xrayAlpha, 0.0, 1.0)
                                   : settings.outline ? 1.0 : 0.22);
        outlineVertexArray_.bind();
        outlineVertexBuffer_.bind();
        const qreal outlineWidth = settings.outline ? 1.0 : 1.5;
        for (const OutlineRange &range : frameOutlineRanges) {
            if (range.count <= 0) {
                continue;
            }
            if (!range.selected && !settings.outline && !settings.cavity) {
                continue;
            }
            const QColor rangeColor = range.selected
                                          ? viewportSelectionColor()
                                          : outlineColor;
            outlineProgram_.setUniformValue(
                "uOutlineColor",
                QVector4D(rangeColor.redF(), rangeColor.greenF(),
                          rangeColor.blueF(),
                          range.selected ? 1.0f : rangeColor.alphaF()));
            glLineWidth(static_cast<GLfloat>(std::max<qreal>(
                dpr * (range.selected ? 2.0 : outlineWidth), 1.0)));
            outlineProgram_.setUniformValue(
                "uWorldOffset",
                QVector3D(static_cast<float>(range.offset.x),
                          static_cast<float>(range.offset.y),
                          static_cast<float>(range.offset.z)));
            glDrawArrays(GL_LINES, range.first, range.count);
        }
        outlineVertexBuffer_.release();
        outlineVertexArray_.release();
        outlineProgram_.release();
        glLineWidth(previousLineWidth);
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
