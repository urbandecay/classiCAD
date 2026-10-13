/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/document/scene_object.h"
#include "core/geometry/geometry_type.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_render_frame.h"
#include "ui/viewport/viewport_shading.h"
#include "ui/viewport/viewport_surface_renderer.h"
#include "ui/viewport/viewport_aa_renderer.h"
#include "ui/viewport/blender_grid_renderer.h"
#include "ui/viewport/viewport_workbench_scene_pass_coordinator.h"
#include "ui/viewport/workbench_render_result.h"

#include <QDir>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOffscreenSurface>
#include <QSurfaceFormat>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) qCritical("%s", message);
    return condition;
}

bool writePfm(const QString &path, const char *magic, const QSize &size,
              const QVector<float> &pixels)
{
    QFile output(path);
    const QByteArray header = QByteArray(magic) + '\n' +
        QByteArray::number(size.width()) + ' ' +
        QByteArray::number(size.height()) + "\n-1.0\n";
    const qsizetype byteCount =
        pixels.size() * qsizetype(sizeof(float));
    return output.open(QIODevice::WriteOnly) &&
        output.write(header) == header.size() &&
        output.write(reinterpret_cast<const char *>(pixels.constData()),
                      byteCount) == byteCount;
}

} // namespace

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(32);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication application(argc, argv);

    bool passed = true;
    QOpenGLContext context;
    context.setFormat(format);
    passed &= check(context.create(),
                    "render-pass capture must create an OpenGL 3.3 context");
    if (!context.isValid()) return 1;
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    passed &= check(surface.isValid() && context.makeCurrent(&surface),
                    "render-pass capture must activate a direct offscreen surface");
    if (!surface.isValid() || QOpenGLContext::currentContext() != &context) {
        return 1;
    }

    QOpenGLFunctions_3_3_Core gl;
    passed &= check(gl.initializeOpenGLFunctions(),
                    "render-pass capture must initialize OpenGL 3.3 functions");
    const QSize size(640, 480);
    {
    QOpenGLFramebufferObjectFormat framebufferFormat;
    framebufferFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    framebufferFormat.setInternalTextureFormat(GL_RGBA16);
    QOpenGLFramebufferObject outputFramebuffer(size, framebufferFormat);
    passed &= check(outputFramebuffer.isValid() && outputFramebuffer.bind(),
                    "render-pass capture must allocate its result framebuffer");
    if (!outputFramebuffer.isValid()) {
        return 1;
    }
    GLuint outputDepthStencilTexture = 0;
    gl.glGenTextures(1, &outputDepthStencilTexture);
    gl.glBindTexture(GL_TEXTURE_2D, outputDepthStencilTexture);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8,
                    size.width(), size.height(), 0, GL_DEPTH_STENCIL,
                    GL_FLOAT_32_UNSIGNED_INT_24_8_REV, nullptr);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_TEXTURE_2D, outputDepthStencilTexture, 0);
    gl.glBindTexture(GL_TEXTURE_2D, 0);
    const bool outputFramebufferComplete =
        gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    passed &= check(outputDepthStencilTexture != 0 && outputFramebufferComplete,
                    "render-pass target must use a complete matching D32F/S8 depth attachment");

        ViewportTransform camera;
        camera.zoom() = 17.28;
        camera.setViewDirection({4.0, -6.0, 4.0});

        ViewportRenderObject cube;
        cube.shape.geometryType = GeometryType::PolygonMesh;
        cube.objectId = ObjectId::fromValue(1);
        cube.geometryRevision = 1;
        cube.shape.polygonMesh.vertices = {
            {-1.0, -1.0, -1.0}, {-1.0, -1.0, 1.0},
            {-1.0, 1.0, -1.0},  {-1.0, 1.0, 1.0},
            {1.0, -1.0, -1.0},  {1.0, -1.0, 1.0},
            {1.0, 1.0, -1.0},   {1.0, 1.0, 1.0}};
        cube.shape.polygonMesh.faces = {
            {0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
            {4, 5, 7, 6}, {1, 0, 2, 3}, {2, 6, 7, 3}};
        cube.shape.polygonMesh.faceMaterialIndices = {0, 0, 0, 0, 0, 0};
        cube.meshMaterials = {WorkbenchMaterialData{}};
        cube.preparedDepthGeometry = QSharedPointer<ViewportDepthGeometry>::create(
            buildViewportDepthGeometry(cube.shape));

        ViewportShadingSettings shading;
        shading.mode = ViewportShadingMode::Solid;
        shading.outline = false;
        ViewportAaRenderer aa;
        aa.setMode(static_cast<int>(ViewportAntiAliasingMode::Off));
        aa.setDisplayTransform(shading, true);
        ViewportSurfaceRenderer renderer;
        gl.glViewport(0, 0, size.width(), size.height());
        gl.glDisable(GL_FRAMEBUFFER_SRGB);
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glClearDepth(1.0);
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                   GL_STENCIL_BUFFER_BIT);
        const bool sampleStarted = aa.beginSceneSample(
            size, 1.0, camera, QByteArrayLiteral("render-pass-cube"),
            false, true);
        passed &= check(sampleStarted,
                        "render-pass capture must begin its production AA sample");
        if (sampleStarted) {
            const GLfloat background[4] = {0.0232f, 0.0232f, 0.0232f, 1.0f};
            gl.glClearBufferfv(GL_COLOR, 0, background);
            ViewportWorkbenchSceneColorPassRequest request;
            request.sharedTargets = &aa.targets();
            request.sceneLinearOutput = true;
            const ViewportWorkbenchSceneColorPassResult scenePass =
                ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                    renderer, {cube}, aa.sampleCamera(), size, 1.0, shading,
                    request);
            if (scenePass.rendered) {
                aa.markCurrentSampleAttachmentsValid(
                    scenePass.report.populatedAttachments);
            }
            passed &= check(scenePass.rendered &&
                                scenePass.completed(ViewportRenderPass::OpaqueColor),
                            "render-pass capture must complete the production opaque-color pass");
            aa.finishSceneSample();
            passed &= check(gl.glGetError() == GL_NO_ERROR,
                            "render-pass depth blit must not report a GL error");
        }

        QVector<float> linearBottomUp(size.width() * size.height() * 4);
        const ViewportRenderTargets &targets = aa.targets();
        const GLuint colorTexture = targets.resolvedSceneLinearColorTexture;
        passed &= check(targets.isValid(
                            ViewportRenderTargets::ResolvedSceneLinearColor) &&
                            colorTexture != 0,
                        "render-pass capture must resolve final scene-linear Combined");
        if (colorTexture != 0) {
            GLint previousTexture = 0;
            gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
            gl.glBindTexture(GL_TEXTURE_2D, colorTexture);
            gl.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT,
                             linearBottomUp.data());
            gl.glBindTexture(GL_TEXTURE_2D,
                             static_cast<GLuint>(previousTexture));
        }

        QVector<float> depthBottomUp(size.width() * size.height());
        outputFramebuffer.bind();
        gl.glReadPixels(0, 0, size.width(), size.height(),
                        GL_DEPTH_COMPONENT, GL_FLOAT,
                        depthBottomUp.data());
        QVector<float> sceneDepthBottomUp(depthBottomUp.size(), 1.0f);
        GLuint depthFramebuffer = 0;
        gl.glGenFramebuffers(1, &depthFramebuffer);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, depthFramebuffer);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_TEXTURE_2D, targets.depthTexture, 0);
        const GLenum sceneDepthStatus =
            gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (sceneDepthStatus == GL_FRAMEBUFFER_COMPLETE) {
            gl.glReadPixels(0, 0, size.width(), size.height(),
                            GL_DEPTH_COMPONENT, GL_FLOAT,
                            sceneDepthBottomUp.data());
        }
        gl.glDeleteFramebuffers(1, &depthFramebuffer);
        const ViewportProjectionClipRange clip =
            viewportProjectionClipRange(camera, size);
        QVector<float> viewZBottomUp(depthBottomUp.size());
        for (qsizetype index = 0; index < depthBottomUp.size(); ++index) {
            viewZBottomUp[index] = workbenchDepthToViewZ(
                depthBottomUp[index], camera.isPerspectiveEnabled(),
                static_cast<float>(clip.nearPlane),
                static_cast<float>(clip.farPlane));
        }

        const QVector4D projectedTopCenter = viewportViewProjection(camera, size) *
            QVector4D(0.0f, 0.0f, 1.0f, 1.0f);
        const int expectedX = std::clamp(
            qRound((projectedTopCenter.x() / projectedTopCenter.w() + 1.0f) *
                   size.width() * 0.5f), 0, size.width() - 1);
        const int expectedYTopDown = std::clamp(
            qRound((1.0f - projectedTopCenter.y() / projectedTopCenter.w()) *
                   size.height() * 0.5f), 0, size.height() - 1);
        const qsizetype projectedDepthIndex =
            qsizetype(size.height() - 1 - expectedYTopDown) * size.width() +
            expectedX;
        int foregroundPixels = 0;
        int backgroundPixels = 0;
        float maxTargetDepthDifference = 0.0f;
        for (qsizetype index = 0; index < depthBottomUp.size(); ++index) {
            maxTargetDepthDifference = std::max(
                maxTargetDepthDifference,
                std::abs(depthBottomUp[index] - sceneDepthBottomUp[index]));
        }
        for (float value : viewZBottomUp) {
            if (value == 1.0e10f) ++backgroundPixels;
            else if (std::isfinite(value) && value > 0.0f) ++foregroundPixels;
        }
        passed &= check(linearBottomUp.size() == size.width() * size.height() * 4 &&
                            viewZBottomUp.size() == size.width() * size.height(),
                        "render-pass Combined and Z captures must retain 640x480 dimensions");
        passed &= check(sceneDepthStatus == GL_FRAMEBUFFER_COMPLETE &&
                            maxTargetDepthDifference < 1.0e-6f,
                        "result target depth must preserve every production AA scene-depth sample");
        passed &= check(foregroundPixels > 1000 && backgroundPixels > 1000 &&
                            std::isfinite(viewZBottomUp[projectedDepthIndex]) &&
                            viewZBottomUp[projectedDepthIndex] < 1.0e10f,
                        "render-pass capture must map the projected upper cube face to top-down image coordinates with finite view-Z");
        passed &= check(viewZBottomUp[0] == 1.0e10f &&
                            viewZBottomUp[size.width() - 1] == 1.0e10f,
                        "render-pass background pixels must use Blender's 1e10 view-Z sentinel");

        const QString captureDirectory = qEnvironmentVariable(
            "CLASSICAD_WORKBENCH_PASS_CAPTURE_DIR");
        if (!captureDirectory.isEmpty()) {
            QDir().mkpath(captureDirectory);
            QVector<float> rgbBottomUp(size.width() * size.height() * 3);
            for (qsizetype pixel = 0; pixel < size.width() * size.height();
                 ++pixel) {
                rgbBottomUp[pixel * 3] = linearBottomUp[pixel * 4];
                rgbBottomUp[pixel * 3 + 1] = linearBottomUp[pixel * 4 + 1];
                rgbBottomUp[pixel * 3 + 2] = linearBottomUp[pixel * 4 + 2];
            }
            const bool wroteColor = writePfm(
                QDir(captureDirectory).filePath(QStringLiteral("classiCAD-combined.pfm")),
                "PF", size, rgbBottomUp);
            const bool wroteDepth = writePfm(
                QDir(captureDirectory).filePath(QStringLiteral("classiCAD-z.pfm")),
                "Pf", size, viewZBottomUp);
            const bool wroteDisplay = outputFramebuffer.toImage().save(
                QDir(captureDirectory).filePath(QStringLiteral("classiCAD-display.png")));
            passed &= check(wroteColor && wroteDepth && wroteDisplay,
                            "opt-in render-pass captures must write scene-linear Combined PFM, view-Z PFM, and display PNG");
        }
        gl.glDeleteTextures(1, &outputDepthStencilTexture);
    }
    context.doneCurrent();
    return passed ? 0 : 1;
}
