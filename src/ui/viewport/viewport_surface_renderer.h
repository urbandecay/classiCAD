/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_shading.h"
#include "viewport_render_frame.h"
#include "viewport_render_targets.h"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>
#include <QString>
#include <QHash>

#include <array>

namespace classiCAD {

// Draws display triangles derived from exact NURBS surfaces. The saved
// NurbsSurface3D remains authoritative; this renderer consumes the cached
// tessellation shared with hit testing and scene-depth rendering.
class ViewportSurfaceRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportSurfaceRenderer();

    bool draw(const QVector<ViewportRenderObject> &objects,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio,
              const ViewportShadingSettings &settings,
              bool previewOverlay = false,
              bool clearDepth = true,
              bool selectionOverlay = false,
              const ViewportRenderTargets *sharedTargets = nullptr,
              quint32 *populatedAttachments = nullptr,
              bool sceneLinearOutput = false);

private:
    struct SurfaceVertex {
        float x;
        float y;
        float z;
        float nx;
        float ny;
        float nz;
        float attributeR = 1.0f;
        float attributeG = 1.0f;
        float attributeB = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
    };

    struct DrawRange {
        int first = 0;
        int count = 0;
        Point3D offset;
        bool selected = false;
        QVector4D displayColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
        WorkbenchMaterialData workbenchMaterial;
        bool useAttributeColor = false;
        bool useTextureColor = false;
        QString textureImagePath;
        bool textureImageSrgb = true;
        quint64 objectSeed = 0;
        quint32 effectObjectId = 0;
        QString displayName;
        QString libraryPath;
        bool outlineHandledBySceneStroke = false;
        bool shadowVolumeSupported = false;
        bool shadowTextureAlphaMask = false;
        QVector<QVector3D> shadowVertices;
        QVector<std::array<int, 3>> shadowTriangles;
    };

    bool initialize();
    void prepareGeometry(const QVector<ViewportRenderObject> &objects);
    bool renderShadowMap(const ViewportTransform &transform,
                         const QSize &viewportSize,
                         const ViewportShadingSettings &settings,
                         bool fallbackOnly);
    bool renderShadowVolumes(const QVector3D &lightDirection,
                             const QMatrix4x4 &viewProjection,
                             const QSize &viewportSize,
                             qreal devicePixelRatio,
                             const QVector<int> &opaqueRangeIndices);
    bool ensureShadowMap();
    bool ensureObjectIdFramebuffer(const QSize &pixelSize);
    bool ensureWorkbenchEffectFramebuffers(
        const QSize &pixelSize,
        const ViewportRenderTargets &targets);
    bool ensureTransparencyFramebuffer(const QSize &pixelSize,
                                      GLuint depthStencilTexture);
    bool renderWorkbenchEffectPrepass(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets &targets,
        const QVector<int> &rangeIndices,
        bool snapshotDepth,
        bool *snapshotDepthReady);
    bool renderWorkbenchTransparentDepthPass(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets &targets,
        const QVector<int> &rangeIndices);
    bool ensureMatcapTexture(const QString &presetName);
    bool ensureAgxDisplayTexture(int colorTransform,
                                 const QString &customLutId = {},
                                 const QByteArray &customLutBytes = {});
    bool ensureCavityJitterTexture(int totalSamples);
    GLuint ensureImageTexture(const QString &path, bool srgb);
    GLuint ensureImageTexture(const QString &path,
                              const QString &packedDataBase64,
                              bool srgb);

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram outlineProgram_;
    QOpenGLShaderProgram objectIdProgram_;
    QOpenGLShaderProgram workbenchEffectProgram_;
    QOpenGLShaderProgram shadowProgram_;
    QOpenGLShaderProgram transparencyResolveProgram_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLVertexArrayObject outlineVertexArray_;
    QOpenGLVertexArrayObject shadowVolumeVertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer shadowVolumeVertexBuffer_{QOpenGLBuffer::VertexBuffer};
    GLuint objectIdFramebuffer_ = 0;
    GLuint objectIdTexture_ = 0;
    GLuint objectIdDepthStencil_ = 0;
    QSize objectIdFramebufferSize_;
    bool objectIdFramebufferAttempted_ = false;
    GLuint workbenchEffectFramebuffer_ = 0;
    GLuint workbenchOutlineIdTexture_ = 0;
    GLuint cavityDepthFramebuffer_ = 0;
    GLuint cavityDepthTexture_ = 0;
    QSize workbenchEffectFramebufferSize_;
    GLuint transparencyFramebuffer_ = 0;
    GLuint transparencyAccumulationTexture_ = 0;
    GLuint transparencyWeightTexture_ = 0;
    QSize transparencyFramebufferSize_;
    QVector<SurfaceVertex> vertices_;
    QVector<DrawRange> ranges_;
    unsigned int matcapTexture_ = 0;
    unsigned int agxDisplayTexture_ = 0;
    int agxDisplayTextureTransform_ = -1;
    QString agxDisplayTextureCustomId_;
    QHash<QString, GLuint> customDisplayTextures_;
    GLuint cavityJitterTexture_ = 0;
    int cavityJitterSampleCount_ = 0;
    unsigned int shadowTexture_ = 0;
    unsigned int shadowFramebuffer_ = 0;
    QHash<QString, GLuint> imageTextures_;
    QMatrix4x4 shadowViewProjection_;
    QString matcapTextureName_;
    QByteArray geometryKey_;
    bool geometryDirty_ = true;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
};

} // namespace classiCAD
