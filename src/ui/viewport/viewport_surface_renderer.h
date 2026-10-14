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

    // Test-only diagnostic output. When enabled, the opaque Workbench color
    // shader writes cavity/edge/curvature scalars instead of surface color.
    // Defaults off and is only used by opt-in offscreen captures.
    void setCavityDiagnosticOutput(bool enabled) noexcept;

    bool draw(const QVector<ViewportRenderObject> &objects,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio,
              const ViewportShadingSettings &settings,
              bool previewOverlay = false,
              bool clearDepth = true,
              bool selectionOverlay = false,
              const ViewportRenderTargets *sharedTargets = nullptr,
              ViewportRenderPassReport *passReport = nullptr,
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
        bool showInFront = false;
        QVector4D displayColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
        WorkbenchMaterialData workbenchMaterial;
        bool hasWorkbenchMaterial = false;
        bool useAttributeColor = false;
        bool useTextureColor = false;
        QString textureImagePath;
        QVector<DocumentImageTextureTile> textureImageTiles;
        bool textureImageSrgb = true;
        QString textureImageColorTransformId = QStringLiteral("identity");
        quint64 objectSeed = 0;
        quint32 effectObjectId = 0;
        QString displayName;
        QString libraryPath;
        bool outlineHandledBySceneStroke = false;
        bool shadowVolumeSupported = false;
        bool shadowVolumeClosed = false;
        bool shadowVolumeForceDepthFail = false;
        bool shadowVolumePolygonMesh = false;
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
                             const QVector<int> &volumeRangeIndices,
                             const QVector<int> &opaqueRangeIndices);
    bool ensureShadowMap();
    bool ensureObjectIdFramebuffer(const QSize &pixelSize);
    bool ensureWorkbenchEffectFramebuffers(
        const QSize &pixelSize,
        const ViewportRenderTargets &targets);
    bool mergeWorkbenchFrontAttachments(const ViewportRenderTargets &targets,
                                        bool mergeColor,
                                        bool mergeNormal = true,
                                        bool mergeMaterial = false,
                                        bool mergeShadowMask = false,
                                        bool captureOpaqueCoverage = false);
    bool resolveWorkbenchMaterial(
        const QSize &pixelSize, const ViewportRenderTargets &targets,
        const ViewportTransform &transform, const QSize &viewportSize,
        const ViewportShadingSettings &settings, bool studioLighting,
        bool matcapLighting, qreal devicePixelRatio, bool cavityRequested,
        bool ssaoEnabled, bool cavityDepthReady);
    bool ensureTransparencyFramebuffer(const QSize &pixelSize,
                                      GLuint depthStencilTexture,
                                      bool frontDomain = false);
    bool renderWorkbenchEffectPrepass(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets &targets,
        const QVector<int> &rangeIndices,
        bool snapshotDepth,
        bool *snapshotDepthReady,
        bool *drewAnyRange = nullptr);
    bool renderWorkbenchTransparentDepthPass(
        const ViewportTransform &transform,
        const QSize &viewportSize,
        qreal devicePixelRatio,
        const ViewportShadingSettings &settings,
        const ViewportRenderTargets &targets,
        const QVector<int> &rangeIndices,
        bool excludeFrontCoverage = false);
    bool renderScalarVolumes(const QVector<ViewportRenderObject> &objects,
                             const ViewportTransform &transform,
                             const QSize &viewportSize,
                             qreal devicePixelRatio,
                             const ViewportShadingSettings &settings,
                             bool sceneLinearOutput,
                             int sampleIndex,
                             GLuint sceneDepthTexture,
                             GLuint frontDepthTexture);
    bool ensureMatcapTexture(const QString &presetName);
    bool ensureAgxDisplayTexture(int colorTransform,
                                 const QString &customLutId = {},
                                 const QByteArray &customLutBytes = {},
                                 const ImportedBlenderColorLutMetadata &customLutMetadata = {});
    bool ensureCavityJitterTexture(int totalSamples);
    GLuint ensureImageTexture(const QString &path, bool srgb);
    GLuint ensureImageTexture(const QString &path,
                              const QString &packedDataBase64,
                              bool srgb,
                              const QString &sceneLinearFloatDataBase64 = {},
                              int sceneLinearFloatWidth = 0,
                              int sceneLinearFloatHeight = 0);
    struct UdimTextureSet {
        GLuint imageArray = 0;
        GLuint tileLookup = 0;
    };
    UdimTextureSet ensureUdimTexture(const QVector<DocumentImageTextureTile> &tiles,
                                     bool srgb);

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram outlineProgram_;
    QOpenGLShaderProgram objectIdProgram_;
    QOpenGLShaderProgram workbenchEffectProgram_;
    QOpenGLShaderProgram shadowProgram_;
    QOpenGLShaderProgram scalarVolumeProgram_;
    QOpenGLShaderProgram transparencyResolveProgram_;
    QOpenGLShaderProgram frontMergeProgram_;
    QOpenGLShaderProgram flatMaterialResolveProgram_;
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
    bool cavityDiagnosticOutput_ = false;
    GLuint workbenchEffectFramebuffer_ = 0;
    GLuint frontMergeFramebuffer_ = 0;
    GLuint flatMaterialResolveFramebuffer_ = 0;
    GLuint workbenchOutlineIdTexture_ = 0;
    GLuint cavityDepthFramebuffer_ = 0;
    GLuint cavityDepthTexture_ = 0;
    QSize workbenchEffectFramebufferSize_;
    GLuint transparencyFramebuffer_ = 0;
    GLuint transparencyAccumulationTexture_ = 0;
    GLuint transparencyWeightTexture_ = 0;
    GLuint frontTransparencyFramebuffer_ = 0;
    GLuint frontTransparencyAccumulationTexture_ = 0;
    GLuint frontTransparencyWeightTexture_ = 0;
    GLuint frontTransparencyFallbackFramebuffer_ = 0;
    QSize transparencyFramebufferSize_;
    QVector<SurfaceVertex> vertices_;
    QVector<DrawRange> ranges_;
    unsigned int matcapTexture_ = 0;
    unsigned int agxDisplayTexture_ = 0;
    int agxDisplayTextureTransform_ = -1;
    QString agxDisplayTextureCustomId_;
    QHash<QString, GLuint> customDisplayTextures_;
    GLuint cavityJitterTexture_ = 0;
    GLuint scalarVolumeTexture_ = 0;
    int cavityJitterSampleCount_ = 0;
    unsigned int shadowTexture_ = 0;
    unsigned int shadowFramebuffer_ = 0;
    QHash<QString, GLuint> imageTextures_;
    QHash<QString, UdimTextureSet> udimImageTextures_;
    QMatrix4x4 shadowViewProjection_;
    QString matcapTextureName_;
    QByteArray geometryKey_;
    bool geometryDirty_ = true;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
};

} // namespace classiCAD
