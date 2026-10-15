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
#include "ui/viewport/workbench_lighting.h"
#include "ui/viewport/workbench_matcap_texture.h"

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
#include <limits>

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

bool verifyMatcapTextureSampling(QOpenGLFunctions_3_3_Core &gl,
                                 const QString &presetName)
{
    int width = 0;
    int height = 0;
    const QByteArray layers = workbenchMatcapLinearHalfData(
        presetName, &width, &height);
    if (layers.isEmpty() || width < 2 || height < 2) return false;

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousArrayBinding = 0;
    GLint previous2dBinding = 0;
    GLint previousFramebuffer = 0;
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousViewport[4] = {};
    gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    gl.glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    gl.glGetIntegerv(GL_VIEWPORT, previousViewport);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous2dBinding);
    gl.glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousArrayBinding);
    const bool previousBlendEnabled = gl.glIsEnabled(GL_BLEND);
    const bool previousDepthTestEnabled = gl.glIsEnabled(GL_DEPTH_TEST);

    GLuint texture = 0;
    if (!uploadWorkbenchMatcapTextureArray(gl, &texture, layers,
                                            width, height)) {
        gl.glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        return false;
    }
    const bool uploadClean = gl.glGetError() == GL_NO_ERROR;

    static const char *vertexShader = R"GLSL(#version 330 core
void main()
{
    vec2 position = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                         gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(position, 0.0, 1.0);
}
)GLSL";
    static const char *fragmentShader = R"GLSL(#version 330 core
uniform sampler2DArray uMatcap;
uniform vec3 uCoordinate;
out vec4 outColor;
void main()
{
    outColor = vec4(texture(uMatcap, uCoordinate).rgb, 1.0);
}
)GLSL";
    QOpenGLShaderProgram program;
    const bool shaderReady =
        program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) &&
        program.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                        fragmentShader) &&
        program.link();

    GLuint outputTexture = 0;
    GLuint framebuffer = 0;
    GLuint vertexArray = 0;
    gl.glGenTextures(1, &outputTexture);
    gl.glBindTexture(GL_TEXTURE_2D, outputTexture);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA,
                    GL_FLOAT, nullptr);
    gl.glGenFramebuffers(1, &framebuffer);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, outputTexture, 0);
    const bool framebufferReady =
        gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.glGenVertexArrays(1, &vertexArray);

    bool samplesMatch = uploadClean && shaderReady && framebufferReady &&
                        texture != 0 && outputTexture != 0 &&
                        framebuffer != 0 && vertexArray != 0;
    if (samplesMatch) {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.glViewport(0, 0, 1, 1);
        gl.glDisable(GL_BLEND);
        gl.glDisable(GL_DEPTH_TEST);
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        gl.glBindVertexArray(vertexArray);
        program.bind();
        program.setUniformValue("uMatcap", 0);

        const int columns[] = {73, width - 83};
        const int rows[] = {41, height - 57};
        for (int layer = 0; layer < 2 && samplesMatch; ++layer) {
            for (int probe = 0; probe < 2 && samplesMatch; ++probe) {
                const float sourceU =
                    (float(columns[probe]) + 0.5f) / float(width);
                const float sourceV =
                    (float(rows[probe]) + 0.5f) / float(height);
                const QVector3D centerCoordinate(
                    sourceU, sourceV, float(layer));
                const QVector3D centerExpected =
                    workbenchMatcapSampleLinearHalf(
                        layers, width, height, layer, sourceU, sourceV);
                program.setUniformValue("uCoordinate", centerCoordinate);
                gl.glDrawArrays(GL_TRIANGLES, 0, 3);
                float sampled[4] = {};
                gl.glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, sampled);
                samplesMatch =
                    (QVector3D(sampled[0], sampled[1], sampled[2]) -
                     centerExpected).length() < 0.002f;

                const float midpointU =
                    (float(columns[probe]) + 1.0f) / float(width);
                const float midpointV =
                    (float(rows[probe]) + 1.0f) / float(height);
                program.setUniformValue(
                    "uCoordinate",
                    QVector3D(midpointU, midpointV, float(layer)));
                const QVector3D midpointExpected =
                    workbenchMatcapSampleLinearHalf(
                        layers, width, height, layer, midpointU, midpointV);
                gl.glDrawArrays(GL_TRIANGLES, 0, 3);
                gl.glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, sampled);
                samplesMatch = samplesMatch &&
                    (QVector3D(sampled[0], sampled[1], sampled[2]) -
                     midpointExpected).length() < 0.002f;
            }
        }
        samplesMatch = samplesMatch && gl.glGetError() == GL_NO_ERROR;
        program.release();
    }

    gl.glBindFramebuffer(GL_FRAMEBUFFER,
                          static_cast<GLuint>(previousFramebuffer));
    gl.glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    gl.glUseProgram(static_cast<GLuint>(previousProgram));
    gl.glViewport(previousViewport[0], previousViewport[1],
                  previousViewport[2], previousViewport[3]);
    if (previousBlendEnabled) gl.glEnable(GL_BLEND);
    else gl.glDisable(GL_BLEND);
    if (previousDepthTestEnabled) gl.glEnable(GL_DEPTH_TEST);
    else gl.glDisable(GL_DEPTH_TEST);
    gl.glBindTexture(GL_TEXTURE_2D_ARRAY,
                     static_cast<GLuint>(previousArrayBinding));
    gl.glBindTexture(GL_TEXTURE_2D,
                     static_cast<GLuint>(previous2dBinding));
    gl.glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    gl.glDeleteVertexArrays(1, &vertexArray);
    gl.glDeleteFramebuffers(1, &framebuffer);
    gl.glDeleteTextures(1, &outputTexture);
    gl.glDeleteTextures(1, &texture);
    return samplesMatch;
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
    passed &= check(verifyMatcapTextureSampling(
                        gl, QStringLiteral("metal_bronze")) &&
                        verifyMatcapTextureSampling(
                            gl, QStringLiteral("check_normal+y")),
                    "GPU MatCap array layers, row orientation, and bilinear samples must match Blender-derived half-float payloads");
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
        const bool inFrontFixture = qEnvironmentVariable(
            "CLASSICAD_WORKBENCH_PASS_SCENE") == QStringLiteral("TRANSPARENT_IN_FRONT");
        if (inFrontFixture) {
            camera.setViewPreset(ViewportViewPreset::Top);
        } else {
            camera.setViewDirection({4.0, -6.0, 4.0});
        }

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
        QVector<ViewportRenderObject> sceneObjects{cube};
        if (inFrontFixture) {
            sceneObjects.clear();
            const Point3D towardCamera = camera.viewDirection();
            const Point3D up = camera.viewUp();
            Point3D right{up.y * towardCamera.z - up.z * towardCamera.y,
                          up.z * towardCamera.x - up.x * towardCamera.z,
                          up.x * towardCamera.y - up.y * towardCamera.x};
            const qreal rightLength = std::sqrt(right.x * right.x +
                right.y * right.y + right.z * right.z);
            right = {right.x / rightLength, right.y / rightLength,
                     right.z / rightLength};
            auto addQuad = [&](quint64 id, const Point3D &center, qreal extent,
                               const QVector3D &color, qreal alpha,
                               bool showInFront) {
                ViewportRenderObject object;
                object.shape.geometryType = GeometryType::PolygonMesh;
                object.objectId = ObjectId::fromValue(id);
                object.geometryRevision = 1;
                auto point = [&](qreal x, qreal y) {
                    return Point3D{center.x + right.x * x + up.x * y,
                                   center.y + right.y * x + up.y * y,
                                   center.z + right.z * x + up.z * y};
                };
                const qreal half = extent * 0.5;
                object.shape.polygonMesh.vertices = {
                    point(-half, -half), point(half, -half),
                    point(half, half), point(-half, half)};
                object.shape.polygonMesh.faces = {{0, 1, 2, 3}};
                object.shape.polygonMesh.faceMaterialIndices = {0};
                WorkbenchMaterialData material;
                material.baseColorSceneLinear = color;
                material.alpha = alpha;
                object.meshMaterials = {material};
                object.showInFront = showInFront;
                object.preparedDepthGeometry =
                    QSharedPointer<ViewportDepthGeometry>::create(
                        buildViewportDepthGeometry(object.shape));
                sceneObjects.append(std::move(object));
            };
            addQuad(2, {right.x * 0.0, right.y * 0.0, right.z * 0.0},
                    3.2, QVector3D(0.12f, 0.62f, 0.18f), 1.0, false);
            addQuad(3, {right.x * -0.55 + towardCamera.x * 0.25,
                        right.y * -0.55 + towardCamera.y * 0.25,
                        right.z * -0.55 + towardCamera.z * 0.25},
                    2.5, QVector3D(0.08f, 0.25f, 0.95f), 0.55, false);
            addQuad(4, {right.x * 0.35 - towardCamera.x * 0.2,
                        right.y * 0.35 - towardCamera.y * 0.2,
                        right.z * 0.35 - towardCamera.z * 0.2},
                    1.15, QVector3D(0.95f, 0.72f, 0.05f), 1.0, true);
            addQuad(5, {right.x * 0.55 + towardCamera.x * 0.4,
                        right.y * 0.55 + towardCamera.y * 0.4,
                        right.z * 0.55 + towardCamera.z * 0.4},
                    1.4, QVector3D(0.95f, 0.12f, 0.04f), 0.48, true);
        }

        ViewportShadingSettings shading;
        shading.mode = ViewportShadingMode::Solid;
        shading.outline = false;
        ViewportAaRenderer aa;
        aa.setMode(static_cast<int>(ViewportAntiAliasingMode::Off));
        aa.setDisplayTransform(shading, true);
        ViewportSurfaceRenderer renderer;
        const auto rejectedPassHasNoOutputs = [&](
            const ViewportWorkbenchScenePassPacket &packet) {
            const ViewportWorkbenchSceneColorPassResult result =
                ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                    renderer, packet);
            return !result.rendered && result.report.completedPasses == 0 &&
                   result.report.populatedAttachments == 0;
        };
        const ViewportWorkbenchScenePassPacket emptySizePacket =
            ViewportWorkbenchScenePassPacket::fromScene(
                sceneObjects, camera, QSize(), 1.0, shading, nullptr, true);
        ViewportRenderTargets wrongSizeTargets;
        wrongSizeTargets.pixelSize = QSize(size.width() - 1, size.height());
        const ViewportWorkbenchScenePassPacket mismatchedTargetPacket =
            ViewportWorkbenchScenePassPacket::fromScene(
                sceneObjects, camera, size, 1.0, shading,
                &wrongSizeTargets, true);
        const ViewportWorkbenchScenePassPacket invalidDprPacket =
            ViewportWorkbenchScenePassPacket::fromScene(
                sceneObjects, camera, size,
                std::numeric_limits<qreal>::quiet_NaN(), shading,
                nullptr, true);
        passed &= check(
            !emptySizePacket.isValid() &&
                rejectedPassHasNoOutputs(emptySizePacket) &&
                !mismatchedTargetPacket.isValid() &&
                rejectedPassHasNoOutputs(mismatchedTargetPacket) &&
                !invalidDprPacket.isValid() &&
                rejectedPassHasNoOutputs(invalidDprPacket),
            "scene-pass coordinator must reject empty-size, mismatched-target, and non-finite-DPR packets without reporting passes or attachments");
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
        ViewportWorkbenchSceneColorPassResult scenePass;
        if (sampleStarted) {
            const GLfloat background[4] = {0.0232f, 0.0232f, 0.0232f, 1.0f};
            gl.glClearBufferfv(GL_COLOR, 0, background);
            scenePass = ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                renderer,
                ViewportWorkbenchScenePassPacket::fromScene(
                    sceneObjects, aa.sampleCamera(), size, 1.0, shading,
                    &aa.targets(), true));
            if (scenePass.rendered) {
                aa.markCurrentSampleAttachmentsValid(
                    scenePass.report.populatedAttachments);
            }
            passed &= check(scenePass.rendered &&
                                scenePass.completed(ViewportRenderPass::OpaqueColor),
                            "render-pass capture must complete the production opaque-color pass");
            passed &= check(
                (scenePass.report.populatedAttachments &
                 ViewportRenderTargets::CurrentSampleColor) != 0,
                "opaque rendering must report the current sample color attachment");
            if (scenePass.completed(
                    ViewportRenderPass::OpaqueDeferredResolve)) {
                passed &= check(
                    (scenePass.report.populatedAttachments &
                     ViewportRenderTargets::CurrentSampleColor) != 0,
                    "deferred material resolve must report the current sample color attachment");
            }
            if (scenePass.completed(ViewportRenderPass::TransparentResolve)) {
                passed &= check(
                    (scenePass.report.populatedAttachments &
                     ViewportRenderTargets::CurrentSampleColor) != 0,
                    "transparent resolve must report the current sample color attachment");
            }
            if (inFrontFixture) {
                passed &= check(
                    (scenePass.report.populatedAttachments &
                     ViewportRenderTargets::FrontColor) != 0,
                    "in-front rendering must report its front color attachment");
                passed &= check(
                    scenePass.completed(
                        ViewportRenderPass::TransparentAccumulation) &&
                        scenePass.completed(ViewportRenderPass::TransparentResolve),
                    "in-front comparison fixture must execute regular weighted-OIT accumulation and resolve");

                const auto readDepthTexturePixel = [&](GLuint texture,
                                                       int x, int y,
                                                       float *value) {
                    if (texture == 0 || value == nullptr) return false;
                    GLint previousReadFramebuffer = 0;
                    GLint previousReadBuffer = GL_NONE;
                    GLuint readFramebuffer = 0;
                    gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,
                                     &previousReadFramebuffer);
                    gl.glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
                    gl.glGenFramebuffers(1, &readFramebuffer);
                    gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
                    gl.glFramebufferTexture2D(
                        GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                        GL_TEXTURE_2D, texture, 0);
                    const bool complete = gl.glCheckFramebufferStatus(
                        GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                    if (complete) {
                        gl.glReadBuffer(GL_NONE);
                        gl.glReadPixels(x, size.height() - 1 - y, 1, 1,
                                        GL_DEPTH_COMPONENT, GL_FLOAT, value);
                    }
                    gl.glBindFramebuffer(
                        GL_READ_FRAMEBUFFER,
                        static_cast<GLuint>(previousReadFramebuffer));
                    gl.glReadBuffer(static_cast<GLenum>(previousReadBuffer));
                    gl.glDeleteFramebuffers(1, &readFramebuffer);
                    return complete;
                };
                float mergedFrontDepth = 1.0f;
                float isolatedFrontDepth = 1.0f;
                const bool depthReadsReady =
                    readDepthTexturePixel(aa.targets().depthTexture,
                                          365, 240, &mergedFrontDepth) &&
                    readDepthTexturePixel(aa.targets().frontDepthTexture,
                                          365, 240, &isolatedFrontDepth);
                passed &= check(
                    depthReadsReady && isolatedFrontDepth > 0.0f &&
                        isolatedFrontDepth < 1.0f &&
                        std::abs(mergedFrontDepth - isolatedFrontDepth) <
                            1.0e-6f,
                    "scene-depth merge must preserve the physical front-domain depth before the late overlay-depth override");
            }
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

        const QImage fromSceneDisplay = outputFramebuffer.toImage();
        ViewportRenderFrame renderFrame;
        renderFrame.viewportSize = size;
        renderFrame.objects = sceneObjects;
        ViewportAaRenderer frameAa;
        frameAa.setMode(static_cast<int>(ViewportAntiAliasingMode::Off));
        frameAa.setDisplayTransform(shading, true);
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                   GL_STENCIL_BUFFER_BIT);
        const bool frameSampleStarted = frameAa.beginSceneSample(
            size, 1.0, camera, QByteArrayLiteral("render-pass-cube-frame"),
            false, true);
        passed &= check(frameSampleStarted,
                        "frame-based scene-pass packet must begin its AA sample");
        ViewportWorkbenchSceneColorPassResult frameScenePass;
        if (frameSampleStarted) {
            const GLfloat background[4] = {0.0232f, 0.0232f, 0.0232f, 1.0f};
            gl.glClearBufferfv(GL_COLOR, 0, background);
            frameScenePass =
                ViewportWorkbenchScenePassCoordinator::renderSceneColor(
                    renderer,
                    ViewportWorkbenchScenePassPacket::fromFrame(
                        renderFrame, renderFrame.objects,
                        frameAa.sampleCamera(), 1.0, shading,
                        &frameAa.targets(), true));
            if (frameScenePass.rendered) {
                frameAa.markCurrentSampleAttachmentsValid(
                    frameScenePass.report.populatedAttachments);
            }
            frameAa.finishSceneSample();
        }
        QVector<float> frameLinearBottomUp(linearBottomUp.size());
        const GLuint frameColorTexture =
            frameAa.targets().resolvedSceneLinearColorTexture;
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousTexture = 0;
        gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        gl.glActiveTexture(GL_TEXTURE0);
        gl.glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        if (frameColorTexture != 0) {
            gl.glBindTexture(GL_TEXTURE_2D, frameColorTexture);
            gl.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT,
                             frameLinearBottomUp.data());
        }
        gl.glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        gl.glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        const auto readDepthTexture = [&](GLuint texture,
                                          QVector<float> &depth) {
            if (texture == 0) return false;
            GLint previousReadFramebuffer = 0;
            GLint previousReadBuffer = GL_NONE;
            gl.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,
                             &previousReadFramebuffer);
            gl.glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
            GLuint readFramebuffer = 0;
            gl.glGenFramebuffers(1, &readFramebuffer);
            gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
            gl.glFramebufferTexture2D(
                GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                GL_TEXTURE_2D, texture, 0);
            const bool complete = gl.glCheckFramebufferStatus(
                GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (complete) {
                gl.glReadBuffer(GL_NONE);
                gl.glReadPixels(0, 0, size.width(), size.height(),
                                GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
            }
            gl.glBindFramebuffer(
                GL_READ_FRAMEBUFFER,
                static_cast<GLuint>(previousReadFramebuffer));
            gl.glReadBuffer(static_cast<GLenum>(previousReadBuffer));
            gl.glDeleteFramebuffers(1, &readFramebuffer);
            return complete;
        };
        QVector<float> fromSceneDepth(size.width() * size.height());
        QVector<float> fromFrameDepth(size.width() * size.height());
        const bool pairedDepthReadable =
            readDepthTexture(aa.targets().depthTexture, fromSceneDepth) &&
            readDepthTexture(frameAa.targets().depthTexture, fromFrameDepth);
        passed &= check(
            frameScenePass.rendered &&
                frameScenePass.report.completedPasses ==
                    scenePass.report.completedPasses &&
                frameScenePass.report.populatedAttachments ==
                    scenePass.report.populatedAttachments &&
                frameAa.targets().isValid(
                    ViewportRenderTargets::ResolvedSceneLinearColor) &&
                frameLinearBottomUp == linearBottomUp &&
                pairedDepthReadable && fromFrameDepth == fromSceneDepth &&
                outputFramebuffer.toImage() == fromSceneDisplay,
            "fromScene and fromFrame coordinator routes must produce identical named passes, attachment masks, scene-linear Combined, depth, and display pixels");

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
        QVector<float> frontDepthBottomUp(depthBottomUp.size(), 1.0f);
        if (inFrontFixture && targets.frontDepthTexture != 0) {
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER,
                                      GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_TEXTURE_2D,
                                      targets.frontDepthTexture, 0);
            const GLenum frontDepthStatus =
                gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (frontDepthStatus == GL_FRAMEBUFFER_COMPLETE) {
                gl.glReadPixels(0, 0, size.width(), size.height(),
                                GL_DEPTH_COMPONENT, GL_FLOAT,
                                frontDepthBottomUp.data());
            }
            passed &= check(frontDepthStatus == GL_FRAMEBUFFER_COMPLETE,
                            "in-front fixture must expose a readable front-domain depth attachment");
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
        if (inFrontFixture) {
            const qsizetype regularTransparentPixel =
                qsizetype(size.height() - 1 - 240) * size.width() + 150;
            const QVector3D regularTransparentColor(
                linearBottomUp[regularTransparentPixel * 4],
                linearBottomUp[regularTransparentPixel * 4 + 1],
                linearBottomUp[regularTransparentPixel * 4 + 2]);
            passed &= check(
                std::abs(regularTransparentColor.x() - 0.0776f) < 0.01f &&
                    std::abs(regularTransparentColor.y() - 0.2106f) < 0.01f &&
                    std::abs(regularTransparentColor.z() - 0.2900f) < 0.01f,
                "regular transparent OIT color must match the isolated Blender scene-linear probe");
            const qsizetype frontOverlapPixel =
                qsizetype(size.height() - 1 - 240) * size.width() + 365;
            const QVector3D frontOverlapColor(
                linearBottomUp[frontOverlapPixel * 4],
                linearBottomUp[frontOverlapPixel * 4 + 1],
                linearBottomUp[frontOverlapPixel * 4 + 2]);
            passed &= check(frontOverlapColor.x() > 0.35f &&
                                frontOverlapColor.y() > 0.15f &&
                                frontOverlapColor.z() < 0.1f,
                            "in-front overlap color must match Blender at the projected probe");
            passed &= check(
                std::abs(viewZBottomUp[frontOverlapPixel] - clip.nearPlane) <
                    0.001,
                "opaque in-front coverage must receive Blender's near-plane view-Z override");

            const qsizetype transparentOnlyPixel =
                qsizetype(size.height() - 1 - 240) * size.width() + 450;
            const float physicalFrontViewZ = workbenchDepthToViewZ(
                frontDepthBottomUp[transparentOnlyPixel],
                camera.isPerspectiveEnabled(),
                static_cast<float>(clip.nearPlane),
                static_cast<float>(clip.farPlane));
            passed &= check(
                frontDepthBottomUp[transparentOnlyPixel] > 0.0f &&
                    frontDepthBottomUp[transparentOnlyPixel] < 1.0f &&
                    std::isfinite(physicalFrontViewZ) &&
                    std::abs(physicalFrontViewZ - 1000.0f) < 2.0f,
                "transparent-only in-front coverage must have physical view-Z near 1000");
            passed &= check(
                std::abs(viewZBottomUp[transparentOnlyPixel] -
                         physicalFrontViewZ) < 0.001,
                "transparent-only in-front coverage must retain Blender's physical view-Z");
        }

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
            const bool wroteFrontDepth = !inFrontFixture || writePfm(
                QDir(captureDirectory).filePath(QStringLiteral("classiCAD-front-depth.pfm")),
                "Pf", size, frontDepthBottomUp);
            const bool wroteDisplay = outputFramebuffer.toImage().save(
                QDir(captureDirectory).filePath(QStringLiteral("classiCAD-display.png")));
            passed &= check(wroteColor && wroteDepth && wroteFrontDepth && wroteDisplay,
                            "opt-in render-pass captures must write scene-linear Combined PFM, view-Z PFM, front-domain depth PFM when selected, and display PNG");
        }
        gl.glDeleteTextures(1, &outputDepthStencilTexture);
    }
    context.doneCurrent();
    return passed ? 0 : 1;
}
