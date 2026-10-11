/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QMatrix4x4>
#include <QSize>
#include <QtCore/QtGlobal>

#include <QtGui/qopengl.h>

namespace classiCAD {

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
    };

    GLuint currentSampleColorTexture = 0;
    GLuint sceneLinearColorTexture = 0;
    GLuint historyColorTexture = 0;
    // Workbench TAA history stores weighted log2 values of the display-linear
    // scene color. It is an accumulation target, not scene-linear output.
    bool historyColorIsSceneLinear = false;
    GLuint depthTexture = 0;
    // RGBA16F view-space normals and R32UI scene object IDs are shared with
    // Workbench effects. A zero ID denotes the background.
    GLuint viewSpaceNormalTexture = 0;
    GLuint objectIdTexture = 0; // R32UI; 0 is reserved for background.
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
