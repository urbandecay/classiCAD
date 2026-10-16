/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_anti_aliasing.h"
#include "viewport_render_targets.h"
#include "viewport_render_frame.h"
#include "viewport_shading.h"
#include "viewport_theme_background.h"
#include "services/viewport/viewport_transform.h"

#include <QByteArray>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>
#include <QString>

#include <array>
#include <optional>

namespace classiCAD {

// Blender 5.2.2 Workbench uses scene_state.sample / samples_len as the DOF
// noise offset. Keep this explicit so the per-sample shader sequence cannot
// drift from the scene's temporal sample index.
inline float workbenchDofNoiseOffset(int sampleIndex, int sampleCount) noexcept
{
    if (sampleCount <= 0) return 0.0f;
    return static_cast<float>(sampleIndex) / static_cast<float>(sampleCount);
}

class ViewportAaRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportAaRenderer();

    void setMode(int mode);
    int mode() const noexcept;
    void setDepthOfField(bool enabled,
                         const ViewportCameraPreferences &optics,
                         bool perspectiveView,
                         qreal projectionClipStart,
                         qreal projectionClipEnd);
    void setDisplayTransform(const ViewportShadingSettings &settings,
                             bool sourceIsSceneLinear,
                             bool sourceHasPremultipliedAlpha = false);

    // Returns true after binding a single-sample scene target. The supplied
    // history key must change whenever pixels in the Workbench scene change.
    bool beginSceneSample(const QSize &logicalSize,
                          qreal devicePixelRatio,
                          const ViewportTransform &camera,
                          const QByteArray &historyKey,
                          bool interactive,
                          bool sceneRender = false);
    const ViewportTransform &sampleCamera() const noexcept;
    bool sceneSampleActive() const noexcept;

    // Camera View-as-Render images are composed after scene effects and AA,
    // but before the display transform. Render each depth domain into its
    // own transparent scene-linear layer; those layers are never accumulated
    // into AA history.
    bool beginCameraImageLayer(bool inFront);
    void endCameraImageLayer();
    void clearCameraImageLayer(bool inFront);
    void setCameraImageLayersEnabled(bool enabled) noexcept
    {
        cameraImageLayersEnabled_ = enabled;
    }

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
    bool ensureSampleZeroDepthTarget();
    bool ensureDisplayTransformTexture();
    bool runDepthOfField(GLuint sourceTexture);
    void applyInFrontOverlayDepth();
    void destroyTargets();
    void drawSmaaResolve(float mixFactor);
    void drawPresentation(GLuint sourceTexture,
                          GLuint destinationFramebuffer,
                          const GLint destinationViewport[4],
                          bool encodeDisplay = true,
                          bool applyDepthOfField = false);
    void copyDepthToFramebuffer(GLuint destinationFramebuffer,
                                const GLint destinationViewport[4]);

    int mode_ = static_cast<int>(ViewportAntiAliasingMode::Samples8);
    QSize pixelSize_;
    QByteArray historyKey_;
    ViewportTransform sampleCamera_;
    ViewportRenderTargets targets_;
    QOpenGLShaderProgram accumulateProgram_;
    QOpenGLShaderProgram presentProgram_;
    QOpenGLShaderProgram depthOfFieldProgram_;
    QOpenGLShaderProgram overlayDepthProgram_;
    std::array<QOpenGLShaderProgram, 3> smaaPrograms_;
    QOpenGLVertexArrayObject vertexArray_;
    GLuint sceneFramebuffer_ = 0;
    GLuint overlayDepthFramebuffer_ = 0;
    GLuint attachmentClearFramebuffer_ = 0;
    GLuint sampleZeroDepthFramebuffer_ = 0;
    GLuint sampleZeroFrontDepthFramebuffer_ = 0;
    QSize sampleZeroDepthSize_;
    GLuint sceneColorTexture_ = 0;
    GLuint displayTransformTexture_ = 0;
    GLuint depthTexture_ = 0;
    GLuint sampleZeroDepthTexture_ = 0;
    GLuint sampleZeroFrontDepthTexture_ = 0;
    GLuint viewSpaceNormalTexture_ = 0;
    GLuint materialDataTexture_ = 0;
    GLuint shadowMaskTexture_ = 0;
    GLuint objectIdTexture_ = 0;
    GLuint frontColorTexture_ = 0;
    GLuint frontDepthTexture_ = 0;
    GLuint frontOpaqueOverlayMaskTexture_ = 0;
    GLuint frontMaterialDataTexture_ = 0;
    GLuint frontShadowMaskTexture_ = 0;
    GLuint frontViewSpaceNormalTexture_ = 0;
    GLuint frontObjectIdTexture_ = 0;
    GLuint frontOutlineIdTexture_ = 0;
    GLuint frontFramebuffer_ = 0;
    GLuint historyFramebuffer_ = 0;
    GLuint historyColorTexture_ = 0;
    GLuint cameraBackFramebuffer_ = 0;
    GLuint cameraBackColorTexture_ = 0;
    GLuint cameraFrontFramebuffer_ = 0;
    GLuint cameraFrontColorTexture_ = 0;
    GLuint cameraLayerPreviousDrawFramebuffer_ = 0;
    GLuint cameraLayerPreviousReadFramebuffer_ = 0;
    GLint cameraLayerPreviousViewport_[4] = {};
    GLboolean cameraLayerPreviousBlend_ = GL_FALSE;
    GLboolean cameraLayerPreviousDepthTest_ = GL_FALSE;
    GLboolean cameraLayerPreviousScissor_ = GL_FALSE;
    GLboolean cameraLayerPreviousFramebufferSrgb_ = GL_FALSE;
    GLboolean cameraLayerPreviousDepthMask_ = GL_TRUE;
    GLboolean cameraLayerPreviousColorMask_[4] = {
        GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint cameraLayerPreviousBlendEquationRgb_ = GL_FUNC_ADD;
    GLint cameraLayerPreviousBlendEquationAlpha_ = GL_FUNC_ADD;
    GLint cameraLayerPreviousBlendSourceRgb_ = GL_ONE;
    GLint cameraLayerPreviousBlendDestinationRgb_ = GL_ZERO;
    GLint cameraLayerPreviousBlendSourceAlpha_ = GL_ONE;
    GLint cameraLayerPreviousBlendDestinationAlpha_ = GL_ZERO;
    bool cameraImageLayerActive_ = false;
    bool cameraImageLayersEnabled_ = false;
    GLuint resolveFramebuffer_ = 0;
    GLuint resolvedColorTexture_ = 0;
    GLuint depthOfFieldFramebuffer_ = 0;
    GLuint depthOfFieldColorTexture_ = 0;
    GLuint depthOfFieldSourceTexture_ = 0;
    GLuint depthOfFieldCocTexture_ = 0;
    GLuint depthOfFieldBlurTexture_ = 0;
    GLuint depthOfFieldPrepareFramebuffer_ = 0;
    GLuint depthOfFieldBlurFramebuffer_ = 0;
    GLuint depthOfFieldMipColorTexture_ = 0;
    GLuint depthOfFieldMipCocTexture_ = 0;
    GLuint depthOfFieldMipFramebuffer_ = 0;
    GLuint depthOfFieldNoiseTexture_ = 0;
    GLuint smaaEdgeFramebuffer_ = 0;
    GLuint smaaEdgeTexture_ = 0;
    GLuint smaaWeightFramebuffer_ = 0;
    GLuint smaaWeightTexture_ = 0;
    GLuint smaaAreaTexture_ = 0;
    GLuint smaaSearchTexture_ = 0;
    QSize targetSize_;
    ViewportCameraPreferences depthOfFieldOptics_;
    bool depthOfFieldEnabled_ = false;
    bool depthOfFieldPerspective_ = false;
    ViewportShadingSettings displaySettings_;
    bool sourceIsSceneLinear_ = false;
    bool sourceHasPremultipliedAlpha_ = false;
    bool displayTransformTextureAvailable_ = false;
    int displayTransformTextureMode_ = -1;
    QString displayTransformTextureLutId_;
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
    bool sceneRender_ = false;
};

QByteArray viewportAaHistoryKey(const ViewportRenderFrame &frame,
                                const ViewportShadingSettings &shading,
                                int aaMode,
                                qreal devicePixelRatio,
                                const QByteArray &sceneContentStateKey = {},
                                std::optional<QVector3D> worldColorSceneLinear =
                                    std::nullopt,
                                const ViewportThemeBackground &themeBackground = {});

} // namespace classiCAD
