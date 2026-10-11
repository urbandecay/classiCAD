/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_anti_aliasing.h"
#include "viewport_render_targets.h"
#include "viewport_render_frame.h"
#include "viewport_shading.h"
#include "services/viewport/viewport_transform.h"

#include <QByteArray>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>

#include <array>

namespace classiCAD {

class ViewportAaRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportAaRenderer();

    void setMode(int mode);
    int mode() const noexcept;

    // Returns true after binding a single-sample scene target. The supplied
    // history key must change whenever pixels in the Workbench scene change.
    bool beginSceneSample(const QSize &logicalSize,
                          qreal devicePixelRatio,
                          const ViewportTransform &camera,
                          const QByteArray &historyKey,
                          bool interactive);
    const ViewportTransform &sampleCamera() const noexcept;
    bool sceneSampleActive() const noexcept;

    // Resolve the current scene sample, present it into Qt's framebuffer, and
    // restore sample depth for later overlays. The return says whether idle
    // accumulation needs another viewport redraw.
    bool finishSceneSample();
    void presentCachedHistory();
    bool hasCachedHistory() const noexcept;
    void markCurrentSampleAttachmentsValid(quint32 attachmentFlags) noexcept;
    bool needsAnotherSample() const noexcept;
    const ViewportRenderTargets &targets() const noexcept;

private:
    bool initialize();
    bool ensureTargets(const QSize &pixelSize);
    void destroyTargets();
    void drawSmaaResolve(float mixFactor);
    void drawPresentation(GLuint sourceTexture,
                          GLuint destinationFramebuffer,
                          const GLint destinationViewport[4]);
    void copyDepthToFramebuffer(GLuint destinationFramebuffer,
                                const GLint destinationViewport[4]);

    int mode_ = static_cast<int>(ViewportAntiAliasingMode::Samples8);
    QSize pixelSize_;
    QByteArray historyKey_;
    ViewportTransform sampleCamera_;
    ViewportRenderTargets targets_;
    QOpenGLShaderProgram accumulateProgram_;
    QOpenGLShaderProgram presentProgram_;
    std::array<QOpenGLShaderProgram, 3> smaaPrograms_;
    QOpenGLVertexArrayObject vertexArray_;
    GLuint sceneFramebuffer_ = 0;
    GLuint attachmentClearFramebuffer_ = 0;
    GLuint sceneColorTexture_ = 0;
    GLuint depthTexture_ = 0;
    GLuint viewSpaceNormalTexture_ = 0;
    GLuint objectIdTexture_ = 0;
    GLuint historyFramebuffer_ = 0;
    GLuint historyColorTexture_ = 0;
    GLuint resolveFramebuffer_ = 0;
    GLuint resolvedColorTexture_ = 0;
    GLuint smaaEdgeFramebuffer_ = 0;
    GLuint smaaEdgeTexture_ = 0;
    GLuint smaaWeightFramebuffer_ = 0;
    GLuint smaaWeightTexture_ = 0;
    GLuint smaaAreaTexture_ = 0;
    GLuint smaaSearchTexture_ = 0;
    QSize targetSize_;
    GLuint destinationFramebuffer_ = 0;
    GLint destinationViewport_[4] = {};
    int requestedSampleCount_ = 1;
    int accumulatedSampleCount_ = 0;
    float accumulatedWeight_ = 0.0f;
    quint64 historyEpoch_ = 0;
    quint64 frameIndex_ = 0;
    bool interactive_ = false;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
    bool sceneSampleActive_ = false;
    bool temporalSample_ = false;
};

QByteArray viewportAaHistoryKey(const ViewportRenderFrame &frame,
                                const ViewportShadingSettings &shading,
                                int aaMode,
                                qreal devicePixelRatio,
                                const QByteArray &sceneContentStateKey = {});

} // namespace classiCAD
