/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2018-2022 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_aa_renderer.h"

#include "blender_grid_renderer.h"
#include "core/serialization/shape_json_codec.h"
#include "workbench_lighting.h"
#include "viewport_cavity_blue_noise.h"

#include <QDataStream>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QOpenGLContext>
#include <QtEndian>
#include <QVector2D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace classiCAD {
namespace {

struct JitterPoint {
    float x = 0.0f;
    float y = 0.0f;
};

class BlenderSeededRandom final {
public:
    explicit BlenderSeededRandom(std::uint32_t seed)
        : state_((std::uint64_t(seed) << 16) | 0x330Eu)
    {
    }

    double getDouble()
    {
        state_ = (0x5DEECE66Dull * state_ + 0xBull) & 0x0000FFFFFFFFFFFFull;
        return double(static_cast<std::uint32_t>(state_ >> 17)) / 0x80000000;
    }

private:
    std::uint64_t state_ = 0;
};

// Adapted from Blender 5.2.2 BLI_jitter_2d.cc and workbench_effect_antialiasing.cc.
// The distribution and its ordering are kept so classiCAD advances through the
// same centered Workbench sample locations.
void jitterateOne(std::vector<JitterPoint> &points,
                  std::vector<JitterPoint> &scratch,
                  float radius)
{
    const int count = static_cast<int>(points.size());
    for (int i = count - 1; i >= 0; --i) {
        float displacementX = 0.0f;
        float displacementY = 0.0f;
        float x = points[i].x;
        float y = points[i].y;
        for (int j = count - 1; j >= 0; --j) {
            if (i == j) {
                continue;
            }
            float vectorX = points[j].x - x - 1.0f;
            float vectorY = points[j].y - y - 1.0f;
            for (int k = 3; k > 0; --k) {
                for (int neighbor = 0; neighbor < 3; ++neighbor) {
                    if (std::fabs(vectorX) < radius &&
                        std::fabs(vectorY) < radius) {
                        const float length =
                            std::sqrt(vectorX * vectorX + vectorY * vectorY);
                        if (length > 0.0f && length < radius) {
                            const float scaledLength = length / radius;
                            displacementX += vectorX / scaledLength;
                            displacementY += vectorY / scaledLength;
                        }
                    }
                    vectorX += neighbor == 2 ? -2.0f : 1.0f;
                }
                vectorY += 1.0f;
            }
        }
        x -= displacementX / 18.0f;
        y -= displacementY / 18.0f;
        x -= std::floor(x);
        y -= std::floor(y);
        scratch[i] = {x, y};
    }
    points = scratch;
}

void jitterateTwo(std::vector<JitterPoint> &points,
                  std::vector<JitterPoint> &scratch,
                  float radius)
{
    const int count = static_cast<int>(points.size());
    for (int i = count - 1; i >= 0; --i) {
        float displacementX = 0.0f;
        float displacementY = 0.0f;
        const float x = points[i].x;
        const float y = points[i].y;
        for (int j = count - 1; j >= 0; --j) {
            if (i == j) {
                continue;
            }
            float vectorX = points[j].x - x - 1.0f;
            float vectorY = points[j].y - y - 1.0f;
            for (int neighbor = 0; neighbor < 3; ++neighbor) {
                if (std::fabs(vectorX) < radius) {
                    displacementX += vectorX * radius;
                }
                vectorX += 1.0f;
            }
            for (int neighbor = 0; neighbor < 3; ++neighbor) {
                if (std::fabs(vectorY) < radius) {
                    displacementY += vectorY * radius;
                }
                vectorY += 1.0f;
            }
        }
        float movedX = x - displacementX / 2.0f;
        float movedY = y - displacementY / 2.0f;
        movedX -= std::floor(movedX);
        movedY -= std::floor(movedY);
        scratch[i] = {movedX, movedY};
    }
    points = scratch;
}

std::vector<JitterPoint> workbenchJitterPoints(int count)
{
    std::vector<JitterPoint> points(static_cast<std::size_t>(count));
    std::vector<JitterPoint> scratch(static_cast<std::size_t>(count));
    if (count <= 0) {
        return points;
    }

    const float countFloat = static_cast<float>(count);
    const float countSqrt = std::sqrt(countFloat);
    const float radiusOne = 1.0f / countSqrt;
    const float radiusTwo = 1.0f / countFloat;
    const float radiusThree = countSqrt / countFloat;
    BlenderSeededRandom random(31415926u + static_cast<std::uint32_t>(count));
    float x = 0.0f;
    for (int i = 0; i < count; ++i) {
        points[static_cast<std::size_t>(i)] = {
            x + radiusOne * static_cast<float>(0.5 - random.getDouble()),
            static_cast<float>(i) / countFloat +
                radiusOne * static_cast<float>(0.5 - random.getDouble())};
        x += radiusThree;
        x -= std::floor(x);
    }
    for (int iteration = 0; iteration < 24; ++iteration) {
        jitterateOne(points, scratch, radiusOne);
        jitterateOne(points, scratch, radiusOne);
        jitterateTwo(points, scratch, radiusTwo);
    }
    for (JitterPoint &point : points) {
        point.x -= 0.5f;
        point.y -= 0.5f;
    }

    int closestIndex = 0;
    float closestDistanceSquared = 1.0f;
    for (int i = 0; i < count; ++i) {
        const JitterPoint &point = points[static_cast<std::size_t>(i)];
        const float distanceSquared = point.x * point.x + point.y * point.y;
        if (distanceSquared < closestDistanceSquared) {
            closestDistanceSquared = distanceSquared;
            closestIndex = i;
        }
    }
    const JitterPoint closest = points[static_cast<std::size_t>(closestIndex)];
    for (JitterPoint &point : points) {
        point.x -= closest.x;
        point.y -= closest.y;
        point.x = std::fmod(point.x + 0.5f, 1.0f);
        point.y = std::fmod(point.y + 0.5f, 1.0f);
        point.x = point.x * 2.0f - 1.0f;
        point.y = point.y * 2.0f - 1.0f;
    }
    if (closestIndex != 0) {
        std::swap(points[0], points[static_cast<std::size_t>(closestIndex)]);
    }
    for (int i = 0; i < count - 2; ++i) {
        float farthestDistanceSquared = 0.0f;
        int farthestIndex = i;
        for (int j = i + 1; j < count; ++j) {
            const JitterPoint &a = points[static_cast<std::size_t>(i)];
            const JitterPoint &b = points[static_cast<std::size_t>(j)];
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > farthestDistanceSquared) {
                farthestDistanceSquared = distanceSquared;
                farthestIndex = j;
            }
        }
        std::swap(points[static_cast<std::size_t>(i + 1)],
                  points[static_cast<std::size_t>(farthestIndex)]);
    }
    return points;
}

const std::vector<JitterPoint> &samplePositions(int count)
{
    static const std::vector<JitterPoint> samples5 = workbenchJitterPoints(5);
    static const std::vector<JitterPoint> samples8 = workbenchJitterPoints(8);
    static const std::vector<JitterPoint> samples11 = workbenchJitterPoints(11);
    static const std::vector<JitterPoint> samples16 = workbenchJitterPoints(16);
    static const std::vector<JitterPoint> samples32 = workbenchJitterPoints(32);
    switch (count) {
    case 5: return samples5;
    case 8: return samples8;
    case 11: return samples11;
    case 16: return samples16;
    default: return samples32;
    }
}

float blackmanHarris(float distance, float width)
{
    if (distance > width * 0.5f) {
        return 0.0f;
    }
    const float angle = 2.0f * 3.14159265358979323846f *
                        std::clamp(distance / width + 0.5f, 0.0f, 1.0f);
    return 0.35875f - 0.48829f * std::cos(angle) +
           0.14128f * std::cos(2.0f * angle) -
           0.01168f * std::cos(3.0f * angle);
}

std::array<float, 9> workbenchSampleWeights(const JitterPoint &offset,
                                           float *sum)
{
    std::array<float, 9> weights{};
    *sum = 0.0f;
    int index = 0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            const float dx = static_cast<float>(x) - offset.x;
            const float dy = static_cast<float>(y) - offset.y;
            const float weight = blackmanHarris(std::sqrt(dx * dx + dy * dy), 2.0f);
            weights[static_cast<std::size_t>(index++)] = weight;
            *sum += weight;
        }
    }
    return weights;
}

const char *fullscreenVertexShader = R"glsl(
    #version 330 core
    out vec2 vUv;
    void main()
    {
        vec2 positions[3] = vec2[3](vec2(-1.0, -1.0),
                                    vec2(3.0, -1.0),
                                    vec2(-1.0, 3.0));
        vec2 position = positions[gl_VertexID];
        gl_Position = vec4(position, 0.0, 1.0);
        vUv = position * 0.5 + 0.5;
    }
)glsl";

QByteArray smaaShaderPreamble(int stage)
{
    QByteArray source = QByteArrayLiteral(
        "#version 330 core\n"
        "#define SMAA_GLSL_3\n"
        "#define SMAA_PRESET_HIGH\n"
        "#define SMAA_NO_DISCARD\n"
        "#define SMAA_RT_METRICS viewport_metrics\n"
        "#define SMAA_LUMA_WEIGHT vec4(1.0f, 1.0f, 1.0f, 1.0f)\n"
        "uniform vec4 viewport_metrics;\n"
        "#define SMAA_STAGE ");
    source += QByteArray::number(stage);
    source += '\n';
    return source;
}

bool readSmaaResource(const QString &path, QByteArray *contents)
{
    if (contents == nullptr) {
        return false;
    }
    QFile resource(path);
    if (!resource.open(QIODevice::ReadOnly)) {
        return false;
    }
    *contents = resource.readAll();
    return !contents->isEmpty();
}

} // namespace

ViewportAaRenderer::~ViewportAaRenderer()
{
    if (QOpenGLContext::currentContext() == nullptr) {
        return;
    }
    destroyTargets();
    if (depthOfFieldNoiseTexture_ != 0) {
        glDeleteTextures(1, &depthOfFieldNoiseTexture_);
        depthOfFieldNoiseTexture_ = 0;
    }
    if (vertexArray_.isCreated()) {
        vertexArray_.destroy();
    }
    accumulateProgram_.removeAllShaders();
    presentProgram_.removeAllShaders();
    depthOfFieldProgram_.removeAllShaders();
    overlayDepthProgram_.removeAllShaders();
}

void ViewportAaRenderer::setMode(int mode)
{
    const int normalized = normalizedViewportAntiAliasingMode(mode);
    if (mode_ == normalized) {
        return;
    }
    mode_ = normalized;
    historyKey_.clear();
}

int ViewportAaRenderer::mode() const noexcept
{
    return mode_;
}

void ViewportAaRenderer::setDepthOfField(
    bool enabled,
    const ViewportCameraPreferences &optics,
    bool perspectiveView,
    qreal projectionClipStart,
    qreal projectionClipEnd)
{
    depthOfFieldOptics_ = optics;
    // Depth-buffer values are encoded with the projection clip range. Use
    // that same range when reconstructing view depth for CoC, even when a
    // free perspective viewport has tightened its far plane for precision.
    depthOfFieldOptics_.clipStart = projectionClipStart;
    depthOfFieldOptics_.clipEnd = projectionClipEnd;
    depthOfFieldEnabled_ = enabled && optics.sceneCameraEnabled;
    depthOfFieldPerspective_ = perspectiveView;
}

void ViewportAaRenderer::setDisplayTransform(
    const ViewportShadingSettings &settings,
    bool sourceIsSceneLinear,
    bool sourceHasPremultipliedAlpha)
{
    displaySettings_ = settings;
    sourceIsSceneLinear_ = sourceIsSceneLinear;
    sourceHasPremultipliedAlpha_ = sourceHasPremultipliedAlpha;
    if (sceneSampleActive_) {
        targets_.sceneLinearColorTexture = sourceIsSceneLinear
            ? sceneColorTexture_ : 0;
        targets_.historyColorIsSceneLinear = sourceIsSceneLinear;
    }
}

bool ViewportAaRenderer::ensureDisplayTransformTexture()
{
    if (displaySettings_.colorTransform == 1) {
        displayTransformTextureAvailable_ = false;
        return false;
    }
    const ImportedBlenderColorLutMetadata &metadata =
        displaySettings_.colorLutMetadata;
    const QString lutCacheKey = displaySettings_.colorLutId + QLatin1Char('|') +
        QString::number(metadata.size) + QLatin1Char('|') +
        QString::number(metadata.domainMin, 'g', 17) + QLatin1Char('|') +
        QString::number(metadata.domainMax, 'g', 17) + QLatin1Char('|') +
        metadata.shaper + QLatin1Char('|') +
        QString::number(metadata.shaperScale, 'g', 17);
    if (displayTransformTexture_ != 0 &&
        displayTransformTextureMode_ == displaySettings_.colorTransform &&
        displayTransformTextureLutId_ == lutCacheKey) {
        return displayTransformTextureAvailable_;
    }
    const int edge = displaySettings_.colorTransform == 4
        ? metadata.size : workbenchDisplayLutSize(displaySettings_.colorTransform);
    const qsizetype expectedBytes = qsizetype(edge) * edge * edge * 12;
    const QVector<float> *builtin = nullptr;
    if (displaySettings_.colorTransform == 2) {
        builtin = &workbenchPbrNeutralDisplayLutData();
    } else if (displaySettings_.colorTransform == 3) {
        builtin = &workbenchFilmicDisplayLutData();
    } else if (displaySettings_.colorTransform != 4) {
        builtin = &workbenchAgxDisplayLutData();
    }
    if (edge < 2 || edge > 140 ||
        (builtin != nullptr && builtin->size() != edge * edge * edge * 3) ||
        (displaySettings_.colorTransform == 4 &&
         (displaySettings_.colorLutData.size() != expectedBytes ||
          !std::isfinite(metadata.domainMin) ||
          !std::isfinite(metadata.domainMax) ||
          metadata.domainMax <= metadata.domainMin ||
          !std::isfinite(metadata.shaperScale) || metadata.shaperScale <= 0.0 ||
          (metadata.shaper != QStringLiteral("linear") &&
           metadata.shaper != QStringLiteral("log2_1p"))))) {
        displayTransformTextureAvailable_ = false;
        return false;
    }
    QVector<float> customValues;
    const float *values = nullptr;
    if (builtin != nullptr) {
        values = builtin->constData();
    } else {
        customValues.resize(edge * edge * edge * 3);
        const auto *bytes = reinterpret_cast<const uchar *>(
            displaySettings_.colorLutData.constData());
        for (qsizetype index = 0; index < customValues.size(); ++index) {
            const quint32 bits = qFromLittleEndian<quint32>(
                bytes + index * qsizetype(sizeof(quint32)));
            std::memcpy(&customValues[index], &bits, sizeof(bits));
            if (!std::isfinite(customValues[index])) {
                displayTransformTextureAvailable_ = false;
                return false;
            }
        }
        values = customValues.constData();
    }
    if (displayTransformTexture_ == 0) {
        glGenTextures(1, &displayTransformTexture_);
    }
    if (displayTransformTexture_ == 0) {
        displayTransformTextureAvailable_ = false;
        return false;
    }
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &previousTexture);
    while (glGetError() != GL_NO_ERROR) {
    }
    glBindTexture(GL_TEXTURE_3D, displayTransformTexture_);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F, edge, edge, edge, 0,
                 GL_RGB, GL_FLOAT, values);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    displayTransformTextureAvailable_ = glGetError() == GL_NO_ERROR;
    displayTransformTextureMode_ = displaySettings_.colorTransform;
    displayTransformTextureLutId_ = lutCacheKey;
    return displayTransformTextureAvailable_;
}

bool ViewportAaRenderer::initialize()
{
    if (initialized_) {
        return true;
    }
    if (initializationAttempted_ || QOpenGLContext::currentContext() == nullptr ||
        !initializeOpenGLFunctions()) {
        return false;
    }
    initializationAttempted_ = true;
    // Follows Blender 5.2.2 workbench_fx_taa.bsl.hh: a 3x3, log-space color
    // filter whose sample offset is expressed by the CPU-generated weights.
    static constexpr const char *accumulateFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uSampleColor;
        uniform vec2 uInvSize;
        uniform float uWeights[9];
        layout(location = 0) out vec4 fragmentColor;
        void main()
        {
            vec4 result = vec4(0.0);
            int index = 0;
            for (int x = -1; x <= 1; ++x) {
                for (int y = -1; y <= 1; ++y) {
                    vec4 sampleColor = texture(
                        uSampleColor, vUv + vec2(x, y) * uInvSize);
                    sampleColor = clamp(sampleColor, vec4(0.0), vec4(1.0e10));
                    sampleColor.rgb = log2(sampleColor.rgb + 1.0);
                    result += sampleColor * uWeights[index++];
                }
            }
            fragmentColor = result;
        }
    )glsl";
    // Blender applies this overlay-only depth override after Workbench's
    // opaque, transparent, volume, outline, and DOF passes. Keep the physical
    // in-front depth through those scene passes, then place front-owned pixels
    // at the near plane so later editor overlays stay behind them.
    static constexpr const char *overlayDepthFragmentShader = R"glsl(
        #version 330 core
        uniform sampler2D uFrontDepth;
        void main()
        {
            ivec2 pixel = ivec2(gl_FragCoord.xy);
            if (texelFetch(uFrontDepth, pixel, 0).r >= 1.0) discard;
            gl_FragDepth = 0.0;
        }
    )glsl";
    static constexpr const char *presentFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uSource;
        uniform sampler2D uDepth;
        uniform bool uUseDepthOfField;
        uniform bool uPerspective;
        uniform vec2 uNearFar;
        uniform float uFocusDistance;
        uniform float uFocalLengthMillimeters;
        uniform float uOpticalSensorSizeMillimeters;
        uniform float uFStop;
        uniform vec2 uPixelSize;
        uniform bool uEncodeDisplay;
        uniform bool uApplyViewTransform;
        uniform bool uPremultipliedAlpha;
        uniform sampler3D uDisplayLut;
        uniform bool uDisplayLutAvailable;
        uniform int uColorTransform;
        uniform int uDisplayLutSize;
        uniform vec2 uDisplayLutDomain;
        uniform int uDisplayLutShaper;
        uniform float uDisplayLutShaperScale;
        uniform float uExposure;
        uniform float uGamma;
        uniform float uDitherIntensity;
        uniform vec2 uPixelOrigin;
        layout(location = 0) out vec4 fragmentColor;
        float ditherRandomValue(ivec2 pixel)
        {
            uvec2 p = uvec2(pixel);
            uint qx = 1103515245u * ((p.x >> 1u) ^ p.y);
            uint qy = 1103515245u * ((p.y >> 1u) ^ p.x);
            uint n = 1103515245u * (qx ^ (qy >> 3u));
            float value = float(n) * (1.0 / float(0xffffffffu));
            value = value * 2.0 - 1.0;
            return sign(value) * (1.0 - sqrt(1.0 - abs(value)));
        }
        float cocRadius(vec2 uv)
        {
            float depth = texture(uDepth, uv).r;
            float z = uPerspective
                ? (uNearFar.x * uNearFar.y) /
                    max(uNearFar.y - depth * (uNearFar.y - uNearFar.x), 1e-6)
                : mix(uNearFar.x, uNearFar.y, depth);
            // Blender 5.2.2 converts camera lens/sensor millimeters to scene
            // units; view depth and focus distance remain in scene units.
            const float millimetersToSceneUnits = 0.001;
            float f = uFocalLengthMillimeters * millimetersToSceneUnits;
            float sensorSize = uOpticalSensorSizeMillimeters * millimetersToSceneUnits;
            float focus = uFocusDistance;
            float coc = abs((f * f / max(uFStop * abs(focus - f), 1e-6)) *
                            ((z - focus) / max(z, 1e-6)));
            return clamp(coc * (uPixelSize.x / max(sensorSize, 1e-6)),
                         0.0, 12.0);
        }
        vec3 median3(vec3 a, vec3 b, vec3 c)
        {
            return max(min(a, b), min(max(a, b), c));
        }
        vec4 dofGather(vec2 uv)
        {
            // CoC-aware 2x2 downsample: retain the largest blur footprint
            // while averaging the scene-linear source colors.
            vec2 halfPixel = 0.5 * uPixelSize;
            vec2 offsets[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0),
                                      vec2(-1.0, 1.0), vec2(1.0, 1.0));
            vec3 downsampled = vec3(0.0);
            float radius = 0.0;
            for (int j = 0; j < 4; ++j) {
                vec2 tapUv = clamp(uv + offsets[j] * halfPixel,
                                   halfPixel, vec2(1.0) - halfPixel);
                downsampled += texture(uSource, tapUv).rgb * 0.25;
                radius = max(radius, cocRadius(tapUv));
            }
            if (radius < 0.75) return texture(uSource, uv);
            const vec2 disk[12] = vec2[12](
                vec2(0.0, 0.0), vec2(0.527, 0.085), vec2(-0.040, 0.536),
                vec2(-0.670, -0.179), vec2(0.244, -0.647), vec2(0.765, 0.429),
                vec2(-0.315, 0.847), vec2(-0.897, 0.310), vec2(0.493, 0.749),
                vec2(0.901, -0.312), vec2(-0.527, -0.757), vec2(0.089, -0.956));
            vec4 sum = vec4(0.0);
            float weight = 0.0;
            for (int i = 0; i < 12; ++i) {
                vec2 sampleUv = clamp(uv + disk[i] * radius * uPixelSize,
                                      uPixelSize * 0.5, vec2(1.0) - uPixelSize * 0.5);
                float centerDepth = texture(uDepth, uv).r;
                float sampleDepth = texture(uDepth, sampleUv).r;
                if (sampleDepth < centerDepth - 0.002) continue;
                sum += texture(uSource, sampleUv);
                weight += 1.0;
            }
            vec3 gathered = weight > 0.0 ? (sum / weight).rgb : downsampled;
            // Three-tap median cleanup suppresses isolated gather outliers.
            vec3 medianColor = median3(
                gathered,
                texture(uSource, clamp(uv + vec2(uPixelSize.x, 0.0),
                                       halfPixel, vec2(1.0) - halfPixel)).rgb,
                texture(uSource, clamp(uv - vec2(uPixelSize.x, 0.0),
                                       halfPixel, vec2(1.0) - halfPixel)).rgb);
            vec4 original = texture(uSource, uv);
            float cocMix = clamp(radius / 6.0, 0.0, 1.0);
            return vec4(mix(original.rgb, medianColor, cocMix), original.a);
        }
        vec3 linearToSrgb(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color * 12.92;
            vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
            vec3 encoded = mix(high, low, lessThanEqual(color, vec3(0.0031308)));
            return mix(encoded, vec3(1.0), greaterThanEqual(color, vec3(1.0)));
        }
        vec3 transformColor(vec3 color)
        {
            color *= exp2(uExposure);
            if (uColorTransform == 1 ||
                (!uDisplayLutAvailable && uColorTransform != 4)) {
                color = linearToSrgb(color);
            } else if (uDisplayLutAvailable) {
                vec3 bounded = clamp(color, vec3(uDisplayLutDomain.x),
                                     vec3(uDisplayLutDomain.y)) -
                               vec3(uDisplayLutDomain.x);
                float span = max(uDisplayLutDomain.y - uDisplayLutDomain.x,
                                 0.000001);
                vec3 normalized = bounded / span;
                if (uDisplayLutShaper == 1) {
                    float denominator = log2(1.0 +
                        uDisplayLutShaperScale * span);
                    normalized = log2(vec3(1.0) +
                        uDisplayLutShaperScale * bounded) /
                        max(denominator, 0.000001);
                }
                vec3 coordinate =
                    (clamp(normalized, vec3(0.0), vec3(1.0)) *
                     float(uDisplayLutSize - 1) + 0.5) /
                    float(uDisplayLutSize);
                color = texture(uDisplayLut, coordinate).rgb;
            }
            color = max(color, vec3(0.0));
            if (uGamma <= 1.1920928955078125e-7) {
                return pow(min(color, vec3(1.0)),
                           vec3(1.0 / 1.1920928955078125e-7));
            }
            return pow(color,
                       vec3(1.0 / max(uGamma, 1.1920928955078125e-7)));
        }
        void main()
        {
            vec4 source = uUseDepthOfField ? dofGather(vUv) : texture(uSource, vUv);
            vec3 outputColor = source.rgb;
            bool predivide = uPremultipliedAlpha && uApplyViewTransform;
            if (predivide) {
                outputColor = source.a > 0.0
                    ? outputColor / source.a : vec3(0.0);
            }
            if (uApplyViewTransform) {
                outputColor = transformColor(outputColor);
            } else if (uEncodeDisplay) {
                outputColor = linearToSrgb(outputColor);
            }
            // Blender 5.2.2 applies render dither after the display transform
            // when converting RGBA float pixels to display bytes. gl_FragCoord
            // and Blender's render pixel coordinates both use a bottom-left
            // origin, so subtract only the destination viewport origin here.
            if (uDitherIntensity > 0.0) {
                ivec2 pixel = ivec2(gl_FragCoord.xy - uPixelOrigin);
                outputColor += vec3(ditherRandomValue(pixel) *
                                    0.0033 * uDitherIntensity);
            }
            if (predivide) {
                outputColor *= source.a;
            }
            fragmentColor = vec4(outputColor, source.a);
        }
    )glsl";
    // Workbench 5.2.2's DOF is a half-resolution, multi-pass gather. This
    // stage-dispatched shader keeps the passes explicit while sharing their
    // optical CoC calculation and texture bindings.
    static constexpr const char *depthOfFieldFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uSource;
        uniform sampler2D uCoc;
        uniform sampler2D uDepth;
        uniform sampler2D uBlur;
        uniform sampler2D uNoise;
        uniform int uStage;
        uniform int uSourceLod;
        uniform bool uPerspective;
        uniform vec2 uNearFar;
        uniform float uFocusDistance;
        uniform float uFocalLengthMillimeters;
        uniform float uOpticalSensorSizeMillimeters;
        uniform float uFStop;
        uniform int uApertureBlades;
        uniform float uApertureRotation;
        uniform float uApertureRatio;
        uniform float uJitterOffset;
        layout(location = 0) out vec4 outColor;
        layout(location = 1) out vec4 outCoc;
        float cocAtDepth(float depth)
        {
            float z = uPerspective
                ? (uNearFar.x * uNearFar.y) /
                    max(uNearFar.y - depth * (uNearFar.y - uNearFar.x), 1e-6)
                : mix(uNearFar.x, uNearFar.y, depth);
            // Camera lens and sensor values are stored in millimeters, while
            // depth and focus distance use unscaled Blender scene coordinates.
            const float millimetersToSceneUnits = 0.001;
            float f = uFocalLengthMillimeters * millimetersToSceneUnits;
            float sensorSize = uOpticalSensorSizeMillimeters * millimetersToSceneUnits;
            float focus = uFocusDistance;
            return (0.5 * f * f / max(uFStop * abs(focus - f), 1e-6)) *
                   ((focus - z) / max(z, 1e-6)) *
                   (float(textureSize(uDepth, 0).x) / max(sensorSize, 1e-6));
        }
        vec2 encodeCoc(float coc)
        {
            return vec2(max(coc, 0.0), max(-coc, 0.0)) / 100.0;
        }
        vec4 fetchColor(ivec2 p, int lod)
        {
            return texelFetch(uSource, p, lod);
        }
        vec2 fetchCoc(ivec2 p, int lod)
        {
            return texelFetch(uCoc, p, lod).rg;
        }
        vec2 shapeAperture(vec2 disk)
        {
            // Blender Workbench enables polygon shaping for every blade
            // count above one, including the two-blade slit aperture.
            if (uApertureBlades > 1) {
                float bladeAngle = 6.28318530718 / float(uApertureBlades);
                float angle = atan(disk.y, disk.x);
                float sector = mod(angle + 0.5 * bladeAngle, bladeAngle) -
                               0.5 * bladeAngle;
                float polygonRadius = cos(0.5 * bladeAngle) / cos(sector);
                disk *= polygonRadius;
            }
            // Blender stores the aperture ratio as an X scale reciprocal:
            // sample.x = r * cos(T) * (1.0 / aperture_ratio).
            float angle = atan(disk.y, disk.x) + uApertureRotation;
            float radius = length(disk);
            return vec2(cos(angle) * radius / max(uApertureRatio, 0.01),
                        sin(angle) * radius);
        }
        // Blender Workbench's square_to_circle mapping, applied to the
        // concentric square rings used by DofPass::setup_samples.
        vec2 squareToCircle(vec2 p)
        {
            float r;
            float angle;
            if (p.x > -p.y) {
                if (p.x > p.y) { r = p.x; angle = 0.78539816339 * (p.y / p.x); }
                else { r = p.y; angle = 0.78539816339 * (2.0 - p.x / p.y); }
            } else {
                if (p.x < p.y) { r = -p.x; angle = 0.78539816339 * (4.0 + p.y / p.x); }
                else { r = -p.y; angle = p.y != 0.0 ? 0.78539816339 * (6.0 - p.x / p.y) : 0.0; }
            }
            return r * vec2(cos(angle), sin(angle));
        }
        vec4 weightedFour(vec4 a, vec4 b, vec4 c, vec4 d,
                          vec4 weights)
        {
            float total = dot(weights, vec4(1.0));
            return total > 1e-6 ?
                (a * weights.x + b * weights.y + c * weights.z + d * weights.w) / total :
                (a + b + c + d) * 0.25;
        }
        void main()
        {
            ivec2 p = ivec2(gl_FragCoord.xy);
            if (uStage == 0) {
                ivec2 sourceSize = textureSize(uSource, 0);
                ivec2 depthSize = textureSize(uDepth, 0);
                ivec2 b = p * 2;
                ivec2 p00 = min(b, sourceSize - 1);
                ivec2 p10 = min(b + ivec2(1, 0), sourceSize - 1);
                ivec2 p01 = min(b + ivec2(0, 1), sourceSize - 1);
                ivec2 p11 = min(b + ivec2(1, 1), sourceSize - 1);
                vec4 colors[4] = vec4[4](
                    fetchColor(p00, 0), fetchColor(p10, 0),
                    fetchColor(p01, 0), fetchColor(p11, 0));
                float c[4];
                c[0] = cocAtDepth(texelFetch(uDepth, min(b, depthSize - 1), 0).r);
                c[1] = cocAtDepth(texelFetch(uDepth, min(b + ivec2(1, 0), depthSize - 1), 0).r);
                c[2] = cocAtDepth(texelFetch(uDepth, min(b + ivec2(0, 1), depthSize - 1), 0).r);
                c[3] = cocAtDepth(texelFetch(uDepth, min(b + ivec2(1, 1), depthSize - 1), 0).r);
                float nearCoc = max(max(max(c[0], c[1]), max(c[2], c[3])), 0.0);
                float farCoc = max(max(max(-c[0], -c[1]), max(-c[2], -c[3])), 0.0);
                vec4 nearW = vec4(0.0);
                vec4 farW = vec4(0.0);
                for (int i = 0; i < 4; ++i) {
                    nearW[i] = c[i] >= 0.0 ? clamp(1.0 - abs(nearCoc - c[i]), 0.0, 1.0) : 0.0;
                    farW[i] = c[i] <= 0.0 ? clamp(1.0 - abs(farCoc + c[i]), 0.0, 1.0) : 0.0;
                }
                vec4 weights = dot(farW, vec4(1.0)) > 0.0 ? farW : nearW;
                outColor = clamp(weightedFour(colors[0], colors[1], colors[2], colors[3], weights), 0.0, 3.0);
                outCoc = vec4(encodeCoc(nearCoc) + vec2(0.0, farCoc / 100.0),
                              0.0, 1.0);
                return;
            }
            if (uStage == 1) {
                ivec2 sourceSize = textureSize(uSource, uSourceLod);
                ivec2 b = p * 2;
                ivec2 p00 = min(b, sourceSize - 1);
                ivec2 p10 = min(b + ivec2(1, 0), sourceSize - 1);
                ivec2 p01 = min(b + ivec2(0, 1), sourceSize - 1);
                ivec2 p11 = min(b + ivec2(1, 1), sourceSize - 1);
                vec4 colors[4] = vec4[4](
                    fetchColor(p00, uSourceLod), fetchColor(p10, uSourceLod),
                    fetchColor(p01, uSourceLod), fetchColor(p11, uSourceLod));
                vec2 cocs[4] = vec2[4](
                    fetchCoc(p00, uSourceLod), fetchCoc(p10, uSourceLod),
                    fetchCoc(p01, uSourceLod), fetchCoc(p11, uSourceLod));
                float nearCoc = max(max(max(cocs[0].r, cocs[1].r), max(cocs[2].r, cocs[3].r)), 0.0);
                float farCoc = max(max(max(cocs[0].g, cocs[1].g), max(cocs[2].g, cocs[3].g)), 0.0);
                vec4 nearW = vec4(0.0);
                vec4 farW = vec4(0.0);
                for (int i = 0; i < 4; ++i) {
                    float n = cocs[i].r * 100.0;
                    float f = cocs[i].g * 100.0;
                    nearW[i] = n >= 0.0 ? clamp(1.0 - abs(nearCoc * 100.0 - n), 0.0, 1.0) : 0.0;
                    farW[i] = f >= 0.0 ? clamp(1.0 - abs(farCoc * 100.0 - f), 0.0, 1.0) : 0.0;
                }
                vec4 weights = dot(farW, vec4(1.0)) > 0.0 ? farW : nearW;
                outColor = weightedFour(colors[0], colors[1], colors[2], colors[3], weights);
                outCoc = vec4(nearCoc, farCoc, 0.0, 1.0);
                return;
            }
            if (uStage == 2) {
                float centerCoc = max(texture(uCoc, vUv).r,
                                      texture(uCoc, vUv).g) * 100.0;
                vec4 sum = vec4(0.0);
                float total = 0.0;
                ivec2 size = textureSize(uSource, 0);
                // Blender 5.2.2 get_random_vector() combines interleaved
                // gradient noise with channel 1 of the 64x64 EEVEE blue-noise
                // LUT. It jitters the gather origin; it does not rotate the
                // sample pattern.
                ivec2 noiseCoord = ivec2(gl_FragCoord.xy) % 64;
                float blueNoise = texelFetch(uNoise, noiseCoord, 0).r;
                float ign = fract(uJitterOffset + 52.9829189 * fract(
                    0.06711056 * gl_FragCoord.x +
                    0.00583715 * gl_FragCoord.y));
                float angle = 6.28318530718 * fract(blueNoise + uJitterOffset);
                vec2 randomVector = vec2(cos(angle), sin(angle)) * sqrt(ign);
                vec2 noiseOffset = randomVector * 0.2 *
                    clamp(centerCoc * 0.2 - 4.0, 0.0, 1.0);
                // Workbench's inverted_viewport_size is in full-resolution
                // pixels while this gather samples half-resolution textures.
                vec2 halfTexelScale = 0.5 / vec2(size);
                for (int ring = 0; ring <= 3; ++ring) {
                  for (int y = -ring; y <= ring; ++y) {
                    for (int x = -ring; x <= ring; ++x) {
                        if (max(abs(x), abs(y)) != ring) continue;
                        vec2 square = vec2(x, y) / 3.0;
                        vec2 circle = squareToCircle(square);
                        float radius = length(circle);
                        vec2 disk = shapeAperture(circle);
                        vec2 uv = clamp(vUv + (noiseOffset + disk) *
                                        centerCoc * halfTexelScale,
                                        0.5 / vec2(size), 1.0 - 0.5 / vec2(size));
                        vec2 sampleCoc = texture(uCoc, uv).rg;
                        float sampleRadius = max(sampleCoc.r, sampleCoc.g) * 100.0;
                        float weight = sampleRadius * smoothstep(radius * centerCoc - 0.5,
                                                                  radius * centerCoc + 0.5,
                                                                  sampleRadius);
                        if (weight > 0.0) {
                            // Match Workbench blur1's CoC-dependent mip
                            // selection. Texture levels below zero resolve
                            // to the base level, so clamp the logarithm there.
                            float lod = max(log2(clamp(
                                (sampleRadius + min(sampleRadius, centerCoc)) *
                                    0.5 - 21.0,
                                0.0, 16.0) * 0.25), 0.0);
                            sum += textureLod(uSource, uv, lod) * weight;
                            total += weight;
                        }
                    }
                  }
                }
                outColor = total > 0.0 ? sum / total : textureLod(uSource, vUv, 0.0);
                outCoc = vec4(0.0);
                return;
            }
            if (uStage == 3) {
                vec2 size = vec2(textureSize(uBlur, 0));
                vec2 uv = gl_FragCoord.xy / size;
                float coc = max(texture(uCoc, uv).r, texture(uCoc, uv).g) * 100.0;
                float radius = clamp(coc - 9.0, 0.0, 1.0);
                vec4 values[9];
                int n = 0;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                        values[n++] = texture(uBlur, uv + vec2(x, y) / size * radius);
                for (int i = 0; i < 9; ++i)
                    for (int j = i + 1; j < 9; ++j) {
                        vec4 lo = min(values[i], values[j]);
                        vec4 hi = max(values[i], values[j]);
                        values[i] = lo;
                        values[j] = hi;
                    }
                outColor = values[4];
                outCoc = vec4(0.0);
                return;
            }
            // Blender's resolve samples the half-resolution buffers at
            // gl_FragCoord * (0.5 / halfResolution), independently of the
            // full-resolution viewport size. This matters for odd dimensions
            // where the floor-sized half target does not span the last pixel.
            vec2 halfUv = gl_FragCoord.xy *
                          (0.5 / vec2(textureSize(uBlur, 0)));
            float depth = texelFetch(uDepth, p, 0).r;
            float coc = abs(cocAtDepth(depth));
            float blend = smoothstep(1.0, 3.0, coc);
            outColor = mix(texture(uSource, vUv), texture(uBlur, halfUv), blend);
            outCoc = vec4(0.0);
        }
    )glsl";
    QByteArray smaaLibrary;
    QByteArray smaaVertexTemplate;
    QByteArray smaaFragmentTemplate;
    if (!readSmaaResource(QStringLiteral(":/classiCAD/shaders/viewport_smaa_lib.glsl"),
                          &smaaLibrary) ||
        !readSmaaResource(QStringLiteral(":/classiCAD/shaders/viewport_smaa_vertex.glsl"),
                          &smaaVertexTemplate) ||
        !readSmaaResource(QStringLiteral(":/classiCAD/shaders/viewport_smaa_fragment.glsl"),
                          &smaaFragmentTemplate)) {
        qWarning() << "Viewport SMAA shader resources are unavailable";
        return false;
    }
    bool smaaProgramsReady = true;
    for (int stage = 0; stage < int(smaaPrograms_.size()); ++stage) {
        QByteArray vertexSource = smaaShaderPreamble(stage);
        vertexSource += smaaLibrary;
        vertexSource += smaaVertexTemplate;
        QByteArray fragmentSource = smaaShaderPreamble(stage);
        fragmentSource += smaaLibrary;
        fragmentSource += smaaFragmentTemplate;
        QOpenGLShaderProgram &program =
            smaaPrograms_[static_cast<std::size_t>(stage)];
        if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                             vertexSource) ||
            !program.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                             fragmentSource) ||
            !program.link()) {
            qWarning().noquote() << "Viewport SMAA shader stage setup failed:"
                                 << stage << program.log();
            smaaProgramsReady = false;
            break;
        }
    }
    if (!accumulateProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                   fullscreenVertexShader) ||
        !accumulateProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                    accumulateFragmentShader) ||
        !accumulateProgram_.link() ||
        !presentProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                 fullscreenVertexShader) ||
        !presentProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                 presentFragmentShader) ||
        !presentProgram_.link() ||
        !depthOfFieldProgram_.addShaderFromSourceCode(
            QOpenGLShader::Vertex, fullscreenVertexShader) ||
        !depthOfFieldProgram_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, depthOfFieldFragmentShader) ||
        !depthOfFieldProgram_.link() || !smaaProgramsReady ||
        !overlayDepthProgram_.addShaderFromSourceCode(
            QOpenGLShader::Vertex, fullscreenVertexShader) ||
        !overlayDepthProgram_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, overlayDepthFragmentShader) ||
        !overlayDepthProgram_.link() ||
        !vertexArray_.create()) {
        qWarning().noquote() << "Viewport AA shader setup failed:"
                             << accumulateProgram_.log()
                             << presentProgram_.log();
        return false;
    }
    initialized_ = true;
    return true;
}

void ViewportAaRenderer::destroyTargets()
{
    if (depthOfFieldFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &depthOfFieldFramebuffer_);
        depthOfFieldFramebuffer_ = 0;
    }
    if (depthOfFieldColorTexture_ != 0) {
        glDeleteTextures(1, &depthOfFieldColorTexture_);
        depthOfFieldColorTexture_ = 0;
    }
    const GLuint dofFramebuffers[] = {depthOfFieldPrepareFramebuffer_,
                                      depthOfFieldBlurFramebuffer_,
                                      depthOfFieldMipFramebuffer_};
    glDeleteFramebuffers(3, dofFramebuffers);
    depthOfFieldPrepareFramebuffer_ = 0;
    depthOfFieldBlurFramebuffer_ = 0;
    depthOfFieldMipFramebuffer_ = 0;
    const GLuint dofTextures[] = {depthOfFieldSourceTexture_,
                                  depthOfFieldCocTexture_,
                                  depthOfFieldBlurTexture_,
                                  depthOfFieldMipColorTexture_,
                                  depthOfFieldMipCocTexture_};
    glDeleteTextures(5, dofTextures);
    depthOfFieldSourceTexture_ = 0;
    depthOfFieldCocTexture_ = 0;
    depthOfFieldBlurTexture_ = 0;
    depthOfFieldMipColorTexture_ = 0;
    depthOfFieldMipCocTexture_ = 0;
    if (resolveFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &resolveFramebuffer_);
        resolveFramebuffer_ = 0;
    }
    if (smaaEdgeFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &smaaEdgeFramebuffer_);
        smaaEdgeFramebuffer_ = 0;
    }
    if (smaaWeightFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &smaaWeightFramebuffer_);
        smaaWeightFramebuffer_ = 0;
    }
    if (historyFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &historyFramebuffer_);
        historyFramebuffer_ = 0;
    }
    if (sceneFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &sceneFramebuffer_);
        sceneFramebuffer_ = 0;
    }
    if (overlayDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &overlayDepthFramebuffer_);
        overlayDepthFramebuffer_ = 0;
    }
    if (attachmentClearFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &attachmentClearFramebuffer_);
        attachmentClearFramebuffer_ = 0;
    }
    if (sampleZeroDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &sampleZeroDepthFramebuffer_);
        sampleZeroDepthFramebuffer_ = 0;
    }
    if (sampleZeroFrontDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &sampleZeroFrontDepthFramebuffer_);
        sampleZeroFrontDepthFramebuffer_ = 0;
    }
    sampleZeroDepthSize_ = {};
    if (sceneColorTexture_ != 0) {
        glDeleteTextures(1, &sceneColorTexture_);
        sceneColorTexture_ = 0;
    }
    if (displayTransformTexture_ != 0) {
        glDeleteTextures(1, &displayTransformTexture_);
        displayTransformTexture_ = 0;
        displayTransformTextureMode_ = -1;
        displayTransformTextureLutId_.clear();
    }
    if (depthTexture_ != 0) {
        glDeleteTextures(1, &depthTexture_);
        depthTexture_ = 0;
    }
    if (sampleZeroDepthTexture_ != 0) {
        glDeleteTextures(1, &sampleZeroDepthTexture_);
        sampleZeroDepthTexture_ = 0;
    }
    if (sampleZeroFrontDepthTexture_ != 0) {
        glDeleteTextures(1, &sampleZeroFrontDepthTexture_);
        sampleZeroFrontDepthTexture_ = 0;
    }
    if (viewSpaceNormalTexture_ != 0) {
        glDeleteTextures(1, &viewSpaceNormalTexture_);
        viewSpaceNormalTexture_ = 0;
    }
    if (materialDataTexture_ != 0) {
        glDeleteTextures(1, &materialDataTexture_);
        materialDataTexture_ = 0;
    }
    if (shadowMaskTexture_ != 0) {
        glDeleteTextures(1, &shadowMaskTexture_);
        shadowMaskTexture_ = 0;
    }
    if (objectIdTexture_ != 0) {
        glDeleteTextures(1, &objectIdTexture_);
        objectIdTexture_ = 0;
    }
    if (frontFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &frontFramebuffer_);
        frontFramebuffer_ = 0;
    }
    const GLuint frontTextures[] = {frontColorTexture_, frontDepthTexture_,
                                    frontMaterialDataTexture_,
                                    frontViewSpaceNormalTexture_, frontObjectIdTexture_,
                                    frontOutlineIdTexture_,
                                    frontShadowMaskTexture_};
    glDeleteTextures(7, frontTextures);
    frontColorTexture_ = frontDepthTexture_ = 0;
    frontMaterialDataTexture_ = 0;
    frontShadowMaskTexture_ = 0;
    frontViewSpaceNormalTexture_ = frontObjectIdTexture_ = 0;
    frontOutlineIdTexture_ = 0;
    if (historyColorTexture_ != 0) {
        glDeleteTextures(1, &historyColorTexture_);
        historyColorTexture_ = 0;
    }
    if (resolvedColorTexture_ != 0) {
        glDeleteTextures(1, &resolvedColorTexture_);
        resolvedColorTexture_ = 0;
    }
    if (smaaEdgeTexture_ != 0) {
        glDeleteTextures(1, &smaaEdgeTexture_);
        smaaEdgeTexture_ = 0;
    }
    if (smaaWeightTexture_ != 0) {
        glDeleteTextures(1, &smaaWeightTexture_);
        smaaWeightTexture_ = 0;
    }
    if (smaaAreaTexture_ != 0) {
        glDeleteTextures(1, &smaaAreaTexture_);
        smaaAreaTexture_ = 0;
    }
    if (smaaSearchTexture_ != 0) {
        glDeleteTextures(1, &smaaSearchTexture_);
        smaaSearchTexture_ = 0;
    }
    targetSize_ = {};
    targets_ = {};
}

bool ViewportAaRenderer::ensureTargets(const QSize &pixelSize)
{
    if (pixelSize.isEmpty()) {
        return false;
    }
    if (targetSize_ == pixelSize && sceneFramebuffer_ != 0 &&
        historyFramebuffer_ != 0 && resolveFramebuffer_ != 0 &&
        smaaEdgeFramebuffer_ != 0 && smaaWeightFramebuffer_ != 0) {
        return true;
    }
    destroyTargets();

    GLint previousFramebuffer = 0;
    GLint previousTexture = 0;
    GLint previousRenderbuffer = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glGenTextures(1, &sceneColorTexture_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &frontColorTexture_);
    glBindTexture(GL_TEXTURE_2D, frontColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &frontOutlineIdTexture_);
    glBindTexture(GL_TEXTURE_2D, frontOutlineIdTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, pixelSize.width(),
                 pixelSize.height(), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &frontMaterialDataTexture_);
    glBindTexture(GL_TEXTURE_2D, frontMaterialDataTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &frontShadowMaskTexture_);
    glBindTexture(GL_TEXTURE_2D, frontShadowMaskTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RED, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &frontDepthTexture_);
    glBindTexture(GL_TEXTURE_2D, frontDepthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_DEPTH_STENCIL,
                 GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &frontViewSpaceNormalTexture_);
    glBindTexture(GL_TEXTURE_2D, frontViewSpaceNormalTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &frontObjectIdTexture_);
    glBindTexture(GL_TEXTURE_2D, frontObjectIdTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, pixelSize.width(),
                 pixelSize.height(), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &frontFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, frontFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, frontColorTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, frontMaterialDataTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                           GL_TEXTURE_2D, frontViewSpaceNormalTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3,
                           GL_TEXTURE_2D, frontObjectIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4,
                           GL_TEXTURE_2D, frontOutlineIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT5,
                           GL_TEXTURE_2D, frontShadowMaskTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, frontDepthTexture_, 0);
    const GLenum frontDrawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                       GL_COLOR_ATTACHMENT1,
                                       GL_COLOR_ATTACHMENT2,
                                       GL_COLOR_ATTACHMENT3,
                                       GL_COLOR_ATTACHMENT4};
    glDrawBuffers(5, frontDrawBuffers);
    const bool frontComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &viewSpaceNormalTexture_);
    glBindTexture(GL_TEXTURE_2D, viewSpaceNormalTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &materialDataTexture_);
    glBindTexture(GL_TEXTURE_2D, materialDataTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &shadowMaskTexture_);
    glBindTexture(GL_TEXTURE_2D, shadowMaskTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RED, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &objectIdTexture_);
    glBindTexture(GL_TEXTURE_2D, objectIdTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &depthTexture_);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
    // A packed attachment is supported on drivers that reject separate
    // depth textures and stencil renderbuffers in the same framebuffer.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_DEPTH_STENCIL,
                 GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &sceneFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, sceneColorTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, materialDataTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                           GL_TEXTURE_2D, shadowMaskTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, depthTexture_, 0);
    const GLenum sceneDrawAttachments[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, sceneDrawAttachments);
    const GLenum drawAttachment = GL_COLOR_ATTACHMENT0;
    const bool sceneComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenFramebuffers(1, &overlayDepthFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, overlayDepthFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, depthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool overlayDepthComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenFramebuffers(1, &attachmentClearFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, attachmentClearFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, viewSpaceNormalTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, objectIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                           GL_TEXTURE_2D, materialDataTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depthTexture_, 0);
    const GLenum attachmentDrawBuffers[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, attachmentDrawBuffers);
    const bool attachmentsComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &historyColorTexture_);
    glBindTexture(GL_TEXTURE_2D, historyColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &historyFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, historyFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, historyColorTexture_, 0);
    glDrawBuffers(1, &drawAttachment);
    const bool historyComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &resolvedColorTexture_);
    glBindTexture(GL_TEXTURE_2D, resolvedColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &resolveFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, resolvedColorTexture_, 0);
    glDrawBuffers(1, &drawAttachment);
    const bool resolveComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &depthOfFieldColorTexture_);
    glBindTexture(GL_TEXTURE_2D, depthOfFieldColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &depthOfFieldFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, depthOfFieldColorTexture_, 0);
    glDrawBuffers(1, &drawAttachment);
    const bool depthOfFieldComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    const QSize halfSize(std::max(pixelSize.width() / 2, 1),
                         std::max(pixelSize.height() / 2, 1));
    glGenTextures(1, &depthOfFieldSourceTexture_);
    glBindTexture(GL_TEXTURE_2D, depthOfFieldSourceTexture_);
    for (int level = 0; level < 3; ++level) {
        const int width = std::max(halfSize.width() >> level, 1);
        const int height = std::max(halfSize.height() >> level, 1);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA16F, width, height, 0,
                     GL_RGBA, GL_HALF_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &depthOfFieldCocTexture_);
    glBindTexture(GL_TEXTURE_2D, depthOfFieldCocTexture_);
    for (int level = 0; level < 3; ++level) {
        const int width = std::max(halfSize.width() >> level, 1);
        const int height = std::max(halfSize.height() >> level, 1);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RG8, width, height, 0,
                     GL_RG, GL_UNSIGNED_BYTE, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &depthOfFieldPrepareFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldPrepareFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, depthOfFieldSourceTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, depthOfFieldCocTexture_, 0);
    glDrawBuffers(2, attachmentDrawBuffers);
    const bool dofPrepareComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    const QSize mipScratchSize(std::max(halfSize.width() / 2, 1),
                               std::max(halfSize.height() / 2, 1));
    glGenTextures(1, &depthOfFieldMipColorTexture_);
    glBindTexture(GL_TEXTURE_2D, depthOfFieldMipColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, mipScratchSize.width(),
                 mipScratchSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &depthOfFieldMipCocTexture_);
    glBindTexture(GL_TEXTURE_2D, depthOfFieldMipCocTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, mipScratchSize.width(),
                 mipScratchSize.height(), 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &depthOfFieldMipFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldMipFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           depthOfFieldMipColorTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           depthOfFieldMipCocTexture_, 0);
    glDrawBuffers(2, attachmentDrawBuffers);
    const bool dofMipComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &depthOfFieldBlurTexture_);
    glBindTexture(GL_TEXTURE_2D, depthOfFieldBlurTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, halfSize.width(),
                 halfSize.height(), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &depthOfFieldBlurFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldBlurFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, depthOfFieldBlurTexture_, 0);
    glDrawBuffers(1, &drawAttachment);
    const bool dofBlurComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &smaaEdgeTexture_);
    glBindTexture(GL_TEXTURE_2D, smaaEdgeTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, pixelSize.width(),
                 pixelSize.height(), 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &smaaEdgeFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, smaaEdgeFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, smaaEdgeTexture_, 0);
    glDrawBuffers(1, &drawAttachment);
    const bool edgeComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &smaaWeightTexture_);
    glBindTexture(GL_TEXTURE_2D, smaaWeightTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &smaaWeightFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, smaaWeightFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, smaaWeightTexture_, 0);
    glDrawBuffers(1, &drawAttachment);
    const bool weightComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    QByteArray areaCompressed;
    QByteArray searchCompressed;
    QByteArray areaBytes;
    QByteArray searchBytes;
    const bool textureDataLoaded =
        readSmaaResource(
            QStringLiteral(":/classiCAD/shaders/viewport_smaa_area.b64"),
            &areaCompressed) &&
        readSmaaResource(
            QStringLiteral(":/classiCAD/shaders/viewport_smaa_search.b64"),
            &searchCompressed);
    if (textureDataLoaded) {
        areaBytes = qUncompress(QByteArray::fromBase64(areaCompressed));
        searchBytes = qUncompress(QByteArray::fromBase64(searchCompressed));
    }
    const bool textureDataValid = areaBytes.size() == 160 * 560 * 2 &&
                                  searchBytes.size() == 64 * 16;
    if (textureDataValid) {
        glGenTextures(1, &smaaAreaTexture_);
        glBindTexture(GL_TEXTURE_2D, smaaAreaTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 160, 560, 0,
                     GL_RG, GL_UNSIGNED_BYTE, areaBytes.constData());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &smaaSearchTexture_);
        glBindTexture(GL_TEXTURE_2D, smaaSearchTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 64, 16, 0,
                     GL_RED, GL_UNSIGNED_BYTE, searchBytes.constData());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glBindRenderbuffer(GL_RENDERBUFFER,
                       static_cast<GLuint>(previousRenderbuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (!sceneComplete || !overlayDepthComplete || !frontComplete ||
        !attachmentsComplete ||
        !historyComplete ||
        !resolveComplete || !depthOfFieldComplete || !dofPrepareComplete ||
        !dofBlurComplete || !dofMipComplete || !edgeComplete || !weightComplete ||
        !textureDataValid) {
        qWarning() << "Viewport AA target allocation failed"
                   << "scene=" << sceneComplete
                   << "overlay depth=" << overlayDepthComplete
                   << "front=" << frontComplete
                   << "attachments=" << attachmentsComplete
                   << "history=" << historyComplete
                   << "resolve=" << resolveComplete
                   << "dof prepare=" << dofPrepareComplete
                   << "dof blur=" << dofBlurComplete
                   << "dof mip=" << dofMipComplete
                   << "smaa edges=" << edgeComplete
                   << "smaa weights=" << weightComplete
                   << "smaa data=" << textureDataValid;
        destroyTargets();
        return false;
    }
    targetSize_ = pixelSize;
    pixelSize_ = pixelSize;
    accumulatedSampleCount_ = 0;
    accumulatedWeight_ = 0.0f;
    return true;
}

bool ViewportAaRenderer::ensureSampleZeroDepthTarget()
{
    if (sampleZeroDepthFramebuffer_ != 0 &&
        sampleZeroDepthTexture_ != 0 &&
        sampleZeroFrontDepthFramebuffer_ != 0 &&
        sampleZeroFrontDepthTexture_ != 0 &&
        sampleZeroDepthSize_ == targetSize_) {
        return true;
    }
    if (sampleZeroDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &sampleZeroDepthFramebuffer_);
        sampleZeroDepthFramebuffer_ = 0;
    }
    if (sampleZeroDepthTexture_ != 0) {
        glDeleteTextures(1, &sampleZeroDepthTexture_);
        sampleZeroDepthTexture_ = 0;
    }
    if (sampleZeroFrontDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &sampleZeroFrontDepthFramebuffer_);
        sampleZeroFrontDepthFramebuffer_ = 0;
    }
    if (sampleZeroFrontDepthTexture_ != 0) {
        glDeleteTextures(1, &sampleZeroFrontDepthTexture_);
        sampleZeroFrontDepthTexture_ = 0;
    }
    sampleZeroDepthSize_ = {};

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousTexture = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glGenTextures(1, &sampleZeroDepthTexture_);
    glBindTexture(GL_TEXTURE_2D, sampleZeroDepthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8,
                 targetSize_.width(), targetSize_.height(), 0,
                 GL_DEPTH_STENCIL,
                 GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &sampleZeroDepthFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, sampleZeroDepthFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sampleZeroDepthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool mainComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &sampleZeroFrontDepthTexture_);
    glBindTexture(GL_TEXTURE_2D, sampleZeroFrontDepthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8,
                 targetSize_.width(), targetSize_.height(), 0,
                 GL_DEPTH_STENCIL,
                 GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &sampleZeroFrontDepthFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, sampleZeroFrontDepthFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sampleZeroFrontDepthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool frontComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (!mainComplete || !frontComplete) {
        glDeleteFramebuffers(1, &sampleZeroDepthFramebuffer_);
        glDeleteTextures(1, &sampleZeroDepthTexture_);
        glDeleteFramebuffers(1, &sampleZeroFrontDepthFramebuffer_);
        glDeleteTextures(1, &sampleZeroFrontDepthTexture_);
        sampleZeroDepthFramebuffer_ = 0;
        sampleZeroDepthTexture_ = 0;
        sampleZeroFrontDepthFramebuffer_ = 0;
        sampleZeroFrontDepthTexture_ = 0;
        return false;
    }
    sampleZeroDepthSize_ = targetSize_;
    return true;
}

bool ViewportAaRenderer::beginSceneSample(const QSize &logicalSize,
                                          qreal devicePixelRatio,
                                          const ViewportTransform &camera,
                                          const QByteArray &historyKey,
                                          bool interactive,
                                          bool sceneRender)
{
    sceneSampleActive_ = false;
    targets_.materialDataTexture = 0;
    sceneRender_ = sceneRender;
    if (!initialize()) {
        return false;
    }
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize requestedPixelSize(qRound(logicalSize.width() * dpr),
                                   qRound(logicalSize.height() * dpr));
    if (!ensureTargets(requestedPixelSize)) {
        return false;
    }
    if (mode_ >= 5 && !interactive && !ensureSampleZeroDepthTarget()) {
        return false;
    }

    ++frameIndex_;
    const bool interactiveChanged = interactive_ != interactive;
    // Blender resets TAA during navigation/playback and on every one-sample
    // frame. The caller combines navigation with the explicit playback state
    // supplied through ViewportWidgetApi; reset each interactive paint and
    // each Single Pass frame to avoid stale history.
    const bool historyChanged = historyKey_ != historyKey ||
                                pixelSize_ != requestedPixelSize ||
                                interactiveChanged || interactive ||
                                mode_ == static_cast<int>(
                                             ViewportAntiAliasingMode::SinglePass);
    if (historyChanged) {
        historyKey_ = historyKey;
        interactive_ = interactive;
        ++historyEpoch_;
        accumulatedSampleCount_ = 0;
        accumulatedWeight_ = 0.0f;
        targets_.validAttachments = 0;
    }
    requestedSampleCount_ = mode_ >= 5 && !interactive ? mode_ : 1;
    temporalSample_ = mode_ >= 5 && !interactive;

    if (temporalSample_ && accumulatedSampleCount_ >= requestedSampleCount_) {
        targets_.pixelSize = requestedPixelSize;
        targets_.historyEpoch = historyEpoch_;
        targets_.frameIndex = frameIndex_;
        targets_.sampleIndex = requestedSampleCount_ - 1;
        targets_.sampleCount = requestedSampleCount_;
        targets_.currentSampleColorTexture = sceneColorTexture_;
        targets_.depthTexture = depthTexture_;
        targets_.historyColorTexture = historyColorTexture_;
        targets_.validAttachments |= ViewportRenderTargets::HistoryColor;
        return false;
    }

    const int sampleIndex = temporalSample_ ? accumulatedSampleCount_ : 0;
    const bool singlePassSample =
        mode_ == static_cast<int>(ViewportAntiAliasingMode::SinglePass);
    const JitterPoint offset = temporalSample_
                                   ? samplePositions(requestedSampleCount_)
                                         [static_cast<std::size_t>(sampleIndex)]
                                   : singlePassSample
                                         ? samplePositions(5)[0]
                                         : JitterPoint{};
    sampleCamera_ = camera;
    // Blender adds offset / resolution directly to its projection matrix.
    // viewportViewProjection converts screen pixels to clip space with a
    // factor of two, so pass half a physical pixel offset here to match that
    // Workbench projection shift.
    sampleCamera_.setRenderJitterPixels(
        QPointF(offset.x / (2.0 * dpr), offset.y / (2.0 * dpr)));

    GLint destinationFramebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destinationFramebuffer);
    destinationFramebuffer_ = static_cast<GLuint>(destinationFramebuffer);
    glGetIntegerv(GL_VIEWPORT, destinationViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, attachmentClearFramebuffer_);
    glViewport(0, 0, requestedPixelSize.width(), requestedPixelSize.height());
    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLuint clearObjectId[4] = {0u, 0u, 0u, 0u};
    const GLfloat clearMaterialData[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clearDepth[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 0, clearNormal);
    glClearBufferuiv(GL_COLOR, 1, clearObjectId);
    glClearBufferfv(GL_COLOR, 2, clearMaterialData);
    glClearBufferfv(GL_DEPTH, 0, clearDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
    glViewport(0, 0, requestedPixelSize.width(), requestedPixelSize.height());
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    const GLenum sceneSampleDrawBuffers[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, sceneSampleDrawBuffers);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    bool invertible = false;
    const QMatrix4x4 viewProjection =
        viewportViewProjection(sampleCamera_, logicalSize);
    targets_.inverseViewProjection = viewProjection.inverted(&invertible);
    targets_.pixelSize = requestedPixelSize;
    targets_.historyEpoch = historyEpoch_;
    targets_.frameIndex = frameIndex_;
    targets_.sampleIndex = sampleIndex;
    targets_.sampleCount = requestedSampleCount_;
    targets_.currentSampleColorTexture = sceneColorTexture_;
    targets_.sceneLinearColorTexture = sourceIsSceneLinear_
        ? sceneColorTexture_ : 0;
    targets_.historyColorTexture = historyColorTexture_;
    targets_.depthTexture = depthTexture_;
    targets_.viewSpaceNormalTexture = viewSpaceNormalTexture_;
    targets_.objectIdTexture = objectIdTexture_;
    targets_.frontColorTexture = frontColorTexture_;
    targets_.frontDepthTexture = frontDepthTexture_;
    targets_.frontMaterialDataTexture = frontMaterialDataTexture_;
    targets_.frontViewSpaceNormalTexture = frontViewSpaceNormalTexture_;
    targets_.frontObjectIdTexture = frontObjectIdTexture_;
    targets_.frontOutlineIdTexture = frontOutlineIdTexture_;
    targets_.frontFramebuffer = frontFramebuffer_;
    targets_.materialDataTexture = materialDataTexture_;
    targets_.shadowMaskTexture = shadowMaskTexture_;
    targets_.frontShadowMaskTexture = frontShadowMaskTexture_;
    targets_.historyColorIsSceneLinear = sourceIsSceneLinear_;
    targets_.resolvedSceneLinearColorTexture = 0;
    targets_.validAttachments = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, frontFramebuffer_);
    const GLenum frontClearDrawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                            GL_COLOR_ATTACHMENT1,
                                            GL_COLOR_ATTACHMENT2,
                                            GL_COLOR_ATTACHMENT3,
                                            GL_COLOR_ATTACHMENT4,
                                            GL_COLOR_ATTACHMENT5};
    glDrawBuffers(6, frontClearDrawBuffers);
    const GLfloat frontClearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat frontClearMaterial[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat frontClearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLuint frontClearId[4] = {0u, 0u, 0u, 0u};
    const GLfloat frontClearDepth[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 0, frontClearColor);
    glClearBufferfv(GL_COLOR, 1, frontClearMaterial);
    glClearBufferfv(GL_COLOR, 2, frontClearNormal);
    glClearBufferuiv(GL_COLOR, 3, frontClearId);
    glClearBufferuiv(GL_COLOR, 4, frontClearId);
    const GLfloat frontClearShadowMask[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 5, frontClearShadowMask);
    glClearBufferfv(GL_DEPTH, 0, frontClearDepth);
    const GLint frontClearStencil = 0;
    glClearBufferiv(GL_STENCIL, 0, &frontClearStencil);
    const GLenum frontDrawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                       GL_COLOR_ATTACHMENT1,
                                       GL_COLOR_ATTACHMENT2,
                                       GL_COLOR_ATTACHMENT3,
                                       GL_COLOR_ATTACHMENT4};
    glDrawBuffers(5, frontDrawBuffers);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
    const GLfloat clearShadowMask[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 2, clearShadowMask);
    sceneSampleActive_ = true;
    return true;
}

bool ViewportAaRenderer::runDepthOfField(GLuint sourceTexture)
{
    if (depthOfFieldNoiseTexture_ == 0) {
        const QByteArray blueNoiseRg = QByteArray::fromBase64(QByteArray(
            detail::kWorkbenchCavityBlueNoiseRgBase64,
            static_cast<qsizetype>(sizeof(detail::kWorkbenchCavityBlueNoiseRgBase64) - 1)));
        constexpr int noiseSize = 64;
        constexpr int channels = 2;
        if (blueNoiseRg.size() != noiseSize * noiseSize * channels) return false;
        std::array<float, noiseSize * noiseSize> alphaNoise{};
        for (int i = 0; i < noiseSize * noiseSize; ++i)
            alphaNoise[static_cast<std::size_t>(i)] =
                static_cast<unsigned char>(blueNoiseRg.at(i * channels + 1)) /
                256.0f;
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousTextureBinding = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding);
        glGenTextures(1, &depthOfFieldNoiseTexture_);
        glBindTexture(GL_TEXTURE_2D, depthOfFieldNoiseTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, noiseSize, noiseSize, 0,
                     GL_RED, GL_FLOAT, alphaNoise.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTextureBinding));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    }
    const auto &optics = depthOfFieldOptics_;
    const float focalLengthSceneUnits =
        float(optics.focalLengthMillimeters * 0.001);
    if (!std::isfinite(focalLengthSceneUnits) || focalLengthSceneUnits <= 0.0f ||
        !std::isfinite(optics.focusDistance) || optics.focusDistance <= 0.0 ||
        !std::isfinite(optics.fStop) || optics.fStop <= 0.0 ||
        !std::isfinite(optics.opticalSensorSizeMillimeters) ||
        optics.opticalSensorSizeMillimeters <= 0.0 ||
        !std::isfinite(optics.opticalSensorSizeMillimeters * 0.001) ||
        std::abs(optics.focusDistance - focalLengthSceneUnits) <= 1.0e-6 ||
        !std::isfinite(optics.clipStart) || optics.clipStart <= 0.0 ||
        !std::isfinite(optics.clipEnd) || optics.clipEnd <= optics.clipStart) {
        return false;
    }
    if (sourceTexture == 0 || depthOfFieldPrepareFramebuffer_ == 0 ||
        depthOfFieldBlurFramebuffer_ == 0 || depthOfFieldFramebuffer_ == 0 ||
        !depthOfFieldProgram_.isLinked()) {
        return false;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextures[5] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    for (int unit = 0; unit < 5; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[unit]);
    }
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    const QSize halfSize(std::max(targetSize_.width() / 2, 1),
                         std::max(targetSize_.height() / 2, 1));
    const auto bindPass = [&](int stage, GLuint framebuffer, int width, int height,
                              GLuint color, GLuint coc, GLuint blur, int lod) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, width, height);
        glUseProgram(static_cast<GLuint>(depthOfFieldProgram_.programId()));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, color);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, coc);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, depthTexture_);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, blur);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, depthOfFieldNoiseTexture_);
        depthOfFieldProgram_.setUniformValue("uSource", 0);
        depthOfFieldProgram_.setUniformValue("uCoc", 1);
        depthOfFieldProgram_.setUniformValue("uDepth", 2);
        depthOfFieldProgram_.setUniformValue("uBlur", 3);
        depthOfFieldProgram_.setUniformValue("uNoise", 4);
        depthOfFieldProgram_.setUniformValue("uStage", stage);
        depthOfFieldProgram_.setUniformValue("uSourceLod", lod);
        depthOfFieldProgram_.setUniformValue("uPerspective",
                                            depthOfFieldPerspective_);
        depthOfFieldProgram_.setUniformValue(
            "uNearFar", QVector2D(float(depthOfFieldOptics_.clipStart),
                                  float(depthOfFieldOptics_.clipEnd)));
        depthOfFieldProgram_.setUniformValue(
            "uFocusDistance", float(depthOfFieldOptics_.focusDistance));
        depthOfFieldProgram_.setUniformValue(
            "uFocalLengthMillimeters",
            float(depthOfFieldOptics_.focalLengthMillimeters));
        depthOfFieldProgram_.setUniformValue(
            "uOpticalSensorSizeMillimeters",
            float(depthOfFieldOptics_.opticalSensorSizeMillimeters));
        depthOfFieldProgram_.setUniformValue("uFStop", float(depthOfFieldOptics_.fStop));
        depthOfFieldProgram_.setUniformValue("uApertureBlades",
                                             depthOfFieldOptics_.apertureBlades);
        depthOfFieldProgram_.setUniformValue("uApertureRotation",
            float(depthOfFieldOptics_.apertureRotationRadians));
        depthOfFieldProgram_.setUniformValue("uApertureRatio",
            float(depthOfFieldOptics_.apertureRatio));
        depthOfFieldProgram_.setUniformValue(
            "uJitterOffset",
            float(accumulatedSampleCount_) /
                float(std::max(requestedSampleCount_, 1)));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };

    bool complete = true;
    const GLenum mrtAttachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldPrepareFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          depthOfFieldSourceTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                          depthOfFieldCocTexture_, 0);
    glDrawBuffers(2, mrtAttachments);
    complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        bindPass(0, depthOfFieldPrepareFramebuffer_, halfSize.width(),
                 halfSize.height(), sourceTexture, sourceTexture,
                 depthOfFieldBlurTexture_, 0);
    }

    for (int sourceLod = 0; complete && sourceLod < 2; ++sourceLod) {
        const int destinationLod = sourceLod + 1;
        const int width = std::max(halfSize.width() >> destinationLod, 1);
        const int height = std::max(halfSize.height() >> destinationLod, 1);
        glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldMipFramebuffer_);
        glDrawBuffers(2, mrtAttachments);
        complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            bindPass(1, depthOfFieldMipFramebuffer_, width, height,
                     depthOfFieldSourceTexture_, depthOfFieldCocTexture_,
                     depthOfFieldBlurTexture_, sourceLod);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, depthOfFieldMipFramebuffer_);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthOfFieldSourceTexture_);
            glCopyTexSubImage2D(GL_TEXTURE_2D, destinationLod, 0, 0,
                                0, 0, width, height);
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, depthOfFieldCocTexture_);
            glCopyTexSubImage2D(GL_TEXTURE_2D, destinationLod, 0, 0,
                                0, 0, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldMipFramebuffer_);
        }
    }

    if (complete) {
        const GLenum colorAttachment = GL_COLOR_ATTACHMENT0;
        glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldBlurFramebuffer_);
        glDrawBuffers(1, &colorAttachment);
        bindPass(2, depthOfFieldBlurFramebuffer_, halfSize.width(),
                 halfSize.height(), depthOfFieldSourceTexture_,
                 depthOfFieldCocTexture_, depthOfFieldBlurTexture_, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldPrepareFramebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               depthOfFieldSourceTexture_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                               GL_TEXTURE_2D, 0, 0);
        glDrawBuffers(1, &colorAttachment);
        bindPass(3, depthOfFieldPrepareFramebuffer_, halfSize.width(),
                 halfSize.height(), sourceTexture,
                 depthOfFieldCocTexture_, depthOfFieldBlurTexture_, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, depthOfFieldFramebuffer_);
        glDrawBuffers(1, &colorAttachment);
        bindPass(4, depthOfFieldFramebuffer_, targetSize_.width(),
                 targetSize_.height(), sourceTexture, depthOfFieldCocTexture_,
                 depthOfFieldSourceTexture_, 0);
    }

    glUseProgram(static_cast<GLuint>(previousProgram));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    for (int unit = 0; unit < 5; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTextures[unit]));
    }
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return complete;
}

const ViewportTransform &ViewportAaRenderer::sampleCamera() const noexcept
{
    return sampleCamera_;
}

bool ViewportAaRenderer::sceneSampleActive() const noexcept
{
    return sceneSampleActive_;
}

void ViewportAaRenderer::applyInFrontOverlayDepth()
{
    if (frontDepthTexture_ == 0 || depthTexture_ == 0 ||
        overlayDepthFramebuffer_ == 0 ||
        !overlayDepthProgram_.isLinked()) {
        return;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint previousDepthFunction = GL_LESS;
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousStencil = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean previousCull = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glBindFramebuffer(GL_FRAMEBUFFER, overlayDepthFramebuffer_);
    glViewport(0, 0, targetSize_.width(), targetSize_.height());
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, frontDepthTexture_);
    overlayDepthProgram_.bind();
    overlayDepthProgram_.setUniformValue("uFrontDepth", 0);
    {
        QOpenGLVertexArrayObject::Binder binder(&vertexArray_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glUseProgram(static_cast<GLuint>(previousProgram));
    glDepthMask(previousDepthMask);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (previousBlend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    if (previousStencil) glEnable(GL_STENCIL_TEST);
    else glDisable(GL_STENCIL_TEST);
    if (previousScissor) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    if (previousCull) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
}

void ViewportAaRenderer::drawSmaaResolve(float mixFactor)
{
    if (!initialized_ || historyColorTexture_ == 0 ||
        resolvedColorTexture_ == 0 || smaaEdgeTexture_ == 0 ||
        smaaWeightTexture_ == 0 || smaaAreaTexture_ == 0 ||
        smaaSearchTexture_ == 0 || accumulatedWeight_ <= 0.0f) {
        return;
    }

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextures[3] = {};
    GLint previousFramebufferSrgb = GL_FALSE;
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean previousStencil = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previousCull = glIsEnabled(GL_CULL_FACE);
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_FRAMEBUFFER_SRGB, &previousFramebufferSrgb);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    for (int unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[unit]);
    }

    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, targetSize_.width(), targetSize_.height());
    const QVector4D metrics(1.0f / std::max(1, targetSize_.width()),
                            1.0f / std::max(1, targetSize_.height()),
                            static_cast<float>(targetSize_.width()),
                            static_cast<float>(targetSize_.height()));
    const GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto drawFullscreen = [this](QOpenGLShaderProgram &program) {
        {
            QOpenGLVertexArrayObject::Binder binder(&vertexArray_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        program.release();
    };

    if (mixFactor > 0.0f) {
        glBindFramebuffer(GL_FRAMEBUFFER, smaaEdgeFramebuffer_);
        glClearBufferfv(GL_COLOR, 0, clearColor);
        QOpenGLShaderProgram &edgeProgram = smaaPrograms_[0];
        edgeProgram.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, historyColorTexture_);
        edgeProgram.setUniformValue("color_tx", 0);
        edgeProgram.setUniformValue("viewport_metrics", metrics);
        drawFullscreen(edgeProgram);

        glBindFramebuffer(GL_FRAMEBUFFER, smaaWeightFramebuffer_);
        glClearBufferfv(GL_COLOR, 0, clearColor);
        QOpenGLShaderProgram &weightProgram = smaaPrograms_[1];
        weightProgram.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, smaaEdgeTexture_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, smaaAreaTexture_);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, smaaSearchTexture_);
        weightProgram.setUniformValue("edges_tx", 0);
        weightProgram.setUniformValue("area_tx", 1);
        weightProgram.setUniformValue("search_tx", 2);
        weightProgram.setUniformValue("viewport_metrics", metrics);
        drawFullscreen(weightProgram);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, resolveFramebuffer_);
    glClearBufferfv(GL_COLOR, 0, clearColor);
    QOpenGLShaderProgram &resolveProgram = smaaPrograms_[2];
    resolveProgram.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, historyColorTexture_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, smaaWeightTexture_);
    resolveProgram.setUniformValue("color_tx", 0);
    resolveProgram.setUniformValue("blend_tx", 1);
    resolveProgram.setUniformValue("viewport_metrics", metrics);
    resolveProgram.setUniformValue("mix_factor", mixFactor);
    resolveProgram.setUniformValue("taa_accumulated_weight",
                                   accumulatedWeight_);
    drawFullscreen(resolveProgram);

    for (int unit = 0; unit < 3; ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousTextures[unit]));
    }
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glUseProgram(static_cast<GLuint>(previousProgram));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (previousStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (previousCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    if (previousFramebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB);
    else glDisable(GL_FRAMEBUFFER_SRGB);
}

void ViewportAaRenderer::drawPresentation(GLuint sourceTexture,
                                          GLuint destinationFramebuffer,
                                          const GLint destinationViewport[4],
                                          bool encodeDisplay,
                                          bool applyDepthOfField)
{
    if (sourceTexture == 0) {
        return;
    }
    GLint previousFramebufferSrgb = GL_FALSE;
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint previousDepthTexture = 0;
    GLint previousDisplayLutTexture = 0;
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    if (!presentProgram_.bind()) {
        glUseProgram(static_cast<GLuint>(previousProgram));
        return;
    }
    glGetIntegerv(GL_FRAMEBUFFER_SRGB, &previousFramebufferSrgb);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousDepthTexture);
    const bool transformAvailable = sourceIsSceneLinear_ &&
        displaySettings_.colorTransform != 1 && ensureDisplayTransformTexture();
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &previousDisplayLutTexture);

    glBindFramebuffer(GL_FRAMEBUFFER, destinationFramebuffer);
    glViewport(destinationViewport[0], destinationViewport[1],
               destinationViewport[2], destinationViewport[3]);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    presentProgram_.setUniformValue("uSource", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthTexture_);
    presentProgram_.setUniformValue("uDepth", 1);
    presentProgram_.setUniformValue(
        "uUseDepthOfField", applyDepthOfField && depthOfFieldEnabled_ &&
                                depthTexture_ != 0);
    presentProgram_.setUniformValue("uPerspective", depthOfFieldPerspective_);
    presentProgram_.setUniformValue("uEncodeDisplay", encodeDisplay);
    presentProgram_.setUniformValue("uApplyViewTransform", sourceIsSceneLinear_);
    presentProgram_.setUniformValue("uPremultipliedAlpha",
                                    sourceHasPremultipliedAlpha_);
    presentProgram_.setUniformValue("uDisplayLutAvailable", transformAvailable);
    presentProgram_.setUniformValue("uColorTransform",
                                    displaySettings_.colorTransform);
    const bool customDisplayLut = displaySettings_.colorTransform == 4;
    const ImportedBlenderColorLutMetadata &lutMetadata =
        displaySettings_.colorLutMetadata;
    presentProgram_.setUniformValue(
        "uDisplayLutSize", customDisplayLut
            ? lutMetadata.size
            : workbenchDisplayLutSize(displaySettings_.colorTransform));
    presentProgram_.setUniformValue(
        "uDisplayLutDomain",
        QVector2D(customDisplayLut ? lutMetadata.domainMin : 0.0,
                  customDisplayLut ? lutMetadata.domainMax
                      : workbenchDisplayLutDomainMaximum(
                            displaySettings_.colorTransform)));
    presentProgram_.setUniformValue(
        "uDisplayLutShaper",
        displaySettings_.colorTransform == 0 ||
                (customDisplayLut &&
                 lutMetadata.shaper == QStringLiteral("log2_1p"))
            ? 1 : 0);
    presentProgram_.setUniformValue(
        "uDisplayLutShaperScale",
        static_cast<float>(displaySettings_.colorTransform == 0
            ? workbenchAgxDisplayLutShaperScale
            : customDisplayLut ? lutMetadata.shaperScale : 1.0));
    presentProgram_.setUniformValue("uExposure",
                                    static_cast<float>(displaySettings_.colorExposure));
    presentProgram_.setUniformValue("uGamma",
                                    static_cast<float>(displaySettings_.colorGamma));
    presentProgram_.setUniformValue(
        "uDitherIntensity",
        static_cast<float>(std::max<qreal>(displaySettings_.colorDitherIntensity,
                                           0.0)));
    presentProgram_.setUniformValue(
        "uPixelOrigin", QVector2D(float(destinationViewport[0]),
                                  float(destinationViewport[1])));
    presentProgram_.setUniformValue("uDisplayLut", 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D,
                  transformAvailable ? displayTransformTexture_ : 0);
    presentProgram_.setUniformValue(
        "uNearFar", QVector2D(float(depthOfFieldOptics_.clipStart),
                              float(depthOfFieldOptics_.clipEnd)));
    presentProgram_.setUniformValue("uFocusDistance",
                                    float(depthOfFieldOptics_.focusDistance));
    presentProgram_.setUniformValue(
        "uFocalLengthMillimeters",
        float(depthOfFieldOptics_.focalLengthMillimeters));
    presentProgram_.setUniformValue(
        "uOpticalSensorSizeMillimeters",
        float(depthOfFieldOptics_.opticalSensorSizeMillimeters));
    presentProgram_.setUniformValue("uFStop", float(depthOfFieldOptics_.fStop));
    presentProgram_.setUniformValue(
        "uPixelSize", QVector2D(1.0f / std::max(1, targetSize_.width()),
                                1.0f / std::max(1, targetSize_.height())));
    glActiveTexture(GL_TEXTURE0);
    {
        QOpenGLVertexArrayObject::Binder binder(&vertexArray_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    presentProgram_.release();

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousDepthTexture));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D,
                  static_cast<GLuint>(previousDisplayLutTexture));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glUseProgram(static_cast<GLuint>(previousProgram));
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (previousFramebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB);
    else glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_FRAMEBUFFER, destinationFramebuffer);
}

void ViewportAaRenderer::copyDepthToFramebuffer(
    GLuint destinationFramebuffer,
    const GLint destinationViewport[4])
{
    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFramebuffer_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFramebuffer);
    glBlitFramebuffer(0, 0, targetSize_.width(), targetSize_.height(),
                      destinationViewport[0], destinationViewport[1],
                      destinationViewport[0] + destinationViewport[2],
                      destinationViewport[1] + destinationViewport[3],
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
}

bool ViewportAaRenderer::finishSceneSample()
{
    if (!sceneSampleActive_) {
        return false;
    }
    const int completedSampleIndex = accumulatedSampleCount_;
    sceneSampleActive_ = false;
    const bool accumulate = mode_ != static_cast<int>(ViewportAntiAliasingMode::Off);
    GLuint sampleColorTexture = sceneColorTexture_;
    const bool useDepthOfField = depthOfFieldEnabled_ &&
                                 depthTexture_ != 0 &&
                                 depthOfFieldFramebuffer_ != 0;
    if (useDepthOfField) {
        if (runDepthOfField(sceneColorTexture_)) {
            sampleColorTexture = depthOfFieldColorTexture_;
        }
    }
    // Blender applies this overlay-only override after scene effects and
    // immediately before AA. Keep physical front depth for transparency,
    // volume, outline, and DOF, then mask later editor overlays at the near
    // plane using the dedicated front-depth attachment.
    applyInFrontOverlayDepth();
    GLuint presentTexture = sampleColorTexture;
    if (accumulate) {
        const bool singlePassSample =
            mode_ == static_cast<int>(ViewportAntiAliasingMode::SinglePass);
        const JitterPoint offset = temporalSample_
                                       ? samplePositions(requestedSampleCount_)
                                             [static_cast<std::size_t>(
                                                 accumulatedSampleCount_)]
                                       : singlePassSample
                                             ? samplePositions(5)[0]
                                             : JitterPoint{};
        float sampleWeightSum = 0.0f;
        const std::array<float, 9> weights =
            workbenchSampleWeights(offset, &sampleWeightSum);
        const GLboolean previousBlend = glIsEnabled(GL_BLEND);
        const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousViewport[4] = {};
        GLint previousProgram = 0;
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousTexture = 0;
        GLint previousBlendEquationRgb = GL_FUNC_ADD;
        GLint previousBlendEquationAlpha = GL_FUNC_ADD;
        GLint previousBlendSourceRgb = GL_ONE;
        GLint previousBlendDestinationRgb = GL_ZERO;
        GLint previousBlendSourceAlpha = GL_ONE;
        GLint previousBlendDestinationAlpha = GL_ZERO;
        GLint previousFramebufferSrgb = GL_FALSE;
        GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        glGetIntegerv(GL_VIEWPORT, previousViewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSourceRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDestinationRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSourceAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDestinationAlpha);
        glGetIntegerv(GL_FRAMEBUFFER_SRGB, &previousFramebufferSrgb);
        glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        glBindFramebuffer(GL_FRAMEBUFFER, historyFramebuffer_);
        glViewport(0, 0, targetSize_.width(), targetSize_.height());
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (accumulatedSampleCount_ == 0) {
            glDisable(GL_BLEND);
            const GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 0, clearColor);
        }
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        accumulateProgram_.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sampleColorTexture);
        accumulateProgram_.setUniformValue("uSampleColor", 0);
        accumulateProgram_.setUniformValue(
            "uInvSize", QVector2D(1.0f / std::max(1, targetSize_.width()),
                                  1.0f / std::max(1, targetSize_.height())));
        accumulateProgram_.setUniformValueArray("uWeights", weights.data(),
                                                 int(weights.size()), 1);
        {
            QOpenGLVertexArrayObject::Binder binder(&vertexArray_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        accumulateProgram_.release();
        accumulatedWeight_ += sampleWeightSum;
        ++accumulatedSampleCount_;
        const float smaaMix = 1.0f - std::clamp(
            static_cast<float>(accumulatedSampleCount_ - 1) / 4.0f, 0.0f, 1.0f);
        drawSmaaResolve(smaaMix);
        presentTexture = resolvedColorTexture_;

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(previousReadFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(previousDrawFramebuffer));
        glViewport(previousViewport[0], previousViewport[1],
                   previousViewport[2], previousViewport[3]);
        glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                                static_cast<GLenum>(previousBlendEquationAlpha));
        glBlendFuncSeparate(static_cast<GLenum>(previousBlendSourceRgb),
                            static_cast<GLenum>(previousBlendDestinationRgb),
                            static_cast<GLenum>(previousBlendSourceAlpha),
                            static_cast<GLenum>(previousBlendDestinationAlpha));
        if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        glColorMask(previousColorMask[0], previousColorMask[1],
                    previousColorMask[2], previousColorMask[3]);
        if (previousFramebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB);
        else glDisable(GL_FRAMEBUFFER_SRGB);
        glUseProgram(static_cast<GLuint>(previousProgram));
    }

    // Workbench restores sample-zero depth for later TAA samples in the
    // interactive viewport, and after the final sample of an offline render.
    // Keep the in-front depth domain in step with the main scene depth.
    if (temporalSample_) {
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        glDisable(GL_SCISSOR_TEST);
        if (completedSampleIndex == 0) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFramebuffer_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              sampleZeroDepthFramebuffer_);
            glBlitFramebuffer(0, 0, targetSize_.width(), targetSize_.height(),
                              0, 0, targetSize_.width(), targetSize_.height(),
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, frontFramebuffer_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              sampleZeroFrontDepthFramebuffer_);
            glBlitFramebuffer(0, 0, targetSize_.width(), targetSize_.height(),
                              0, 0, targetSize_.width(), targetSize_.height(),
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else if (!sceneRender_ ||
                   accumulatedSampleCount_ >= requestedSampleCount_) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              sampleZeroDepthFramebuffer_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFramebuffer_);
            glBlitFramebuffer(0, 0, targetSize_.width(), targetSize_.height(),
                              0, 0, targetSize_.width(), targetSize_.height(),
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              sampleZeroFrontDepthFramebuffer_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frontFramebuffer_);
            glBlitFramebuffer(0, 0, targetSize_.width(), targetSize_.height(),
                              0, 0, targetSize_.width(), targetSize_.height(),
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(previousReadFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(previousDrawFramebuffer));
        if (previousScissor) glEnable(GL_SCISSOR_TEST);
        else glDisable(GL_SCISSOR_TEST);
    }

    targets_.currentSampleColorTexture = sceneColorTexture_;
    targets_.depthTexture = depthTexture_;
    targets_.historyColorTexture = historyColorTexture_;
    targets_.sampleIndex = temporalSample_
                               ? accumulatedSampleCount_ - 1
                               : 0;
    targets_.sampleCount = requestedSampleCount_;
    targets_.validAttachments |= ViewportRenderTargets::CurrentSampleColor |
                                 ViewportRenderTargets::Depth;
    if (sourceIsSceneLinear_) {
        targets_.validAttachments |= ViewportRenderTargets::SceneLinearColor;
    }
    if (accumulatedSampleCount_ > 0) {
        targets_.validAttachments |= ViewportRenderTargets::HistoryColor;
    }

    // Keep the final scene-linear texture available to offscreen render
    // callers. It includes the active DOF and temporal resolve, but remains
    // before display conversion and editor overlays.
    targets_.resolvedSceneLinearColorTexture = sourceIsSceneLinear_
        ? presentTexture : 0;
    if (targets_.resolvedSceneLinearColorTexture != 0) {
        targets_.validAttachments |=
            ViewportRenderTargets::ResolvedSceneLinearColor;
    }

    drawPresentation(presentTexture, destinationFramebuffer_, destinationViewport_,
                     !sourceIsSceneLinear_, false);
    copyDepthToFramebuffer(destinationFramebuffer_, destinationViewport_);
    return temporalSample_ && accumulatedSampleCount_ < requestedSampleCount_;
}

void ViewportAaRenderer::presentCachedHistory()
{
    if (accumulatedSampleCount_ <= 0 || resolvedColorTexture_ == 0) {
        return;
    }
    GLint destinationFramebuffer = 0;
    GLint viewport[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destinationFramebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    drawPresentation(resolvedColorTexture_,
                     static_cast<GLuint>(destinationFramebuffer), viewport,
                     !sourceIsSceneLinear_, false);
    copyDepthToFramebuffer(static_cast<GLuint>(destinationFramebuffer), viewport);
    targets_.validAttachments |= ViewportRenderTargets::HistoryColor |
                                ViewportRenderTargets::CurrentSampleColor |
                                ViewportRenderTargets::Depth;
}

bool ViewportAaRenderer::hasCachedHistory() const noexcept
{
    return accumulatedSampleCount_ > 0 && resolvedColorTexture_ != 0;
}

void ViewportAaRenderer::markCurrentSampleAttachmentsValid(
    quint32 attachmentFlags) noexcept
{
    if (!sceneSampleActive_) {
        return;
    }
    const quint32 sampleAttachments =
        ViewportRenderTargets::Depth |
        ViewportRenderTargets::ViewSpaceNormal |
        ViewportRenderTargets::ObjectId |
        ViewportRenderTargets::MaterialData |
        ViewportRenderTargets::ShadowMask |
        ViewportRenderTargets::FrontColor |
        ViewportRenderTargets::FrontDepth |
        ViewportRenderTargets::FrontViewSpaceNormal |
        ViewportRenderTargets::FrontObjectId |
        ViewportRenderTargets::FrontOutlineId |
        ViewportRenderTargets::FrontTransparentDepthAttachment;
    targets_.validAttachments |= attachmentFlags & sampleAttachments;
}

bool ViewportAaRenderer::needsAnotherSample() const noexcept
{
    return temporalSample_ && accumulatedSampleCount_ < requestedSampleCount_;
}

const ViewportRenderTargets &ViewportAaRenderer::targets() const noexcept
{
    return targets_;
}

QByteArray viewportAaHistoryKey(const ViewportRenderFrame &frame,
                                const ViewportShadingSettings &shading,
                                int aaMode,
                                qreal devicePixelRatio,
                                const QByteArray &sceneContentStateKey,
                                std::optional<QVector3D> worldColorSceneLinear,
                                const ViewportThemeBackground &themeBackground)
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << aaMode << devicePixelRatio
           << qint32(frame.viewportSize.width())
           << qint32(frame.viewportSize.height());
    const ViewportCameraPreferences optics = frame.camera.cameraPreferences();
    stream << optics.sceneCameraEnabled << optics.focusDistance << optics.fStop
           << optics.sensorWidthMillimeters
           << optics.opticalSensorSizeMillimeters
           << optics.focalLengthMillimeters << optics.clipStart << optics.clipEnd;
    stream << sceneContentStateKey << worldColorSceneLinear.has_value();
    if (worldColorSceneLinear) {
        stream << *worldColorSceneLinear;
    }
    const QMatrix4x4 viewProjection =
        viewportViewProjection(frame.camera, frame.viewportSize);
    for (int i = 0; i < 16; ++i) {
        stream << viewProjection.constData()[i];
    }
    stream << quint8(shading.mode) << quint8(shading.lightingMode)
           << shading.studioLightPreset << shading.matcapPreset
           << shading.matcapFlipX
           << shading.studioLightRotationDegrees << shading.worldSpaceLighting
           << shading.xray
           << quint8(shading.colorMode) << quint8(shading.backgroundMode)
           << quint8(shading.cavityType)
           << shading.customColor << shading.outlineColor
           << shading.customBackgroundColor << shading.shadowDirection
           << shading.backfaceCulling << shading.outline
           << shading.specularLighting << shading.shadows
           << shading.depthOfField << shading.cavity
           << shading.cavityDistance << shading.cavityAttenuation
           << shading.cavitySamples << shading.cavityRidgeFactor
           << shading.cavityValleyFactor << shading.curvatureRidgeFactor
           << shading.curvatureValleyFactor
           << shading.xrayAlpha << shading.shadowIntensity
           << shading.shadowOffset << shading.shadowFocus;
    const bool usesThemeBackground =
        shading.backgroundMode == ViewportBackgroundMode::Theme;
    stream << usesThemeBackground;
    if (usesThemeBackground) {
        stream << quint8(themeBackground.style)
               << themeBackground.highColor << themeBackground.lowColor;
    }
    stream << shading.colorTransform << shading.colorLutId
           << shading.colorExposure << shading.colorGamma;
    // Only filled Workbench surfaces and polygon meshes reach this pre-resolve
    // scene image. Curve/point/dimension wires and selection/tool overlays are
    // drawn after resolve; committed pictures are keyed by the caller.
    const auto contributesToSceneResolve = [&shading](
        const ViewportRenderObject &object) {
        return shading.mode == ViewportShadingMode::Solid &&
               (object.shape.geometryType == GeometryType::NurbsSurface ||
                object.shape.geometryType == GeometryType::NurbsSolid ||
                object.shape.geometryType == GeometryType::PolygonMesh);
    };
    int sceneObjectCount = 0;
    for (const ViewportRenderObject &object : frame.objects) {
        if (contributesToSceneResolve(object)) {
            ++sceneObjectCount;
        }
    }
    stream << qint32(sceneObjectCount);
    const auto writeMaterial = [&stream](const WorkbenchMaterialData &material) {
        const QFileInfo imageInfo(material.textureImagePath);
        stream << material.baseColorSceneLinear << material.roughness
               << material.metallic << material.alpha
               << material.textureImagePath << material.textureImageSrgb
               << imageInfo.exists() << imageInfo.size()
               << imageInfo.lastModified().toMSecsSinceEpoch()
               << material.textureImageAlphaMode
               << material.textureImageInterpolation
               << material.textureImageExtension
               << material.textureImageColorTransformId
               << material.textureImageSceneLinearFloatWidth
               << material.textureImageSceneLinearFloatHeight
               << QCryptographicHash::hash(
                      material.textureImageSceneLinearFloatDataBase64.toLatin1(),
                      QCryptographicHash::Sha256)
               << QCryptographicHash::hash(
                      material.textureImagePackedDataBase64.toLatin1(),
                      QCryptographicHash::Sha256)
               << qint32(material.textureImageTiles.size());
        for (const DocumentImageTextureTile &tile :
             material.textureImageTiles) {
            const QFileInfo tileInfo(tile.filepath);
            stream << qint32(tile.number) << tile.filepath
                   << tile.sourceFileExists << tileInfo.exists()
                   << tileInfo.size()
                   << tileInfo.lastModified().toMSecsSinceEpoch()
                   << QCryptographicHash::hash(
                          tile.packedDataBase64.toLatin1(),
                          QCryptographicHash::Sha256)
                   << tile.sceneLinearFloatWidth
                   << tile.sceneLinearFloatHeight
                   << QCryptographicHash::hash(
                          tile.sceneLinearFloatDataBase64.toLatin1(),
                          QCryptographicHash::Sha256);
        }
    };
    for (const ViewportRenderObject &object : frame.objects) {
        if (!contributesToSceneResolve(object)) {
            continue;
        }
        stream << quint64(object.objectId.value())
               << static_cast<quint8>(object.shape.geometryType)
               << quint64(object.geometryRevision)
               << object.selected << object.scalePreview << object.rotatePreview
               << object.showInFront << object.outlineHandledBySceneStroke
               << object.displayName << object.libraryPath << object.layerColor
               << object.placementTranslation.x
               << object.placementTranslation.y
               << object.placementTranslation.z
               << object.preparedGeometryOffset.x
               << object.preparedGeometryOffset.y
               << object.preparedGeometryOffset.z
               << quint64(object.preparedGeometryRevision)
               << object.displayColorSceneLinear
               << object.hasWorkbenchMaterial
               << qint32(object.meshMaterials.size());
        for (const WorkbenchMaterialData &material : object.meshMaterials) {
            writeMaterial(material);
        }
        writeMaterial(object.workbenchMaterial);
        QStringList faceMaterialIds = object.nurbsSolidFaceMaterials.keys();
        std::sort(faceMaterialIds.begin(), faceMaterialIds.end());
        stream << qint32(faceMaterialIds.size());
        for (const QString &faceId : faceMaterialIds) {
            stream << faceId;
            writeMaterial(object.nurbsSolidFaceMaterials.value(faceId));
        }
        for (const double component : object.meshObjectTransform) {
            stream << component;
        }
        if (object.scalePreview || object.rotatePreview) {
            stream << QJsonDocument(shapeToJson(object.shape))
                          .toJson(QJsonDocument::Compact);
        }
    }
    return key;
}

} // namespace classiCAD
