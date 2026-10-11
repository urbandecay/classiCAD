/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2018-2022 Blender Authors
 * SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_aa_renderer.h"

#include "blender_grid_renderer.h"
#include "core/serialization/shape_json_codec.h"

#include <QDataStream>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QOpenGLContext>
#include <QVector2D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
    if (vertexArray_.isCreated()) {
        vertexArray_.destroy();
    }
    accumulateProgram_.removeAllShaders();
    presentProgram_.removeAllShaders();
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
    static constexpr const char *presentFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uSource;
        layout(location = 0) out vec4 fragmentColor;
        vec3 linearToSrgb(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color * 12.92;
            vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
            return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
        }
        void main()
        {
            vec4 source = texture(uSource, vUv);
            fragmentColor = vec4(linearToSrgb(source.rgb), source.a);
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
        !presentProgram_.link() || !smaaProgramsReady ||
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
    if (attachmentClearFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &attachmentClearFramebuffer_);
        attachmentClearFramebuffer_ = 0;
    }
    if (sceneColorTexture_ != 0) {
        glDeleteTextures(1, &sceneColorTexture_);
        sceneColorTexture_ = 0;
    }
    if (depthTexture_ != 0) {
        glDeleteTextures(1, &depthTexture_);
        depthTexture_ = 0;
    }
    if (viewSpaceNormalTexture_ != 0) {
        glDeleteTextures(1, &viewSpaceNormalTexture_);
        viewSpaceNormalTexture_ = 0;
    }
    if (objectIdTexture_ != 0) {
        glDeleteTextures(1, &objectIdTexture_);
        objectIdTexture_ = 0;
    }
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, pixelSize.width(),
                 pixelSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &viewSpaceNormalTexture_);
    glBindTexture(GL_TEXTURE_2D, viewSpaceNormalTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RGBA, GL_HALF_FLOAT, nullptr);
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &sceneFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, sceneColorTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, depthTexture_, 0);
    const GLenum drawAttachment = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawAttachment);
    const bool sceneComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenFramebuffers(1, &attachmentClearFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, attachmentClearFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, viewSpaceNormalTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, objectIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depthTexture_, 0);
    const GLenum attachmentDrawBuffers[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, attachmentDrawBuffers);
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
    if (!sceneComplete || !attachmentsComplete || !historyComplete ||
        !resolveComplete || !edgeComplete || !weightComplete ||
        !textureDataValid) {
        qWarning() << "Viewport AA target allocation failed"
                   << "scene=" << sceneComplete
                   << "attachments=" << attachmentsComplete
                   << "history=" << historyComplete
                   << "resolve=" << resolveComplete
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

bool ViewportAaRenderer::beginSceneSample(const QSize &logicalSize,
                                          qreal devicePixelRatio,
                                          const ViewportTransform &camera,
                                          const QByteArray &historyKey,
                                          bool interactive)
{
    sceneSampleActive_ = false;
    if (!initialize()) {
        return false;
    }
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize requestedPixelSize(qRound(logicalSize.width() * dpr),
                                   qRound(logicalSize.height() * dpr));
    if (!ensureTargets(requestedPixelSize)) {
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
    const JitterPoint offset = temporalSample_
                                   ? samplePositions(requestedSampleCount_)
                                         [static_cast<std::size_t>(sampleIndex)]
                                   : JitterPoint{};
    sampleCamera_ = camera;
    sampleCamera_.setRenderJitterPixels(
        QPointF(offset.x / dpr, offset.y / dpr));

    GLint destinationFramebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destinationFramebuffer);
    destinationFramebuffer_ = static_cast<GLuint>(destinationFramebuffer);
    glGetIntegerv(GL_VIEWPORT, destinationViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, attachmentClearFramebuffer_);
    glViewport(0, 0, requestedPixelSize.width(), requestedPixelSize.height());
    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLuint clearObjectId[4] = {0u, 0u, 0u, 0u};
    const GLfloat clearDepth[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 0, clearNormal);
    glClearBufferuiv(GL_COLOR, 1, clearObjectId);
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
    targets_.sceneLinearColorTexture = 0;
    targets_.historyColorTexture = historyColorTexture_;
    targets_.depthTexture = depthTexture_;
    targets_.viewSpaceNormalTexture = viewSpaceNormalTexture_;
    targets_.objectIdTexture = objectIdTexture_;
    targets_.materialDataTexture = 0;
    targets_.shadowMaskTexture = 0;
    targets_.historyColorIsSceneLinear = false;
    targets_.validAttachments = 0;
    sceneSampleActive_ = true;
    return true;
}

const ViewportTransform &ViewportAaRenderer::sampleCamera() const noexcept
{
    return sampleCamera_;
}

bool ViewportAaRenderer::sceneSampleActive() const noexcept
{
    return sceneSampleActive_;
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
                                          const GLint destinationViewport[4])
{
    if (sourceTexture == 0) {
        return;
    }
    GLint previousFramebufferSrgb = GL_FALSE;
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
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
    {
        QOpenGLVertexArrayObject::Binder binder(&vertexArray_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    presentProgram_.release();

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
    sceneSampleActive_ = false;
    const bool accumulate = mode_ != static_cast<int>(ViewportAntiAliasingMode::Off);
    GLuint presentTexture = sceneColorTexture_;
    if (accumulate) {
        const JitterPoint offset = temporalSample_
                                       ? samplePositions(requestedSampleCount_)
                                             [static_cast<std::size_t>(
                                                 accumulatedSampleCount_)]
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
        glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
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

    targets_.currentSampleColorTexture = sceneColorTexture_;
    targets_.depthTexture = depthTexture_;
    targets_.historyColorTexture = historyColorTexture_;
    targets_.sampleIndex = temporalSample_
                               ? accumulatedSampleCount_ - 1
                               : 0;
    targets_.sampleCount = requestedSampleCount_;
    targets_.validAttachments |= ViewportRenderTargets::CurrentSampleColor |
                                 ViewportRenderTargets::Depth;
    if (accumulatedSampleCount_ > 0) {
        targets_.validAttachments |= ViewportRenderTargets::HistoryColor;
    }

    drawPresentation(presentTexture, destinationFramebuffer_, destinationViewport_);
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
                     static_cast<GLuint>(destinationFramebuffer), viewport);
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
        ViewportRenderTargets::ShadowMask;
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
                                const QByteArray &sceneContentStateKey)
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << aaMode << devicePixelRatio
           << qint32(frame.viewportSize.width())
           << qint32(frame.viewportSize.height());
    stream << sceneContentStateKey;
    const QMatrix4x4 viewProjection =
        viewportViewProjection(frame.camera, frame.viewportSize);
    for (int i = 0; i < 16; ++i) {
        stream << viewProjection.constData()[i];
    }
    stream << quint8(shading.mode) << quint8(shading.lightingMode)
           << shading.studioLightPreset << shading.matcapPreset
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
    // Only filled Workbench surfaces reach this pre-resolve scene image.
    // Curve/point/dimension wires and selection/tool overlays are drawn after
    // resolve; committed pictures are keyed separately by the caller.
    const auto contributesToSceneResolve = [&shading](
        const ViewportRenderObject &object) {
        return shading.mode == ViewportShadingMode::Solid &&
               (object.shape.geometryType == GeometryType::NurbsSurface ||
                object.shape.geometryType == GeometryType::NurbsSolid);
    };
    int sceneSurfaceCount = 0;
    for (const ViewportRenderObject &object : frame.objects) {
        if (contributesToSceneResolve(object)) {
            ++sceneSurfaceCount;
        }
    }
    stream << qint32(sceneSurfaceCount);
    for (const ViewportRenderObject &object : frame.objects) {
        if (!contributesToSceneResolve(object)) {
            continue;
        }
        stream << quint64(object.objectId.value())
               << quint64(object.geometryRevision)
               << object.selected << object.scalePreview << object.rotatePreview
               << object.placementTranslation.x
               << object.placementTranslation.y
               << object.placementTranslation.z
               << object.preparedGeometryOffset.x
               << object.preparedGeometryOffset.y
               << object.preparedGeometryOffset.z
               << object.displayColorSceneLinear
               << object.workbenchMaterial.baseColorSceneLinear
               << object.workbenchMaterial.roughness
               << object.workbenchMaterial.metallic
               << object.workbenchMaterial.alpha
               << object.preparedGeometryRevision;
        if (object.scalePreview || object.rotatePreview) {
            stream << QJsonDocument(shapeToJson(object.shape))
                          .toJson(QJsonDocument::Compact);
        }
    }
    return key;
}

} // namespace classiCAD
