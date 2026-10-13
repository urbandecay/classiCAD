/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QMatrix4x4>
#include <QSize>
#include <QtCore/QtGlobal>

#include <QtGui/qopengl.h>

namespace classiCAD {

enum class ViewportRenderPass : quint32 {
    OpaqueColor = 1u << 0,
    TransparentColor = 1u << 1,
    TransparentAccumulation = 1u << 2,
    TransparentResolve = 1u << 3,
    TransparentDepth = 1u << 4,
    FrontTransparentAccumulation = 1u << 5,
    FrontTransparentResolve = 1u << 6,
    FrontTransparentDepth = 1u << 7,
    FrontTransparentMerge = 1u << 8,
    FrontOpaqueColor = 1u << 9,
    OpaqueDeferredResolve = 1u << 18,
    Volume = 1u << 10,
    OpaqueDepthPrepass = 1u << 11,
    OpaqueViewNormalPrepass = 1u << 12,
    OpaqueObjectIdPrepass = 1u << 13,
    FrontDepthPrepass = 1u << 14,
    FrontViewNormalPrepass = 1u << 16,
    FrontObjectIdPrepass = 1u << 17,
};

// Named completion and attachment report shared by the live viewport and
// offscreen Workbench render-result path.
struct ViewportRenderPassReport {
    quint32 completedPasses = 0;
    quint32 populatedAttachments = 0;

    void markPassCompleted(ViewportRenderPass pass) noexcept
    {
        completedPasses |= static_cast<quint32>(pass);
    }

    bool passCompleted(ViewportRenderPass pass) const noexcept
    {
        return (completedPasses & static_cast<quint32>(pass)) != 0;
    }
};

// Borrowed texture handles for the active Workbench sample and its resolved
// history. Consumers must check validAttachments before sampling a texture;
// a zero handle or a clear-only attachment is never advertised as valid.
struct ViewportRenderTargets {
    enum Attachment : quint32 {
        CurrentSampleColor = 1u << 0,
        SceneLinearColor = 1u << 1,
        Depth = 1u << 2,
        ViewSpaceNormal = 1u << 3,
        ObjectId = 1u << 4,
        MaterialData = 1u << 5,
        ShadowMask = 1u << 6,
        HistoryColor = 1u << 7,
        ResolvedSceneLinearColor = 1u << 8,
        FrontColor = 1u << 9,
        FrontDepth = 1u << 10,
        FrontViewSpaceNormal = 1u << 11,
        FrontObjectId = 1u << 12,
        FrontOutlineId = 1u << 13,
        FrontTransparentDepthAttachment = 1u << 15,
    };

    GLuint currentSampleColorTexture = 0;
    GLuint sceneLinearColorTexture = 0;
    // Final post-DOF/AA scene-linear output, before the display transform.
    GLuint resolvedSceneLinearColorTexture = 0;
    GLuint historyColorTexture = 0;
    // When true, history contains weighted log2 values of scene-linear color.
    // The stored numbers are encoded for accumulation rather than raw RGB.
    bool historyColorIsSceneLinear = false;
    GLuint depthTexture = 0;
    // RGBA16F view-space normals and R32UI scene object IDs are shared with
    // Workbench effects. A zero ID denotes the background.
    GLuint viewSpaceNormalTexture = 0;
    GLuint objectIdTexture = 0; // R32UI; 0 is reserved for background.
    // Isolated opaque showInFront sample, merged by the surface renderer
    // into the ordinary per-sample attachments before effects and AA resolve.
    GLuint frontColorTexture = 0;
    GLuint frontDepthTexture = 0;
    GLuint frontMaterialDataTexture = 0;
    GLuint frontViewSpaceNormalTexture = 0;
    GLuint frontObjectIdTexture = 0;
    GLuint frontOutlineIdTexture = 0;
    GLuint frontFramebuffer = 0;
    // RGBA16F opaque material G-buffer: scene-linear base RGB plus the
    // Blender Workbench mode-dependent payload in alpha.
    GLuint materialDataTexture = 0;
    GLuint shadowMaskTexture = 0;
    QSize pixelSize;
    QMatrix4x4 inverseViewProjection;
    quint64 historyEpoch = 0;
    quint64 frameIndex = 0;
    int sampleIndex = 0;
    int sampleCount = 1;
    quint32 validAttachments = 0;

    bool isValid(Attachment attachment) const noexcept
    {
        return (validAttachments & static_cast<quint32>(attachment)) != 0;
    }
};

} // namespace classiCAD
