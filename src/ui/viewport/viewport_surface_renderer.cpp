/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_surface_renderer.h"

#include "viewport_cavity_blue_noise.h"
#include "viewport_depth_geometry.h"
#include "blender_grid_renderer.h"
#include "viewport_line_weight.h"
#include "workbench_lighting.h"
#include "core/geometry/polygon_mesh.h"

#include <QDataStream>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QIODevice>
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QBuffer>
#include <QOpenGLContext>
#include <QVector2D>
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

QVector3D workbenchShadowDirectionWorld(
    const ViewportShadingSettings &settings)
{
    // Blender swaps Y/Z, then flips X/Z before Workbench shadow-volume
    // extrusion. Keep the same scene-display direction convention here.
    QVector3D direction(-settings.shadowDirection.x(),
                        settings.shadowDirection.z(),
                        -settings.shadowDirection.y());
    if (direction.lengthSquared() <= 1.0e-10f) {
        direction = QVector3D(-0.57735026f, 0.57735026f, -0.57735026f);
    }
    direction.normalize();
    return direction;
}

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

QVector3D transformMeshNormal(const std::array<double, 16> &m,
                              const Point3D &normal)
{
    const double a=m[0], b=m[1], c=m[2], d=m[4], e=m[5], f=m[6];
    const double g=m[8], h=m[9], i=m[10];
    const double cof[9] = {e*i-f*h, f*g-d*i, d*h-e*g,
                           c*h-b*i, a*i-c*g, b*g-a*h,
                           b*f-c*e, c*d-a*f, a*e-b*d};
    const double determinant = a*cof[0] + b*cof[1] + c*cof[2];
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-15)
        return {};
    QVector3D result(
        static_cast<float>((cof[0]*normal.x + cof[1]*normal.y + cof[2]*normal.z) / determinant),
        static_cast<float>((cof[3]*normal.x + cof[4]*normal.y + cof[5]*normal.z) / determinant),
        static_cast<float>((cof[6]*normal.x + cof[7]*normal.y + cof[8]*normal.z) / determinant));
    if (result.lengthSquared() <= 1.0e-16f) return {};
    return result.normalized();
}

bool hasNurbsSurfaceGeometry(const ViewportRenderObject &object)
{
    return object.shape.geometryType == GeometryType::NurbsSurface ||
           object.shape.geometryType == GeometryType::NurbsSolid;
}

bool hasPolygonMeshGeometry(const ViewportRenderObject &object)
{
    return object.shape.geometryType == GeometryType::PolygonMesh &&
           validatePolygonMesh(object.shape.polygonMesh);
}

constexpr int kWorkbenchCavityMaxSamples = 512;

quint32 blenderStringHash(const QByteArray &bytes, quint32 seed = 0)
{
    constexpr quint32 multiplier = 0x5bd1e995U;
    const uchar *data = reinterpret_cast<const uchar *>(bytes.constData());
    int remaining = bytes.size();
    quint32 hash = seed ^ static_cast<quint32>(remaining);
    while (remaining >= 4) {
        quint32 chunk = static_cast<quint32>(data[0]) |
                        (static_cast<quint32>(data[1]) << 8) |
                        (static_cast<quint32>(data[2]) << 16) |
                        (static_cast<quint32>(data[3]) << 24);
        chunk *= multiplier;
        chunk ^= chunk >> 24;
        chunk *= multiplier;
        hash *= multiplier;
        hash ^= chunk;
        data += 4;
        remaining -= 4;
    }
    if (remaining == 3) hash ^= static_cast<quint32>(data[2]) << 16;
    if (remaining >= 2) hash ^= static_cast<quint32>(data[1]) << 8;
    if (remaining >= 1) {
        hash ^= data[0];
        hash *= multiplier;
    }
    hash ^= hash >> 13;
    hash *= multiplier;
    hash ^= hash >> 15;
    return hash;
}

QColor workbenchRandomColor(quint64 objectId,
                            const QString &displayName,
                            const QString &libraryPath)
{
    quint32 hash = 0;
    if (!displayName.isEmpty()) {
        hash = blenderStringHash(QByteArrayLiteral("OB") + displayName.toUtf8());
        if (!libraryPath.isEmpty()) {
            hash = blenderStringHash(libraryPath.toUtf8(), hash);
        }
    } else {
        hash = static_cast<quint32>(objectId) ^
               static_cast<quint32>(objectId >> 32);
        hash ^= hash >> 16;
        hash *= 0x7feb352dU;
        hash ^= hash >> 15;
        hash *= 0x846ca68bU;
        hash ^= hash >> 16;
    }
    const qreal hue = static_cast<qreal>(hash) / 4294967296.0;
    return QColor::fromHsvF(hue, 0.5, 0.8);
}

qreal workbenchSourceAlpha(ViewportColorMode colorMode,
                           qreal objectAlpha,
                           const WorkbenchMaterialData &material,
                           const QColor &customColor)
{
    switch (colorMode) {
    case ViewportColorMode::Material:
    case ViewportColorMode::Attribute:
    case ViewportColorMode::Texture:
        return material.alpha;
    case ViewportColorMode::Object:
    case ViewportColorMode::Random:
        return objectAlpha;
    case ViewportColorMode::Custom:
        Q_UNUSED(customColor);
        return workbenchCustomColorSourceAlpha;
    }
    return 1.0;
}

qreal workbenchEffectiveAlpha(qreal sourceAlpha,
                              const ViewportShadingSettings &settings,
                              bool previewOverlay)
{
    if (previewOverlay) {
        return 0.5;
    }
    return std::clamp(sourceAlpha *
                          (settings.xrayEnabled() ? settings.xrayAlpha : 1.0),
                      0.0, 1.0);
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
    if (shadowVolumeVertexBuffer_.isCreated()) {
        shadowVolumeVertexBuffer_.destroy();
    }
    if (shadowVolumeVertexArray_.isCreated()) {
        shadowVolumeVertexArray_.destroy();
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
    for (GLuint texture : imageTextures_) {
        if (texture != 0) glDeleteTextures(1, &texture);
    }
    imageTextures_.clear();
    if (agxDisplayTexture_ != 0) {
        glDeleteTextures(1, &agxDisplayTexture_);
        agxDisplayTexture_ = 0;
    }
    if (cavityJitterTexture_ != 0) {
        glDeleteTextures(1, &cavityJitterTexture_);
        cavityJitterTexture_ = 0;
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
    if (workbenchEffectFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &workbenchEffectFramebuffer_);
        workbenchEffectFramebuffer_ = 0;
    }
    if (workbenchOutlineIdTexture_ != 0) {
        glDeleteTextures(1, &workbenchOutlineIdTexture_);
        workbenchOutlineIdTexture_ = 0;
    }
    if (cavityDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &cavityDepthFramebuffer_);
        cavityDepthFramebuffer_ = 0;
    }
    if (cavityDepthTexture_ != 0) {
        glDeleteTextures(1, &cavityDepthTexture_);
        cavityDepthTexture_ = 0;
    }
    program_.removeAllShaders();
    outlineProgram_.removeAllShaders();
    objectIdProgram_.removeAllShaders();
    workbenchEffectProgram_.removeAllShaders();
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
        layout(location = 2) in vec3 aAttributeColor;
        layout(location = 3) in vec2 aUv;
        uniform mat4 uViewProjection;
        uniform vec3 uWorldOffset;
        uniform mat4 uShadowViewProjection;
        out vec3 vNormalView;
        out vec3 vAttributeColor;
        out vec2 vUv;
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
            vAttributeColor = aAttributeColor;
            vUv = aUv;
            vWorldPosition = position;
            vShadowPosition = uShadowViewProjection * vec4(position, 1.0);
            gl_Position = uViewProjection * vec4(position, 1.0);
        }
    )glsl";
    // The screen-space cavity and normal-curvature equations below adapt
    // Blender 5.2.2 Workbench shaders. Blender Authors; GPL-2.0-or-later.
    static constexpr const char *fragmentShader = R"glsl(
        #version 330 core
        in vec3 vNormalView;
        in vec3 vAttributeColor;
        in vec2 vUv;
        in vec3 vWorldPosition;
        in vec4 vShadowPosition;
        uniform vec4 uBaseColor;
        uniform bool uUseAttributeColor;
        uniform bool uUseTextureColor;
        uniform sampler2D uWorkbenchImage;
        uniform bool uTexturePremultiplied;
        uniform bool uTextureAlphaCutoff;
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
        uniform bool uMatcapFlipX;
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
        uniform bool uUseShadowMap;
        uniform bool uShadowVolumeShadowed;
        uniform float uShadowIntensity;
        uniform float uShadowBias;
        uniform float uShadowFocus;
        uniform float uShadowShift;
        uniform vec3 uShadowDirectionView;
        uniform bool uUseCavity;
        uniform bool uUseSsao;
        uniform bool uUseCurvature;
        uniform sampler2D uCavityDepth;
        uniform sampler2D uCavityNormal;
        uniform usampler2D uCavityObjectId;
        uniform sampler2D uCavityJitterTexture;
        uniform mat4 uInverseViewProjection;
        uniform vec2 uCavityProjectionScale;
        uniform float uCavityUiScale;
        uniform float uCavityDistance;
        uniform float uCavityAttenuation;
        uniform float uCavityValleyFactor;
        uniform float uCavityRidgeFactor;
        uniform float uCurvatureRidgeControl;
        uniform float uCurvatureValleyControl;
        uniform int uCavitySampleStart;
        uniform int uCavitySampleEnd;
        uniform int uCavitySamplesPerIteration;
        uniform float uCavitySampleCountInv;
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

        vec3 cavityNoise(vec2 screenCoordinate, ivec2 pixelSize)
        {
            // Blender samples a repeating 64x64 texture at one texel per
            // viewport pixel. Its RGB values are the rotated direction and
            // sample-radius offset, built from the shared blue-noise LUT.
            vec2 jitterCoordinate = screenCoordinate * vec2(pixelSize) *
                                    (1.0 / 64.0);
            return texture(uCavityJitterTexture, jitterCoordinate).rgb;
        }

        float radicalInverseVdc(uint bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) |
                   ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) |
                   ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) |
                   ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) |
                   ((bits & 0xFF00FF00u) >> 8u);
            return float(bits) * 2.3283064365386963e-10;
        }

        vec3 cavityViewPosition(vec2 screenCoordinate, float depth)
        {
            vec4 worldPosition = uInverseViewProjection *
                vec4(screenCoordinate * 2.0 - 1.0,
                     depth * 2.0 - 1.0, 1.0);
            if (abs(worldPosition.w) <= 1.0e-8) {
                return vec3(0.0);
            }
            worldPosition.xyz /= worldPosition.w;
            vec3 relative = worldPosition.xyz - uCameraPosition;
            return vec3(dot(relative, uViewRight),
                        dot(relative, uViewUp),
                        dot(relative, uViewFacing));
        }

        vec2 workbenchCavity(vec2 screenCoordinate,
                             float depth,
                             vec3 position,
                             vec3 normal)
        {
            if (depth == 1.0 || depth == 0.0) {
                return vec2(0.0);
            }

            vec3 noise = cavityNoise(screenCoordinate,
                                     textureSize(uCavityNormal, 0));
            float homcoord = uPerspective ? max(abs(position.z), 1.0e-6) : 1.0;
            vec2 offset = uCavityProjectionScale *
                          (uCavityDistance / homcoord) * 0.5;
            vec2 rotX = noise.rg;
            vec2 rotY = vec2(-rotX.y, rotX.x);
            float cavities = 0.0;
            float edges = 0.0;
            for (int index = uCavitySampleStart;
                 index < uCavitySampleEnd && index < 512;
                 ++index) {
                float iterationOffset =
                    float(index / uCavitySamplesPerIteration) * 0.499;
                float radius = fract((float(index) + 0.5 + iterationOffset) /
                                     float(uCavitySamplesPerIteration));
                float angle = radicalInverseVdc(uint(index)) *
                              6.28318530718 + iterationOffset;
                vec2 sampleDirection = vec2(cos(angle), sin(angle));
                vec2 jitteredDirection = vec2(
                    dot(sampleDirection, rotX), dot(sampleDirection, rotY));
                jitteredDirection *= radius + noise.b;
                vec2 sampleCoordinate = screenCoordinate +
                                        jitteredDirection * offset;
                if (any(greaterThan(abs(sampleCoordinate - 0.5),
                                    vec2(0.5)))) {
                    continue;
                }

                float sampleDepth = texture(uCavityDepth, sampleCoordinate).r;
                bool isBackground = sampleDepth == 1.0;
                if (isBackground) {
                    sampleDepth = depth;
                }
                vec3 samplePosition = cavityViewPosition(sampleCoordinate,
                                                         sampleDepth);
                if (isBackground) {
                    samplePosition.z -= uCavityDistance;
                }
                vec3 direction = samplePosition - position;
                float lengthToSample = length(direction);
                float cavityDot = dot(direction, normal);
                float edgeDot = -cavityDot;
                float bias = 0.05 * lengthToSample + 0.0001;
                float attenuation = 1.0 /
                    max(lengthToSample *
                            (1.0 + lengthToSample * lengthToSample *
                                       uCavityAttenuation),
                        1.0e-8);
                if (cavityDot > -bias) {
                    cavities += cavityDot * attenuation;
                }
                if (edgeDot > bias) {
                    edges += edgeDot * attenuation;
                }
            }
            cavities *= uCavitySampleCountInv;
            edges *= uCavitySampleCountInv;
            cavities = clamp(cavities * uCavityValleyFactor, 0.0, 1.0);
            edges *= uCavityRidgeFactor;
            return vec2(cavities, edges);
        }

        float curvatureSoftClamp(float curvature, float control)
        {
            if (curvature < 0.5 / control) {
                return curvature * (1.0 - curvature * control);
            }
            return 0.25 / control;
        }

        float workbenchCurvature(ivec2 pixel, ivec2 size)
        {
            int pixelOffset = max(1, int(round(uCavityUiScale)));
            ivec2 upPixel = clamp(pixel + ivec2(0, pixelOffset),
                                  ivec2(0), size - 1);
            ivec2 downPixel = clamp(pixel - ivec2(0, pixelOffset),
                                    ivec2(0), size - 1);
            ivec2 rightPixel = clamp(pixel + ivec2(pixelOffset, 0),
                                     ivec2(0), size - 1);
            ivec2 leftPixel = clamp(pixel - ivec2(pixelOffset, 0),
                                    ivec2(0), size - 1);
            uint objectUp = texelFetch(uCavityObjectId, upPixel, 0).r;
            uint objectDown = texelFetch(uCavityObjectId, downPixel, 0).r;
            uint objectRight = texelFetch(uCavityObjectId, rightPixel, 0).r;
            uint objectLeft = texelFetch(uCavityObjectId, leftPixel, 0).r;
            if (objectUp != objectDown || objectRight != objectLeft ||
                (objectUp == 0u && objectRight == 0u)) {
                return 0.0;
            }
            float normalUp = texelFetch(uCavityNormal, upPixel, 0).g;
            float normalDown = texelFetch(uCavityNormal, downPixel, 0).g;
            float normalRight = texelFetch(uCavityNormal, rightPixel, 0).r;
            float normalLeft = texelFetch(uCavityNormal, leftPixel, 0).r;
            float normalDifference = (normalUp - normalDown) +
                                     (normalRight - normalLeft);
            if (normalDifference < 0.0) {
                return -2.0 * curvatureSoftClamp(-normalDifference,
                                                 uCurvatureValleyControl);
            }
            return 2.0 * curvatureSoftClamp(normalDifference,
                                            uCurvatureRidgeControl);
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
            vec3 textureColor = vec3(1.0);
            if (uUseTextureColor) {
                vec4 imageColor = texture(uWorkbenchImage, vUv);
                if (uTextureAlphaCutoff && imageColor.a < 0.1) {
                    discard;
                }
                if (uTexturePremultiplied && imageColor.a != 0.0 &&
                    imageColor.a != 1.0) {
                    imageColor.rgb /= imageColor.a;
                }
                textureColor = imageColor.rgb;
            }
            vec3 baseColor = uUseTextureColor
                                 ? textureColor
                                 : (uUseAttributeColor ? vAttributeColor
                                                       : uBaseColor.rgb);
            vec3 color = baseColor;
            if (uLightingMode == 0) {
                color = workbenchStudioLighting(baseColor,
                                                uRoughness,
                                                uMetallic,
                                                normal,
                                                incident);
            } else if (uLightingMode == 1 && uMatcapAvailable) {
                vec2 uv = workbenchMatcapUv(incident, normal);
                if (uMatcapFlipX) {
                    uv.x = 1.0 - uv.x;
                }
                vec3 diffuse = texture(uMatcapTexture, vec3(uv, 0.0)).rgb;
                vec3 specular = uUseSpecular
                                    ? texture(uMatcapTexture,
                                              vec3(uv, 1.0)).rgb
                                    : vec3(0.0);
                color = diffuse * baseColor + specular;
            }
            if (uUseCavity) {
                ivec2 pixel = ivec2(gl_FragCoord.xy);
                ivec2 size = textureSize(uCavityNormal, 0);
                vec2 screenCoordinate = gl_FragCoord.xy / vec2(size);
                float cavityAmount = 0.0;
                float edgeAmount = 0.0;
                if (uUseSsao) {
                    float depth = texture(uCavityDepth,
                                          screenCoordinate).r;
                    vec2 cavity = workbenchCavity(
                        screenCoordinate, depth,
                        cavityViewPosition(screenCoordinate, depth),
                        normal);
                    cavityAmount = cavity.x;
                    edgeAmount = cavity.y;
                }
                float curvature = uUseCurvature
                                      ? workbenchCurvature(pixel, size)
                                      : 0.0;
                color *= clamp((1.0 - cavityAmount) * (1.0 + edgeAmount) *
                                   (1.0 + curvature),
                               0.0, 4.0);
            }
            if (uUseShadows) {
                float visibility = 1.0;
                if (uUseShadowMap) {
                    vec3 shadowCoordinate =
                        vShadowPosition.xyz / vShadowPosition.w * 0.5 + 0.5;
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
                }
                float focus = clamp(uShadowFocus, 0.0001, 0.99999);
                float focusEdge = 1.0 - focus * (1.0 - uShadowShift);
                float lightFactor = -dot(normal, uShadowDirectionView);
                float shadowMix = smoothstep(uShadowShift, focusEdge,
                                             lightFactor);
                float shadowVisibility = uShadowVolumeShadowed ? 0.0 : visibility;
                shadowMix *= shadowVisibility *
                             clamp(uShadowIntensity, 0.0, 1.0);
                shadowMix += 1.0 - clamp(uShadowIntensity, 0.0, 1.0);
                color *= shadowMix;
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
        layout(location = 1) in vec3 aNormal;
        uniform mat4 uViewProjection;
        uniform vec3 uWorldOffset;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        out vec3 vNormalView;
        void main()
        {
            vNormalView = vec3(dot(aNormal, uViewRight),
                               dot(aNormal, uViewUp),
                               dot(aNormal, uViewFacing));
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
    static constexpr const char *workbenchEffectFragmentShader = R"glsl(
        #version 330 core
        in vec3 vNormalView;
        uniform uint uObjectId;
        uniform uint uOutlineObjectId;
        layout(location = 0) out vec4 fragmentViewNormal;
        layout(location = 1) out uint fragmentObjectId;
        layout(location = 2) out uint fragmentOutlineObjectId;
        void main()
        {
            fragmentViewNormal = vec4(normalize(vNormalView), 1.0);
            fragmentObjectId = uObjectId;
            fragmentOutlineObjectId = uOutlineObjectId;
        }
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
        !shadowVolumeVertexArray_.create() ||
        !shadowVolumeVertexBuffer_.create() ||
        !objectIdProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                  objectIdVertexShader) ||
        !objectIdProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                  objectIdFragmentShader) ||
        !objectIdProgram_.link() ||
        !workbenchEffectProgram_.addShaderFromSourceCode(
            QOpenGLShader::Vertex, objectIdVertexShader) ||
        !workbenchEffectProgram_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, workbenchEffectFragmentShader) ||
        !workbenchEffectProgram_.link() ||
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
            << "Workbench effects:" << workbenchEffectProgram_.log()
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
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, attributeR)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, u)));
    vertexBuffer_.release();
    vertexArray_.release();

    shadowVolumeVertexArray_.bind();
    shadowVolumeVertexBuffer_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(QVector3D), nullptr);
    shadowVolumeVertexBuffer_.release();
    shadowVolumeVertexArray_.release();

    initialized_ = true;
    return true;
}

void ViewportSurfaceRenderer::prepareGeometry(
    const QVector<ViewportRenderObject> &objects)
{
    QByteArray key;
    QDataStream keyStream(&key, QIODevice::WriteOnly);
    keyStream << qint32(objects.size());
    for (int objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ViewportRenderObject &object = objects[objectIndex];
        const bool hasSurface = hasNurbsSurfaceGeometry(object);
        const bool hasMesh = hasPolygonMeshGeometry(object);
        const ViewportDepthGeometry *geometry =
            hasSurface ? object.preparedDepthGeometry.data() : nullptr;
        keyStream << quint64(object.objectId.value())
                  << quint64(object.geometryRevision)
                  << quint8(hasSurface ? 1 : (hasMesh ? 2 : 0))
                  << quint64(reinterpret_cast<quintptr>(geometry))
                  << qint32(geometry != nullptr
                                ? geometry->surfaceVertices.size()
                                : 0);
        if (hasMesh) {
            const PolygonMesh3D &mesh = object.shape.polygonMesh;
            keyStream << qint32(mesh.vertices.size()) << qint32(mesh.faces.size());
            for (const Point3D &point : mesh.vertices) {
                keyStream << double(point.x) << double(point.y) << double(point.z);
            }
            for (const QVector<int> &face : mesh.faces) {
                keyStream << qint32(face.size());
                for (int index : face) keyStream << qint32(index);
            }
            for (const Point3D &normal : mesh.cornerNormals)
                keyStream << double(normal.x) << double(normal.y) << double(normal.z);
            keyStream << mesh.activeColorAttributeName;
            keyStream << mesh.activeUvMapName;
            for (const MeshUvMap &uvMap : mesh.uvMaps) {
                if (uvMap.name != mesh.activeUvMapName) continue;
                for (const QPointF &uv : uvMap.cornerValues)
                    keyStream << double(uv.x()) << double(uv.y());
            }
            for (const WorkbenchMaterialData &material : object.meshMaterials)
                keyStream << material.textureImagePath
                          << QCryptographicHash::hash(
                                 material.textureImagePackedDataBase64.toLatin1(),
                                 QCryptographicHash::Sha256)
                          << material.textureImageSrgb
                          << material.textureImageAlphaMode;
            for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
                if (attribute.name != mesh.activeColorAttributeName) continue;
                keyStream << attribute.name << qint32(attribute.domain)
                          << attribute.dataType << attribute.colorSpace
                          << qint32(attribute.componentCount)
                          << qint32(attribute.values.size());
                for (float value : attribute.values) keyStream << value;
            }
            for (double value : object.meshObjectTransform) keyStream << value;
        }
    }
    const bool changed = key != geometryKey_;
    ranges_.clear();
    ranges_.reserve(objects.size());
    if (changed) {
        geometryKey_ = std::move(key);
        geometryDirty_ = true;
        vertices_.clear();
    }
    for (int objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ViewportRenderObject &object = objects[objectIndex];
        const ViewportDepthGeometry *geometry =
            hasNurbsSurfaceGeometry(object)
                ? object.preparedDepthGeometry.data()
                : nullptr;
        const auto makeRange = [&object, objectIndex](int first, int count) {
            DrawRange range;
            range.first = first;
            range.count = count;
            range.offset = object.preparedGeometryOffset;
            range.selected = object.selected;
            range.displayColorSceneLinear = object.displayColorSceneLinear;
            range.workbenchMaterial = object.workbenchMaterial;
            range.objectSeed = object.objectId.value();
            range.effectObjectId = static_cast<quint32>(objectIndex + 1) &
                                   0x7fffffffu;
            range.displayName = object.displayName;
            range.libraryPath = object.libraryPath;
            range.outlineHandledBySceneStroke =
                object.outlineHandledBySceneStroke;
            return range;
        };
        if (hasPolygonMeshGeometry(object)) {
            const PolygonMesh3D &mesh = object.shape.polygonMesh;
            const MeshColorAttribute *activeAttribute = nullptr;
            for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
                if (attribute.name == mesh.activeColorAttributeName) {
                    activeAttribute = &attribute;
                    break;
                }
            }
            int firstCorner = 0;
            const auto &matrix = object.meshObjectTransform;
            const qreal linearDeterminant =
                matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
                matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
                matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
            const bool reflected = std::isfinite(linearDeterminant) &&
                                   linearDeterminant < -1.0e-12;
            const MeshUvMap *activeUvMap = nullptr;
            for (const MeshUvMap &uvMap : mesh.uvMaps) {
                if (uvMap.name == mesh.activeUvMapName) {
                    activeUvMap = &uvMap;
                    break;
                }
            }
            for (int faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
                const QVector<int> &face = mesh.faces[faceIndex];
                QVector<Point3D> localLoop;
                QVector<Point3D> worldLoop;
                localLoop.reserve(face.size());
                worldLoop.reserve(face.size());
                for (int index : face) {
                    const Point3D &p = mesh.vertices[index];
                    localLoop.append(p);
                    const qreal w = matrix[12] * p.x + matrix[13] * p.y +
                                    matrix[14] * p.z + matrix[15];
                    const qreal divisor = std::abs(w) > 1.0e-12 ? w : 1.0;
                    worldLoop.append({(matrix[0] * p.x + matrix[1] * p.y +
                                       matrix[2] * p.z + matrix[3]) / divisor,
                                      (matrix[4] * p.x + matrix[5] * p.y +
                                       matrix[6] * p.z + matrix[7]) / divisor,
                                      (matrix[8] * p.x + matrix[9] * p.y +
                                       matrix[10] * p.z + matrix[11]) / divisor});
                }
                const auto triangles = triangulatePolygonLoop(localLoop);
                const int faceFirstCorner = firstCorner;
                firstCorner += face.size();
                const int faceSlot = mesh.faceMaterialIndices.value(faceIndex, -1);
                const QString imagePath = faceSlot >= 0 &&
                        faceSlot < object.meshMaterials.size()
                    ? object.meshMaterials[faceSlot].textureImagePath : QString{};
                const bool packedImageAvailable = faceSlot >= 0 &&
                        faceSlot < object.meshMaterials.size() &&
                    !object.meshMaterials[faceSlot].textureImagePackedDataBase64.isEmpty();
                const bool textureAvailable = (!imagePath.isEmpty() || packedImageAvailable) &&
                    activeUvMap != nullptr &&
                    faceFirstCorner + face.size() <= activeUvMap->cornerValues.size();
                const int first = changed
                    ? vertices_.size()
                    : (ranges_.isEmpty() ? 0
                                         : ranges_.last().first + ranges_.last().count);
                int count = 0;
                for (const auto &triangle : triangles) {
                    const int indexA = triangle[0];
                    const int indexB = reflected ? triangle[2] : triangle[1];
                    const int indexC = reflected ? triangle[1] : triangle[2];
                    const Point3D &a = worldLoop[indexA];
                    const Point3D &b = worldLoop[indexB];
                    const Point3D &c = worldLoop[indexC];
                    QVector3D normal(
                        static_cast<float>((b.y - a.y) * (c.z - a.z) -
                                           (b.z - a.z) * (c.y - a.y)),
                        static_cast<float>((b.z - a.z) * (c.x - a.x) -
                                           (b.x - a.x) * (c.z - a.z)),
                        static_cast<float>((b.x - a.x) * (c.y - a.y) -
                                           (b.y - a.y) * (c.x - a.x)));
                    if (normal.lengthSquared() <= 1.0e-16f) continue;
                    normal.normalize();
                    QVector3D triangleColors[3] = {
                        QVector3D(1.0f, 1.0f, 1.0f),
                        QVector3D(1.0f, 1.0f, 1.0f),
                        QVector3D(1.0f, 1.0f, 1.0f)};
                    QPointF triangleUvs[3] = {};
                    if (textureAvailable) {
                        for (int corner = 0; corner < 3; ++corner)
                            triangleUvs[corner] = activeUvMap->cornerValues[
                                faceFirstCorner + triangle[corner]];
                    }
                    bool triangleHasAttribute = activeAttribute != nullptr &&
                        activeAttribute->componentCount >= 1 &&
                        activeAttribute->componentCount <= 4;
                    if (triangleHasAttribute) {
                        for (int corner = 0; corner < 3; ++corner) {
                            const int loopIndex = triangle[corner];
                            int elementIndex = faceIndex;
                            if (activeAttribute->domain == MeshAttributeDomain::Point) {
                                elementIndex = face[loopIndex];
                            } else if (activeAttribute->domain == MeshAttributeDomain::Corner) {
                                elementIndex = faceFirstCorner + loopIndex;
                            }
                            const int base = elementIndex * activeAttribute->componentCount;
                            if (base < 0 || base + activeAttribute->componentCount >
                                                activeAttribute->values.size()) {
                                triangleHasAttribute = false;
                                break;
                            }
                            float r = activeAttribute->values[base];
                            float g = activeAttribute->componentCount >= 3
                                          ? activeAttribute->values[base + 1] : r;
                            float b = activeAttribute->componentCount >= 3
                                          ? activeAttribute->values[base + 2] : r;
                            if (activeAttribute->colorSpace.contains(
                                    QStringLiteral("srgb"), Qt::CaseInsensitive)) {
                                const QVector3D linear = workbenchSrgbToSceneLinear(
                                    QVector3D(r, g, b));
                                r = linear.x(); g = linear.y(); b = linear.z();
                            }
                            triangleColors[corner] = QVector3D(r, g, b);
                        }
                    }
                    if (changed) {
                        const Point3D *trianglePoints[3] = {&a, &b, &c};
                        for (int corner = 0; corner < 3; ++corner) {
                            const Point3D *point = trianglePoints[corner];
                            const QVector3D &color = triangleColors[corner];
                            QVector3D vertexNormal = normal;
                            if (!mesh.cornerNormals.isEmpty()) {
                                const int loopIndex = triangle[corner];
                                vertexNormal = transformMeshNormal(
                                    matrix, mesh.cornerNormals[faceFirstCorner + loopIndex]);
                                if (vertexNormal.lengthSquared() <= 1.0e-16f)
                                    vertexNormal = normal;
                            }
                            vertices_.append({static_cast<float>(point->x),
                                              static_cast<float>(point->y),
                                              static_cast<float>(point->z),
                                              vertexNormal.x(), vertexNormal.y(), vertexNormal.z(),
                                              color.x(), color.y(), color.z(),
                                              static_cast<float>(triangleUvs[corner].x()),
                                              static_cast<float>(triangleUvs[corner].y())});
                        }
                    }
                    count += 3;
                }
                DrawRange range = makeRange(first, count);
                range.useAttributeColor = activeAttribute != nullptr &&
                    activeAttribute->componentCount >= 1 &&
                    activeAttribute->componentCount <= 4;
                range.useTextureColor = textureAvailable;
                range.textureImagePath = textureAvailable ? imagePath : QString{};
                const int slotIndex = mesh.faceMaterialIndices.value(faceIndex, -1);
                if (slotIndex >= 0 && slotIndex < object.meshMaterials.size()) {
                    range.workbenchMaterial = object.meshMaterials[slotIndex];
                } else if (slotIndex >= 0 && slotIndex < mesh.materialSlots.size()) {
                    const PolygonMeshMaterialSlot &slot = mesh.materialSlots[slotIndex];
                    if (slot.baseColorSceneLinear.size() >= 3) {
                        range.workbenchMaterial.baseColorSceneLinear = QVector3D(
                            slot.baseColorSceneLinear[0], slot.baseColorSceneLinear[1],
                            slot.baseColorSceneLinear[2]);
                    }
                    range.workbenchMaterial.roughness = slot.roughness;
                    range.workbenchMaterial.metallic = slot.metallic;
                    range.workbenchMaterial.alpha = slot.alpha;
                }
                range.textureImageSrgb = range.workbenchMaterial.textureImageSrgb;
                ranges_.append(std::move(range));
            }
        } else {
            const int first = changed ? vertices_.size() :
                (ranges_.isEmpty() ? 0 : ranges_.last().first + ranges_.last().count);
            const int count = geometry != nullptr
                                  ? static_cast<int>(std::min(
                                        geometry->surfaceVertices.size(),
                                        geometry->surfaceNormals.size())) : 0;
            if (changed) {
                for (int index = 0; index < count; ++index) {
                    const QVector3D &position = geometry->surfaceVertices[index];
                    const QVector3D &normal = geometry->surfaceNormals[index];
                    vertices_.append({position.x(), position.y(), position.z(),
                                      normal.x(), normal.y(), normal.z()});
                }
            }
            DrawRange range = makeRange(first, count);
            if (object.shape.geometryType == GeometryType::NurbsSolid &&
                geometry != nullptr && geometry->shadowCasterMesh.valid) {
                range.shadowVolumeSupported = true;
                range.shadowVertices = geometry->shadowCasterMesh.vertices;
                range.shadowTriangles = geometry->shadowCasterMesh.triangles;
            }
            ranges_.append(std::move(range));
        }
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

bool ViewportSurfaceRenderer::ensureWorkbenchEffectFramebuffers(
    const QSize &pixelSize,
    const ViewportRenderTargets &targets)
{
    if (pixelSize.isEmpty() || targets.depthTexture == 0 ||
        targets.viewSpaceNormalTexture == 0 || targets.objectIdTexture == 0 ||
        targets.pixelSize != pixelSize) {
        return false;
    }

    if (workbenchEffectFramebufferSize_ != pixelSize) {
        if (workbenchEffectFramebuffer_ != 0) {
            glDeleteFramebuffers(1, &workbenchEffectFramebuffer_);
            workbenchEffectFramebuffer_ = 0;
        }
        if (workbenchOutlineIdTexture_ != 0) {
            glDeleteTextures(1, &workbenchOutlineIdTexture_);
            workbenchOutlineIdTexture_ = 0;
        }
        if (cavityDepthFramebuffer_ != 0) {
            glDeleteFramebuffers(1, &cavityDepthFramebuffer_);
            cavityDepthFramebuffer_ = 0;
        }
        if (cavityDepthTexture_ != 0) {
            glDeleteTextures(1, &cavityDepthTexture_);
            cavityDepthTexture_ = 0;
        }
        workbenchEffectFramebufferSize_ = pixelSize;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    if (workbenchOutlineIdTexture_ == 0) {
        glGenTextures(1, &workbenchOutlineIdTexture_);
        glBindTexture(GL_TEXTURE_2D, workbenchOutlineIdTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                     pixelSize.width(), pixelSize.height(), 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (cavityDepthTexture_ == 0) {
        glGenTextures(1, &cavityDepthTexture_);
        glBindTexture(GL_TEXTURE_2D, cavityDepthTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                     pixelSize.width(), pixelSize.height(), 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (workbenchEffectFramebuffer_ == 0) {
        glGenFramebuffers(1, &workbenchEffectFramebuffer_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, workbenchEffectFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, targets.viewSpaceNormalTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, targets.objectIdTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                           GL_TEXTURE_2D, workbenchOutlineIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, targets.depthTexture, 0);
    const GLenum drawAttachments[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, drawAttachments);
    const bool effectsComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (cavityDepthFramebuffer_ == 0) {
        glGenFramebuffers(1, &cavityDepthFramebuffer_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, cavityDepthFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, cavityDepthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool depthSnapshotComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (!effectsComplete || !depthSnapshotComplete) {
        qWarning() << "Workbench effect framebuffer is incomplete"
                   << "effects=" << effectsComplete
                   << "depthSnapshot=" << depthSnapshotComplete;
        return false;
    }
    return true;
}

bool ViewportSurfaceRenderer::renderWorkbenchEffectPrepass(
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportRenderTargets &targets,
    bool snapshotDepth,
    bool *snapshotDepthReady)
{
    if (snapshotDepthReady != nullptr) {
        *snapshotDepthReady = !snapshotDepth;
    }
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    if (vertices_.isEmpty() ||
        !ensureWorkbenchEffectFramebuffers(pixelSize, targets)) {
        return false;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousDepthFunction = GL_LESS;
    GLint previousProgram = 0;
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);

    glBindFramebuffer(GL_FRAMEBUFFER, workbenchEffectFramebuffer_);
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_SCISSOR_TEST);
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

    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLuint clearId[4] = {0u, 0u, 0u, 0u};
    const GLfloat clearDepth[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 0, clearNormal);
    glClearBufferuiv(GL_COLOR, 1, clearId);
    glClearBufferuiv(GL_COLOR, 2, clearId);
    glClearBufferfv(GL_DEPTH, 0, clearDepth);

    const Point3D viewDirection =
        normalized(transform.viewDirection(), {0.0, 0.0, 1.0});
    const Point3D viewUp = normalized(transform.viewUp(), {0.0, 1.0, 0.0});
    const Point3D viewRight = normalized(cross(viewUp, viewDirection),
                                         {1.0, 0.0, 0.0});
    workbenchEffectProgram_.bind();
    workbenchEffectProgram_.setUniformValue(
        "uViewProjection", viewportViewProjection(transform, viewportSize));
    workbenchEffectProgram_.setUniformValue("uViewRight", asVector(viewRight));
    workbenchEffectProgram_.setUniformValue("uViewUp", asVector(viewUp));
    workbenchEffectProgram_.setUniformValue("uViewFacing", asVector(viewDirection));
    const GLint objectIdUniformLocation =
        workbenchEffectProgram_.uniformLocation("uObjectId");
    const GLint outlineObjectIdUniformLocation =
        workbenchEffectProgram_.uniformLocation("uOutlineObjectId");

    vertexArray_.bind();
    vertexBuffer_.bind();
    if (geometryDirty_) {
        vertexBuffer_.allocate(vertices_.constData(),
                               vertices_.size() *
                                   static_cast<int>(sizeof(SurfaceVertex)));
        geometryDirty_ = false;
    }
    for (int rangeIndex = 0; rangeIndex < ranges_.size(); ++rangeIndex) {
        const DrawRange &range = ranges_[rangeIndex];
        if (range.count <= 0) {
            continue;
        }
        const GLuint objectId = range.effectObjectId;
        GLuint outlineObjectId = range.outlineHandledBySceneStroke
                                     ? 0u
                                     : objectId;
        if (range.selected && !range.outlineHandledBySceneStroke) {
            outlineObjectId |= 0x80000000u;
        }
        glUniform1ui(objectIdUniformLocation, objectId);
        glUniform1ui(outlineObjectIdUniformLocation, outlineObjectId);
        workbenchEffectProgram_.setUniformValue(
            "uWorldOffset",
            QVector3D(static_cast<float>(range.offset.x),
                      static_cast<float>(range.offset.y),
                      static_cast<float>(range.offset.z)));
        glDrawArrays(GL_TRIANGLES, range.first, range.count);
    }
    vertexBuffer_.release();
    vertexArray_.release();
    workbenchEffectProgram_.release();

    if (snapshotDepth) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, workbenchEffectFramebuffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cavityDepthFramebuffer_);
        const bool sourceComplete =
            glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE;
        const bool destinationComplete =
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE;
        if (sourceComplete && destinationComplete) {
            glBlitFramebuffer(0, 0, pixelSize.width(), pixelSize.height(),
                              0, 0, pixelSize.width(), pixelSize.height(),
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            if (snapshotDepthReady != nullptr) {
                *snapshotDepthReady = true;
            }
        } else {
            if (snapshotDepthReady != nullptr) {
                *snapshotDepthReady = false;
            }
        }
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glUseProgram(static_cast<GLuint>(previousProgram));
    return true;
}

bool ViewportSurfaceRenderer::renderShadowMap(
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const ViewportShadingSettings &settings,
    bool fallbackOnly)
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
    const QVector3D lightDirection = workbenchShadowDirectionWorld(settings);
    const QVector3D eye = center - lightDirection * (radius * 2.5f);
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
        if (range.count <= 0 ||
            (fallbackOnly && range.shadowVolumeSupported)) {
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

bool ViewportSurfaceRenderer::renderShadowVolumes(
    const QVector3D &lightDirection,
    const QMatrix4x4 &viewProjection,
    const QSize &viewportSize,
    qreal devicePixelRatio)
{
    struct EdgeAdjacency {
        int count = 0;
        int firstTriangle = -1;
        int secondTriangle = -1;
        int firstFrom = -1;
        int firstTo = -1;
        int secondFrom = -1;
        int secondTo = -1;
    };
    const auto edgeKey = [](int first, int second) {
        const quint32 low = static_cast<quint32>(std::min(first, second));
        const quint32 high = static_cast<quint32>(std::max(first, second));
        return (quint64(low) << 32u) | high;
    };
    const auto appendTriangle = [](QVector<QVector3D> *vertices,
                                   const QVector3D &a,
                                   const QVector3D &b,
                                   const QVector3D &c) {
        vertices->append(a);
        vertices->append(b);
        vertices->append(c);
    };

    // OpenGL clip space uses z <= w for the far clip plane. Row 3 minus row 2
    // gives its inward clip-plane covector; negate the normal to get the
    // outward plane orientation used by Blender's shadow extrusion math.
    const QVector4D inwardFarClipPlane(
        viewProjection(3, 0) - viewProjection(2, 0),
        viewProjection(3, 1) - viewProjection(2, 1),
        viewProjection(3, 2) - viewProjection(2, 2),
        viewProjection(3, 3) - viewProjection(2, 3));
    const float farPlaneNormalLength =
        QVector3D(inwardFarClipPlane.x(), inwardFarClipPlane.y(),
                  inwardFarClipPlane.z()).length();
    if (!std::isfinite(farPlaneNormalLength) ||
        farPlaneNormalLength <= 1.0e-8f) {
        return false;
    }
    const QVector3D farPlaneNormal(
        -inwardFarClipPlane.x() / farPlaneNormalLength,
        -inwardFarClipPlane.y() / farPlaneNormalLength,
        -inwardFarClipPlane.z() / farPlaneNormalLength);
    const float farPlaneOffset =
        inwardFarClipPlane.w() / farPlaneNormalLength;
    const float lightFarPlaneDot =
        QVector3D::dotProduct(lightDirection, farPlaneNormal);
    const auto extrudeToFarPlane = [&](const QVector3D &position) {
        float extrusionDistance = 1.0e5f;
        if (lightFarPlaneDot > 0.0f) {
            const float signedDistance =
                QVector3D::dotProduct(farPlaneNormal, position) -
                farPlaneOffset;
            extrusionDistance = -signedDistance / lightFarPlaneDot;
            // Keep the cap just inside the far plane, matching Blender.
            extrusionDistance -= 1.0e-3f;
        }
        return position + lightDirection * extrusionDistance;
    };

    QVector<QVector3D> volumeVertices;
    for (const DrawRange &range : ranges_) {
        if (!range.shadowVolumeSupported || range.shadowTriangles.isEmpty()) {
            continue;
        }
        const QVector3D offset(static_cast<float>(range.offset.x),
                               static_cast<float>(range.offset.y),
                               static_cast<float>(range.offset.z));
        QVector<bool> facingLight;
        facingLight.reserve(range.shadowTriangles.size());
        QHash<quint64, EdgeAdjacency> edges;

        for (int triangleIndex = 0;
             triangleIndex < range.shadowTriangles.size(); ++triangleIndex) {
            const auto &triangle = range.shadowTriangles[triangleIndex];
            for (const int index : triangle) {
                if (index < 0 || index >= range.shadowVertices.size()) {
                    return false;
                }
            }
            const QVector3D a = range.shadowVertices[triangle[0]] + offset;
            const QVector3D b = range.shadowVertices[triangle[1]] + offset;
            const QVector3D c = range.shadowVertices[triangle[2]] + offset;
            const QVector3D normal = QVector3D::crossProduct(b - a, c - a);
            if (!std::isfinite(normal.lengthSquared()) ||
                normal.lengthSquared() <= 1.0e-18f) {
                return false;
            }
            // Blender's pass marks dot(N, light-ray-direction) > 0 as the
            // back-facing side. The opposite faces receive light.
            const bool faceLit =
                QVector3D::dotProduct(normal, lightDirection) < 0.0f;
            facingLight.append(faceLit);

            if (faceLit) {
                const QVector3D farA = extrudeToFarPlane(a);
                const QVector3D farB = extrudeToFarPlane(b);
                const QVector3D farC = extrudeToFarPlane(c);
                // Only light-facing faces cap the volume; their exposed
                // boundary is closed by the silhouette side quads.
                appendTriangle(&volumeVertices, a, b, c);
                appendTriangle(&volumeVertices, farC, farB, farA);
            }

            const int directedEdges[][2] = {
                {triangle[0], triangle[1]},
                {triangle[1], triangle[2]},
                {triangle[2], triangle[0]}};
            for (const auto &edge : directedEdges) {
                EdgeAdjacency &adjacent = edges[edgeKey(edge[0], edge[1])];
                if (adjacent.count == 0) {
                    adjacent.firstTriangle = triangleIndex;
                    adjacent.firstFrom = edge[0];
                    adjacent.firstTo = edge[1];
                } else if (adjacent.count == 1) {
                    adjacent.secondTriangle = triangleIndex;
                    adjacent.secondFrom = edge[0];
                    adjacent.secondTo = edge[1];
                }
                ++adjacent.count;
                if (adjacent.count > 2) return false;
            }
        }

        for (const EdgeAdjacency &adjacent : edges) {
            if (adjacent.count != 2 || adjacent.firstTriangle < 0 ||
                adjacent.secondTriangle < 0) {
                return false;
            }
            const bool firstFacesLight =
                facingLight[adjacent.firstTriangle];
            const bool secondFacesLight =
                facingLight[adjacent.secondTriangle];
            if (firstFacesLight == secondFacesLight) continue;

            // Take the directed edge from the light-facing triangle. This
            // winding closes the volume against both caps.
            const int from = firstFacesLight ? adjacent.firstFrom
                                             : adjacent.secondFrom;
            const int to = firstFacesLight ? adjacent.firstTo
                                           : adjacent.secondTo;
            const QVector3D a = range.shadowVertices[from] + offset;
            const QVector3D b = range.shadowVertices[to] + offset;
            const QVector3D farA = extrudeToFarPlane(a);
            const QVector3D farB = extrudeToFarPlane(b);
            appendTriangle(&volumeVertices, a, farA, farB);
            appendTriangle(&volumeVertices, a, farB, b);
        }
    }
    if (volumeVertices.isEmpty()) return false;

    GLint previousViewport[4] = {};
    GLint previousDepthFunction = GL_LESS;
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousFrontFace = GL_CCW;
    GLint previousStencilClearValue = 0;
    GLint previousStencilFunc = GL_ALWAYS;
    GLint previousStencilRef = 0;
    GLint previousStencilValueMask = -1;
    GLint previousStencilWriteMask = -1;
    GLint previousStencilFail = GL_KEEP;
    GLint previousStencilDepthFail = GL_KEEP;
    GLint previousStencilDepthPass = GL_KEEP;
    GLint previousStencilBackFunc = GL_ALWAYS;
    GLint previousStencilBackRef = 0;
    GLint previousStencilBackValueMask = -1;
    GLint previousStencilBackWriteMask = -1;
    GLint previousStencilBackFail = GL_KEEP;
    GLint previousStencilBackDepthFail = GL_KEEP;
    GLint previousStencilBackDepthPass = GL_KEEP;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previousDepthMask = GL_TRUE;
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previousDepthClamp = glIsEnabled(GL_DEPTH_CLAMP);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_FRONT_FACE, &previousFrontFace);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &previousStencilClearValue);
    glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunc);
    glGetIntegerv(GL_STENCIL_REF, &previousStencilRef);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
    glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &previousStencilDepthFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &previousStencilDepthPass);
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &previousStencilBackFunc);
    glGetIntegerv(GL_STENCIL_BACK_REF, &previousStencilBackRef);
    glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK,
                  &previousStencilBackValueMask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK,
                  &previousStencilBackWriteMask);
    glGetIntegerv(GL_STENCIL_BACK_FAIL, &previousStencilBackFail);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                  &previousStencilBackDepthFail);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS,
                  &previousStencilBackDepthPass);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    glViewport(0, 0, qRound(viewportSize.width() * dpr),
               qRound(viewportSize.height() * dpr));
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFFFFFFFFu);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFFu);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

    shadowVolumeVertexArray_.bind();
    shadowVolumeVertexBuffer_.bind();
    shadowVolumeVertexBuffer_.allocate(
        volumeVertices.constData(),
        volumeVertices.size() * static_cast<int>(sizeof(QVector3D)));
    shadowProgram_.bind();
    shadowProgram_.setUniformValue("uShadowViewProjection", viewProjection);
    shadowProgram_.setUniformValue("uWorldOffset", QVector3D());
    glDrawArrays(GL_TRIANGLES, 0, volumeVertices.size());
    shadowProgram_.release();
    shadowVolumeVertexBuffer_.release();
    shadowVolumeVertexArray_.release();

    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousStencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (previousDepthClamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glFrontFace(static_cast<GLenum>(previousFrontFace));
    glClearStencil(previousStencilClearValue);
    glStencilFuncSeparate(GL_FRONT, static_cast<GLenum>(previousStencilFunc),
                          previousStencilRef,
                          static_cast<GLuint>(previousStencilValueMask));
    glStencilMaskSeparate(GL_FRONT,
                          static_cast<GLuint>(previousStencilWriteMask));
    glStencilOpSeparate(GL_FRONT, static_cast<GLenum>(previousStencilFail),
                        static_cast<GLenum>(previousStencilDepthFail),
                        static_cast<GLenum>(previousStencilDepthPass));
    glStencilFuncSeparate(GL_BACK,
                          static_cast<GLenum>(previousStencilBackFunc),
                          previousStencilBackRef,
                          static_cast<GLuint>(previousStencilBackValueMask));
    glStencilMaskSeparate(GL_BACK,
                          static_cast<GLuint>(previousStencilBackWriteMask));
    glStencilOpSeparate(GL_BACK,
                        static_cast<GLenum>(previousStencilBackFail),
                        static_cast<GLenum>(previousStencilBackDepthFail),
                        static_cast<GLenum>(previousStencilBackDepthPass));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glUseProgram(static_cast<GLuint>(previousProgram));
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

GLuint ViewportSurfaceRenderer::ensureImageTexture(const QString &path, bool srgb)
{
    return ensureImageTexture(path, {}, srgb);
}

GLuint ViewportSurfaceRenderer::ensureImageTexture(
    const QString &path, const QString &packedDataBase64, bool srgb)
{
    if (path.isEmpty() && packedDataBase64.isEmpty()) return 0;
    const QString sourceKey = !packedDataBase64.isEmpty()
        ? QStringLiteral("PACKED\n") + QString::fromLatin1(
              QCryptographicHash::hash(packedDataBase64.toLatin1(),
                                       QCryptographicHash::Sha256).toHex())
        : path;
    const QString cacheKey = sourceKey + (srgb ? QStringLiteral("\nSRGB")
                                               : QStringLiteral("\nLINEAR"));
    const auto cached = imageTextures_.constFind(cacheKey);
    if (cached != imageTextures_.cend()) return cached.value();
    QImage image;
    if (!packedDataBase64.isEmpty()) {
        const QByteArray encoded = packedDataBase64.toLatin1();
        if (encoded.size() > 44 * 1024 * 1024) return 0;
        const QByteArray bytes = QByteArray::fromBase64(encoded);
        if (bytes.size() > 32 * 1024 * 1024) return 0;
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) return 0;
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
            static_cast<quint64>(size.width()) * size.height() > 67108864ULL) {
            imageTextures_.insert(cacheKey, 0);
            return 0;
        }
        image = reader.read();
    } else {
        const QFileInfo info(path);
        const QByteArray suffix = info.suffix().toLower().toLatin1();
        static const QSet<QByteArray> allowedFormats{
            "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"};
        if (!info.isFile() || !allowedFormats.contains(suffix) ||
            info.size() <= 0 || info.size() > 256LL * 1024LL * 1024LL) {
            imageTextures_.insert(cacheKey, 0);
            return 0;
        }
        QImageReader reader(path);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
            static_cast<quint64>(size.width()) * size.height() > 67108864ULL) {
            imageTextures_.insert(cacheKey, 0);
            return 0;
        }
        image = reader.read();
    }
    if (image.isNull()) {
        imageTextures_.insert(cacheKey, 0);
        return 0;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888)
                 .mirrored(false, true);
    if (image.width() > 16384 || image.height() > 16384 ||
        static_cast<quint64>(image.width()) * image.height() > 67108864ULL) {
        imageTextures_.insert(cacheKey, 0);
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        imageTextures_.insert(cacheKey, 0);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                 image.width(), image.height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    imageTextures_.insert(cacheKey, texture);
    return texture;
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

bool ViewportSurfaceRenderer::ensureCavityJitterTexture(int totalSamples)
{
    const int sampleCount = std::clamp(totalSamples, 1,
                                       kWorkbenchCavityMaxSamples);
    if (cavityJitterTexture_ != 0 &&
        cavityJitterSampleCount_ == sampleCount) {
        return true;
    }

    const QByteArray blueNoiseRg = QByteArray::fromBase64(QByteArray(
        detail::kWorkbenchCavityBlueNoiseRgBase64,
        static_cast<qsizetype>(
            sizeof(detail::kWorkbenchCavityBlueNoiseRgBase64) - 1)));
    constexpr int kJitterTextureSize = 64;
    constexpr int kBlueNoiseChannelCount = 2;
    constexpr int kExpectedBlueNoiseBytes =
        kJitterTextureSize * kJitterTextureSize * kBlueNoiseChannelCount;
    if (blueNoiseRg.size() != kExpectedBlueNoiseBytes) {
        return false;
    }

    // Blender's LUT channels are quantized to 1/256 steps. The texture is
    // rebuilt when the total Workbench cavity sample count changes because
    // its blue-noise radius offset is scaled by 1 / total_samples.
    std::array<float, kJitterTextureSize * kJitterTextureSize * 3> pixels{};
    constexpr float kInverseBlueNoiseRange = 1.0f / 256.0f;
    constexpr double kPi = 3.14159265358979323846;
    const float inverseSampleCount = 1.0f / static_cast<float>(sampleCount);
    for (int pixelIndex = 0;
         pixelIndex < kJitterTextureSize * kJitterTextureSize;
         ++pixelIndex) {
        const int sourceIndex = pixelIndex * kBlueNoiseChannelCount;
        const float angleSample = static_cast<float>(
            static_cast<unsigned char>(blueNoiseRg.at(sourceIndex))) *
            kInverseBlueNoiseRange;
        const float radiusSample = static_cast<float>(
            static_cast<unsigned char>(blueNoiseRg.at(sourceIndex + 1))) *
            kInverseBlueNoiseRange;
        const double angle = static_cast<double>(angleSample * 2.0f) * kPi;
        const int destinationIndex = pixelIndex * 3;
        pixels[destinationIndex] = static_cast<float>(std::cos(angle));
        pixels[destinationIndex + 1] = static_cast<float>(std::sin(angle));
        pixels[destinationIndex + 2] =
            std::clamp(radiusSample - 0.5f, -0.499f, 0.499f) *
            inverseSampleCount;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextureBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding);
    if (cavityJitterTexture_ == 0) {
        glGenTextures(1, &cavityJitterTexture_);
    }
    if (cavityJitterTexture_ == 0) {
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousTextureBinding));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, cavityJitterTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F,
                 kJitterTextureSize, kJitterTextureSize, 0, GL_RGB, GL_FLOAT,
                 pixels.data());
    glBindTexture(GL_TEXTURE_2D,
                  static_cast<GLuint>(previousTextureBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    cavityJitterSampleCount_ = sampleCount;
    return true;
}

bool ViewportSurfaceRenderer::draw(
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    bool previewOverlay,
    bool clearDepth,
    bool selectionOverlay,
    const ViewportRenderTargets *sharedTargets,
    quint32 *populatedAttachments)
{
    if (populatedAttachments != nullptr) {
        *populatedAttachments = 0;
    }
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
    const bool hasPartialAlpha = settings.mode == ViewportShadingMode::Solid &&
        !previewOverlay && !selectionOverlay && !settings.xrayEnabled() &&
        std::any_of(ranges_.cbegin(), ranges_.cend(),
                    [&settings](const DrawRange &range) {
                        if (range.count <= 0) {
                            return false;
                        }
                        const qreal alpha = workbenchSourceAlpha(
                            settings.colorMode,
                            range.displayColorSceneLinear.w(),
                            range.workbenchMaterial, settings.customColor);
                        const float packedAlpha =
                            workbenchPackUnitFloatToUnorm8(
                                static_cast<float>(alpha));
                        return workbenchEffectiveAlpha(packedAlpha, settings,
                                                       false) <
                               1.0 - 1.0e-6;
                    });
    const bool primaryOpaqueSceneDraw =
        settings.mode == ViewportShadingMode::Solid && !previewOverlay &&
        !selectionOverlay && clearDepth && !settings.xrayEnabled() &&
        !hasPartialAlpha && !vertices_.isEmpty();
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
        } else if (hasPartialAlpha) {
            // The surface-range draw below renders opaque ranges first and
            // blends transparent ranges back-to-front against opaque depth.
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

    const bool cavityRequested = primaryOpaqueSceneDraw && settings.cavity;
    const bool outlineRequested = primaryOpaqueSceneDraw && settings.outline;
    const bool worldSsaoRequested = cavityRequested &&
        (settings.cavityType == ViewportCavityType::World ||
         settings.cavityType == ViewportCavityType::Both);
    const int aaSampleCount = sharedTargets != nullptr
                                  ? std::clamp(sharedTargets->sampleCount, 1, 32)
                                  : 1;
    const int cavitySamplesPerIteration =
        std::clamp(settings.cavitySamples, 1, 500);
    const int cavityTotalSampleCount = std::min(
        aaSampleCount * cavitySamplesPerIteration,
        kWorkbenchCavityMaxSamples);
    const bool ssaoJitterReady =
        worldSsaoRequested &&
        ensureCavityJitterTexture(cavityTotalSampleCount);
    const bool ssaoEnabled = worldSsaoRequested && ssaoJitterReady;
    const bool sharedPrepassRequested = sharedTargets != nullptr &&
                                       (cavityRequested || outlineRequested);
    bool sharedPrepassReady = false;
    bool cavityDepthReady = false;
    if (sharedPrepassRequested) {
        bool snapshotDepthReady = !ssaoEnabled;
        sharedPrepassReady = renderWorkbenchEffectPrepass(
            transform, viewportSize, devicePixelRatio, settings,
            *sharedTargets, ssaoEnabled, &snapshotDepthReady);
        cavityDepthReady = sharedPrepassReady && ssaoEnabled &&
                           snapshotDepthReady;
        if (sharedPrepassReady && populatedAttachments != nullptr) {
            *populatedAttachments =
                ViewportRenderTargets::Depth |
                ViewportRenderTargets::ViewSpaceNormal |
                ViewportRenderTargets::ObjectId;
        }
    }
    if (sharedPrepassReady) {
        // The prepass has already filled the shared depth attachment. The
        // color pass redraws the same visible samples into the scene target.
        glDepthFunc(GL_LEQUAL);
    }

    const bool shadowMapRequested =
        settings.mode == ViewportShadingMode::Solid && settings.shadows &&
        !settings.xrayEnabled() && !previewOverlay && !selectionOverlay &&
        clearDepth;
    bool shadowVolumesAvailable = false;
    bool sceneDepthPrepassReady = sharedPrepassReady;
    if (shadowMapRequested && primaryOpaqueSceneDraw &&
        std::any_of(ranges_.cbegin(), ranges_.cend(),
                    [](const DrawRange &range) {
                        return range.count > 0 &&
                               range.shadowVolumeSupported;
                    })) {
        GLint stencilBits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
        if (stencilBits >= 8) {
            if (!sceneDepthPrepassReady) {
                GLint previousViewport[4] = {};
                GLint previousDepthFunction = GL_LESS;
                GLint previousProgram = 0;
                GLboolean previousColorMask[4] = {
                    GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
                GLboolean previousDepthMask = GL_TRUE;
                const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
                const GLboolean previousBlend = glIsEnabled(GL_BLEND);
                const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
                glGetIntegerv(GL_VIEWPORT, previousViewport);
                glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
                glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
                glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
                glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
                const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
                glViewport(0, 0, qRound(viewportSize.width() * dpr),
                           qRound(viewportSize.height() * dpr));
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                if (settings.backfaceCulling) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                } else {
                    glDisable(GL_CULL_FACE);
                }
                vertexArray_.bind();
                vertexBuffer_.bind();
                if (geometryDirty_) {
                    vertexBuffer_.allocate(
                        vertices_.constData(),
                        vertices_.size() *
                            static_cast<int>(sizeof(SurfaceVertex)));
                    geometryDirty_ = false;
                }
                shadowProgram_.bind();
                shadowProgram_.setUniformValue(
                    "uShadowViewProjection",
                    viewportViewProjection(transform, viewportSize));
                for (const DrawRange &range : ranges_) {
                    if (range.count <= 0) continue;
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
                glViewport(previousViewport[0], previousViewport[1],
                           previousViewport[2], previousViewport[3]);
                glColorMask(previousColorMask[0], previousColorMask[1],
                            previousColorMask[2], previousColorMask[3]);
                glDepthMask(previousDepthMask);
                glDepthFunc(static_cast<GLenum>(previousDepthFunction));
                if (previousDepthTest) glEnable(GL_DEPTH_TEST);
                else glDisable(GL_DEPTH_TEST);
                if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
                if (previousCullFace) glEnable(GL_CULL_FACE);
                else glDisable(GL_CULL_FACE);
                glUseProgram(static_cast<GLuint>(previousProgram));
                sceneDepthPrepassReady = true;
            }

            if (sceneDepthPrepassReady) {
                const QMatrix4x4 shadowViewProjection =
                    viewportViewProjection(transform, viewportSize);
                shadowVolumesAvailable = renderShadowVolumes(
                    workbenchShadowDirectionWorld(settings),
                    shadowViewProjection, viewportSize, devicePixelRatio);
                if (shadowVolumesAvailable) {
                    glDepthFunc(GL_LEQUAL);
                }
            }
        }
    }

    bool shadowMapAvailable = false;
    if (shadowMapRequested) {
        const bool fallbackOnly = shadowVolumesAvailable && !hasPartialAlpha;
        const bool hasFallbackCasters = std::any_of(
            ranges_.cbegin(), ranges_.cend(),
            [fallbackOnly](const DrawRange &range) {
                return range.count > 0 &&
                       (!fallbackOnly || !range.shadowVolumeSupported);
            });
        if (hasFallbackCasters) {
            shadowMapAvailable = renderShadowMap(
                transform, viewportSize, settings, fallbackOnly);
        }
    }
    const bool useShadows = shadowVolumesAvailable || shadowMapAvailable;
    if (sceneDepthPrepassReady) {
        glDepthFunc(GL_LEQUAL);
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
        program_.setUniformValue("uUseSpecular",
                                 settings.specularLighting &&
                                     lighting.useSpecular);
        const QVector3D shadowDirectionWorld =
            workbenchShadowDirectionWorld(settings);
        const QVector3D shadowDirectionView(
            QVector3D::dotProduct(shadowDirectionWorld,
                                  asVector(viewRight)),
            QVector3D::dotProduct(shadowDirectionWorld,
                                  asVector(viewUp)),
            QVector3D::dotProduct(shadowDirectionWorld,
                                  asVector(viewFacing)));
        const int lightingMode = settings.lightingMode == ViewportLightingMode::Studio
                                     ? 0
                                     : settings.lightingMode == ViewportLightingMode::MatCap
                                           ? 1
                                           : 2;
        program_.setUniformValue("uLightingMode", lightingMode);
        program_.setUniformValue("uMatcapFlipX", settings.matcapFlipX);
        const bool matcapAvailable =
            settings.lightingMode == ViewportLightingMode::MatCap &&
            ensureMatcapTexture(settings.matcapPreset);
        program_.setUniformValue("uMatcapAvailable", matcapAvailable);
        const bool agxDisplayAvailable = ensureAgxDisplayTexture();
        program_.setUniformValue("uAgxDisplayAvailable", agxDisplayAvailable);
        program_.setUniformValue("uUseShadows", useShadows);
        program_.setUniformValue("uUseShadowMap", shadowMapAvailable);
        program_.setUniformValue("uShadowVolumeShadowed", false);
        program_.setUniformValue("uShadowIntensity",
                                 static_cast<float>(settings.shadowIntensity));
        program_.setUniformValue("uShadowBias", 0.0005f);
        program_.setUniformValue("uShadowFocus",
                                 static_cast<float>(settings.shadowFocus));
        program_.setUniformValue("uShadowShift",
                                 static_cast<float>(std::clamp(
                                     settings.shadowOffset, 0.0, 1.0)));
        program_.setUniformValue("uShadowDirectionView",
                                 shadowDirectionView);
        program_.setUniformValue("uShadowViewProjection",
                                 shadowViewProjection_);
        const bool cavityAvailable = sharedPrepassReady && cavityRequested;
        const bool screenCurvatureRequested = cavityAvailable &&
            (settings.cavityType == ViewportCavityType::Screen ||
             settings.cavityType == ViewportCavityType::Both);
        program_.setUniformValue("uUseCavity", cavityAvailable);
        program_.setUniformValue("uUseSsao",
                                 cavityAvailable && ssaoEnabled &&
                                     cavityDepthReady);
        program_.setUniformValue("uUseCurvature",
                                 screenCurvatureRequested);
        QMatrix4x4 inverseViewProjection;
        if (sharedTargets != nullptr) {
            inverseViewProjection = sharedTargets->inverseViewProjection;
        }
        program_.setUniformValue("uInverseViewProjection",
                                 inverseViewProjection);
        const qreal viewScale = std::max<qreal>(
            transform.viewScalePixelsPerWorldUnit(viewportSize), 1.0e-8);
        qreal projectionScale = viewScale;
        if (transform.isPerspectiveEnabled()) {
            const Point3D target = transform.viewTarget();
            const qreal cameraDistance = std::hypot(
                std::hypot(cameraPosition.x - target.x,
                           cameraPosition.y - target.y),
                cameraPosition.z - target.z);
            projectionScale *= cameraDistance;
        }
        program_.setUniformValue(
            "uCavityProjectionScale",
            QVector2D(static_cast<float>(2.0 * projectionScale /
                                         viewportSize.width()),
                      static_cast<float>(2.0 * projectionScale /
                                         viewportSize.height())));
        program_.setUniformValue("uCavityUiScale", static_cast<float>(dpr));
        program_.setUniformValue("uCavityDistance",
                                 static_cast<float>(settings.cavityDistance));
        program_.setUniformValue("uCavityAttenuation",
                                 static_cast<float>(settings.cavityAttenuation));
        program_.setUniformValue("uCavityValleyFactor",
                                 static_cast<float>(settings.cavityValleyFactor));
        program_.setUniformValue("uCavityRidgeFactor",
                                 static_cast<float>(settings.cavityRidgeFactor));
        program_.setUniformValue(
            "uCurvatureRidgeControl",
            0.5f / std::max(static_cast<float>(settings.curvatureRidgeFactor) *
                                static_cast<float>(settings.curvatureRidgeFactor),
                            1.0e-4f));
        program_.setUniformValue(
            "uCurvatureValleyControl",
            0.7f / std::max(static_cast<float>(settings.curvatureValleyFactor) *
                                static_cast<float>(settings.curvatureValleyFactor),
                            1.0e-4f));
        const int cavityIterationCount = std::max(
            1, cavityTotalSampleCount / cavitySamplesPerIteration);
        const int cavityIteration = sharedTargets != nullptr
            ? std::max(0, sharedTargets->sampleIndex) % cavityIterationCount
            : 0;
        const int cavitySampleStart = cavityIteration *
                                      cavitySamplesPerIteration;
        const int cavitySampleEnd = cavitySampleStart +
                                    cavitySamplesPerIteration;
        program_.setUniformValue("uCavitySampleStart", cavitySampleStart);
        program_.setUniformValue("uCavitySampleEnd", cavitySampleEnd);
        program_.setUniformValue("uCavitySamplesPerIteration",
                                 cavitySamplesPerIteration);
        program_.setUniformValue(
            "uCavitySampleCountInv",
            1.0f / static_cast<float>(cavitySampleEnd - cavitySampleStart));
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousMatcapTextureBinding = 0;
        GLint previousAgxTextureBinding = 0;
        GLint previousShadowTextureBinding = 0;
        GLint previousCavityNormalTextureBinding = 0;
        GLint previousCavityObjectIdTextureBinding = 0;
        GLint previousCavityDepthTextureBinding = 0;
        GLint previousCavityJitterTextureBinding = 0;
        GLint previousImageTextureBinding = 0;
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
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityNormalTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityAvailable && sharedTargets != nullptr
                          ? sharedTargets->viewSpaceNormalTexture
                          : 0);
        program_.setUniformValue("uCavityNormal", 3);
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityObjectIdTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityAvailable && sharedTargets != nullptr
                          ? sharedTargets->objectIdTexture
                          : 0);
        program_.setUniformValue("uCavityObjectId", 4);
        glActiveTexture(GL_TEXTURE5);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityDepthTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityDepthReady ? cavityDepthTexture_ : 0);
        program_.setUniformValue("uCavityDepth", 5);
        glActiveTexture(GL_TEXTURE6);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousImageTextureBinding);
        program_.setUniformValue("uWorkbenchImage", 6);
        glActiveTexture(GL_TEXTURE7);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityJitterTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityAvailable && ssaoEnabled
                          ? cavityJitterTexture_
                          : 0);
        program_.setUniformValue("uCavityJitterTexture", 7);
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
        const auto rangeAlpha = [&](const DrawRange &range) {
            const qreal sourceAlpha = workbenchSourceAlpha(
                settings.colorMode, range.displayColorSceneLinear.w(),
                range.workbenchMaterial, settings.customColor);
            const WorkbenchPackedMaterialData packed = workbenchPackMaterialData(
                0.0f, 0.4f, static_cast<float>(sourceAlpha));
            return workbenchEffectiveAlpha(packed.alpha, settings, false);
        };
        struct TransparentTriangle {
            int rangeIndex = -1;
            int firstVertex = 0;
            qreal projectedDepth = 0.0;
        };
        QVector<int> opaqueRangeIndices;
        QVector<TransparentTriangle> transparentTriangles;
        for (int index = 0; index < ranges_.size(); ++index) {
            const DrawRange &range = ranges_[index];
            if (range.count <= 0) {
                continue;
            }
            if (rangeAlpha(range) >= 1.0 - 1.0e-6) {
                opaqueRangeIndices.append(index);
                continue;
            }
            // A range can cover a large or concave tessellated NURBS surface.
            // Sorting one average for the whole object lets distant parts
            // incorrectly occlude nearer parts, so order its display triangles
            // independently while keeping the saved NURBS and shared display
            // tessellation unchanged.
            for (int first = range.first;
                 first + 2 < range.first + range.count; first += 3) {
                QVector4D center;
                for (int vertexIndex = first; vertexIndex < first + 3;
                     ++vertexIndex) {
                    const SurfaceVertex &vertex = vertices_[vertexIndex];
                    center += QVector4D(vertex.x, vertex.y, vertex.z, 1.0f);
                }
                center /= 3.0f;
                center.setX(center.x() + static_cast<float>(range.offset.x));
                center.setY(center.y() + static_cast<float>(range.offset.y));
                center.setZ(center.z() + static_cast<float>(range.offset.z));
                const QVector4D clip = viewProjection * center;
                const qreal depth = std::abs(clip.w()) > 1.0e-8f
                    ? static_cast<qreal>(clip.z() / clip.w())
                    : static_cast<qreal>(clip.z());
                transparentTriangles.append(
                    {index, first, std::isfinite(depth) ? depth : 0.0});
            }
        }
        std::stable_sort(transparentTriangles.begin(),
                         transparentTriangles.end(),
                         [](const TransparentTriangle &left,
                            const TransparentTriangle &right) {
                             if (left.projectedDepth !=
                                 right.projectedDepth) {
                                 return left.projectedDepth >
                                        right.projectedDepth;
                             }
                             if (left.rangeIndex != right.rangeIndex) {
                                 return left.rangeIndex < right.rangeIndex;
                             }
                             return left.firstVertex < right.firstVertex;
                         });

        const auto drawRange = [&](const DrawRange &range,
                                   bool volumeShadowed,
                                   int firstVertex,
                                   int vertexCount) {
            program_.setUniformValue("uShadowVolumeShadowed",
                                     volumeShadowed);
            program_.setUniformValue(
                "uUseAttributeColor",
                settings.colorMode == ViewportColorMode::Attribute &&
                    range.useAttributeColor);
            const GLuint imageTexture = settings.colorMode == ViewportColorMode::Texture &&
                    range.useTextureColor
                ? ensureImageTexture(range.textureImagePath,
                                     range.workbenchMaterial.textureImagePackedDataBase64,
                                     range.textureImageSrgb) : 0;
            program_.setUniformValue("uUseTextureColor", imageTexture != 0);
            const QString alphaMode = range.workbenchMaterial.textureImageAlphaMode;
            const bool ignoreTextureAlpha =
                alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
                alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
                alphaMode.compare(QStringLiteral("CHANNEL_PACKED"), Qt::CaseInsensitive) == 0;
            program_.setUniformValue("uTexturePremultiplied",
                alphaMode.compare(QStringLiteral("PREMUL"), Qt::CaseInsensitive) == 0);
            program_.setUniformValue("uTextureAlphaCutoff", !ignoreTextureAlpha);
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, imageTexture);
            const QVector3D offset(static_cast<float>(range.offset.x),
                                   static_cast<float>(range.offset.y),
                                   static_cast<float>(range.offset.z));
            program_.setUniformValue("uWorldOffset", offset);
            program_.setUniformValue("uEditModeSelected",
                                     range.selected && !previewOverlay);
            const WorkbenchMaterialData &material = range.workbenchMaterial;
            QVector3D baseColorLinear =
                workbenchDefaultSolidMaterialDiffuseColor();
            float roughness = 0.4f;
            float metallic = 0.0f;
            if (previewOverlay) {
                const QColor previewColor(QStringLiteral("#d89a4e"));
                baseColorLinear = workbenchSrgbToSceneLinear(
                    QVector3D(previewColor.redF(), previewColor.greenF(),
                              previewColor.blueF()));
            } else {
                switch (settings.colorMode) {
                case ViewportColorMode::Material:
                case ViewportColorMode::Attribute:
                case ViewportColorMode::Texture:
                    // Attribute and Texture have no color-attribute, UV,
                    // or image inputs in the scene model yet; Blender
                    // falls back to the Material source for these modes.
                    baseColorLinear = material.baseColorSceneLinear;
                    roughness = static_cast<float>(material.roughness);
                    metallic = static_cast<float>(material.metallic);
                    break;
                case ViewportColorMode::Object:
                    baseColorLinear = QVector3D(
                        range.displayColorSceneLinear.x(),
                        range.displayColorSceneLinear.y(),
                        range.displayColorSceneLinear.z());
                    break;
                case ViewportColorMode::Random: {
                    const QColor randomColor =
                        workbenchRandomColor(range.objectSeed,
                                             range.displayName,
                                             range.libraryPath);
                    baseColorLinear = workbenchRec709ToSceneLinear(
                        QVector3D(randomColor.redF(), randomColor.greenF(),
                                  randomColor.blueF()));
                    break;
                }
                case ViewportColorMode::Custom:
                    baseColorLinear = workbenchSrgbToSceneLinear(
                        QVector3D(settings.customColor.redF(),
                                  settings.customColor.greenF(),
                                  settings.customColor.blueF()));
                    break;
                }
            }
            qreal sourceAlpha = workbenchSourceAlpha(
                settings.colorMode, range.displayColorSceneLinear.w(),
                material, settings.customColor);
            const WorkbenchPackedMaterialData packed =
                workbenchPackMaterialData(metallic, roughness,
                                          static_cast<float>(sourceAlpha));
            roughness = packed.roughness;
            metallic = packed.metallic;
            if (!previewOverlay) {
                sourceAlpha = packed.alpha;
            }
            const float alpha = static_cast<float>(workbenchEffectiveAlpha(
                sourceAlpha, settings, previewOverlay));
            program_.setUniformValue("uRoughness", roughness);
            program_.setUniformValue("uMetallic", metallic);
            program_.setUniformValue(
                "uBaseColor",
                QVector4D(baseColorLinear.x(), baseColorLinear.y(),
                          baseColorLinear.z(), alpha));
            glDrawArrays(GL_TRIANGLES, firstVertex, vertexCount);
        };
        const auto drawSurfaceRanges = [&](bool volumeShadowed) {
            if (hasPartialAlpha) {
                // Render opaque surfaces first to establish depth, then blend
                // translucent surfaces back-to-front without updating depth.
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                for (int index : opaqueRangeIndices) {
                    const DrawRange &range = ranges_[index];
                    drawRange(range, volumeShadowed, range.first,
                              range.count);
                }
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                for (const TransparentTriangle &triangle :
                     transparentTriangles) {
                    drawRange(ranges_[triangle.rangeIndex], volumeShadowed,
                              triangle.firstVertex, 3);
                }
                glDepthFunc(GL_LESS);
                return;
            }
            for (const DrawRange &range : ranges_) {
                if (range.count > 0) {
                    drawRange(range, volumeShadowed, range.first,
                              range.count);
                }
            }
        };
        if (shadowVolumesAvailable) {
            GLint previousStencilFunc = GL_ALWAYS;
            GLint previousStencilRef = 0;
            GLint previousStencilValueMask = 0xFFFFFFFFu;
            GLint previousStencilWriteMask = 0xFFFFFFFFu;
            GLint previousStencilFail = GL_KEEP;
            GLint previousStencilDepthFail = GL_KEEP;
            GLint previousStencilDepthPass = GL_KEEP;
            GLint previousStencilBackFunc = GL_ALWAYS;
            GLint previousStencilBackRef = 0;
            GLint previousStencilBackValueMask = 0xFFFFFFFFu;
            GLint previousStencilBackWriteMask = 0xFFFFFFFFu;
            GLint previousStencilBackFail = GL_KEEP;
            GLint previousStencilBackDepthFail = GL_KEEP;
            GLint previousStencilBackDepthPass = GL_KEEP;
            const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
            glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunc);
            glGetIntegerv(GL_STENCIL_REF, &previousStencilRef);
            glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
            glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
            glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL,
                          &previousStencilDepthFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS,
                          &previousStencilDepthPass);
            glGetIntegerv(GL_STENCIL_BACK_FUNC, &previousStencilBackFunc);
            glGetIntegerv(GL_STENCIL_BACK_REF, &previousStencilBackRef);
            glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK,
                          &previousStencilBackValueMask);
            glGetIntegerv(GL_STENCIL_BACK_WRITEMASK,
                          &previousStencilBackWriteMask);
            glGetIntegerv(GL_STENCIL_BACK_FAIL, &previousStencilBackFail);
            glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                          &previousStencilBackDepthFail);
            glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS,
                          &previousStencilBackDepthPass);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0u);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilFunc(GL_EQUAL, 0, 0xFFu);
            drawSurfaceRanges(false);
            glStencilFunc(GL_NOTEQUAL, 0, 0xFFu);
            drawSurfaceRanges(true);

            glStencilFuncSeparate(GL_FRONT,
                                  static_cast<GLenum>(previousStencilFunc),
                                  previousStencilRef,
                                  static_cast<GLuint>(previousStencilValueMask));
            glStencilMaskSeparate(GL_FRONT,
                                  static_cast<GLuint>(previousStencilWriteMask));
            glStencilOpSeparate(GL_FRONT,
                                static_cast<GLenum>(previousStencilFail),
                                static_cast<GLenum>(previousStencilDepthFail),
                                static_cast<GLenum>(previousStencilDepthPass));
            glStencilFuncSeparate(
                GL_BACK, static_cast<GLenum>(previousStencilBackFunc),
                previousStencilBackRef,
                static_cast<GLuint>(previousStencilBackValueMask));
            glStencilMaskSeparate(
                GL_BACK, static_cast<GLuint>(previousStencilBackWriteMask));
            glStencilOpSeparate(
                GL_BACK, static_cast<GLenum>(previousStencilBackFail),
                static_cast<GLenum>(previousStencilBackDepthFail),
                static_cast<GLenum>(previousStencilBackDepthPass));
            if (previousStencilTest) glEnable(GL_STENCIL_TEST);
            else glDisable(GL_STENCIL_TEST);
        } else {
            drawSurfaceRanges(false);
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
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityNormalTextureBinding));
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityObjectIdTextureBinding));
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityDepthTextureBinding));
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousImageTextureBinding));
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityJitterTextureBinding));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    }

    if (settings.mode == ViewportShadingMode::Solid &&
        settings.outline && !previewOverlay && !selectionOverlay &&
        !settings.xrayEnabled() && !vertices_.isEmpty()) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        const QSize pixelSize(qRound(viewportSize.width() * dpr),
                              qRound(viewportSize.height() * dpr));
        const bool useSharedOutlineIds = sharedPrepassReady &&
                                         outlineRequested;
        const bool outlineFramebufferReady = useSharedOutlineIds ||
            ensureObjectIdFramebuffer(pixelSize);
        if (outlineFramebufferReady) {
            GLint previousFramebuffer = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            GLint previousActiveTexture = GL_TEXTURE0;
            GLint previousTextureBinding = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);

            if (!useSharedOutlineIds) {
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
                    "uViewProjection",
                    viewportViewProjection(transform, viewportSize));
                vertexArray_.bind();
                vertexBuffer_.bind();
                for (int rangeIndex = 0; rangeIndex < ranges_.size();
                     ++rangeIndex) {
                    const DrawRange &range = ranges_[rangeIndex];
                    if (range.count <= 0) {
                        continue;
                    }
                    GLuint objectId = range.effectObjectId;
                    if (range.outlineHandledBySceneStroke) {
                        // Keep depth from suppressing outlines behind this
                        // surface, while leaving its own perimeter to the
                        // shared scene-stroke renderer.
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
            }
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding);
            glBindTexture(GL_TEXTURE_2D,
                          useSharedOutlineIds
                              ? workbenchOutlineIdTexture_
                              : objectIdTexture_);

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
