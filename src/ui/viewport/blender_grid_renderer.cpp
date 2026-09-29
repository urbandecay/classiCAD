/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 *
 * Adapted from Blender's 3D View grid shaders and grid draw setup.
 * Blender Authors' original shader portions are GPL-2.0-or-later.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "blender_grid_renderer.h"
#include "blender_grid_frame.h"
#include "blender_grid_scale.h"

#include <QColor>
#include <QDebug>
#include <QMatrix4x4>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qreal kViewportSensorWidthMillimeters = 36.0;
constexpr qreal kBlenderViewportProjectionZoom = 2.0;
constexpr int kPerspectiveGridLineCount = 151;
constexpr int kOrthographicGridLineCount = 301;
constexpr int kGridLevelsDrawn = 3;
constexpr int kPerspectiveGridIterations = 4;

QVector3D asVector(const Point3D &point)
{
    return {static_cast<float>(point.x),
            static_cast<float>(point.y),
            static_cast<float>(point.z)};
}

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D add(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D multiply(const Point3D &point, qreal scalar)
{
    return {point.x * scalar, point.y * scalar, point.z * scalar};
}

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Point3D normalized(const Point3D &point)
{
    const qreal length = std::sqrt(dot(point, point));
    if (length <= 1.0e-15) {
        return {};
    }
    return multiply(point, 1.0 / length);
}

QVector4D colorVector(const QColor &color, qreal opacity)
{
    return {static_cast<float>(color.redF()),
            static_cast<float>(color.greenF()),
            static_cast<float>(color.blueF()),
            static_cast<float>(color.alphaF() * opacity)};
}

QMatrix4x4 viewProjection(const ViewportTransform &transform,
                          const QSize &viewportSize,
                          Point3D *renderCameraPosition)
{
    const ViewportCameraState camera = transform.cameraState();
    const ViewportCameraPreferences cameraPreferences = transform.cameraPreferences();
    const qreal cosineYaw = std::cos(camera.yawRadians);
    const qreal sineYaw = std::sin(camera.yawRadians);
    const Point3D right{cosineYaw, sineYaw, 0.0};
    const Point3D outward = transform.viewDirection();
    const Point3D forward = multiply(outward, -1.0);
    const Point3D up = normalized({right.y * forward.z - right.z * forward.y,
                                   right.z * forward.x - right.x * forward.z,
                                   right.x * forward.y - right.y * forward.x});
    const Point3D target = transform.viewTarget();
    Point3D eye = transform.cameraPosition(viewportSize);
    if (!camera.perspective) {
        eye = add(target, multiply(outward, cameraPreferences.clipEnd));
    }
    if (renderCameraPosition != nullptr) {
        *renderCameraPosition = eye;
    }

    QMatrix4x4 view;
    view.lookAt(asVector(eye), asVector(target), asVector(up));

    QMatrix4x4 projection;
    if (camera.perspective) {
        const qreal sensorExtent = std::max(viewportSize.width(),
                                            viewportSize.height());
        const qreal focalLengthPixels = sensorExtent *
                                        cameraPreferences.focalLengthMillimeters /
                                        (kViewportSensorWidthMillimeters *
                                         kBlenderViewportProjectionZoom);
        const qreal nearPlane = cameraPreferences.clipStart;
        const qreal halfWidth = viewportSize.width() * nearPlane /
                                (2.0 * focalLengthPixels);
        const qreal halfHeight = viewportSize.height() * nearPlane /
                                 (2.0 * focalLengthPixels);
        projection.frustum(-halfWidth,
                           halfWidth,
                           -halfHeight,
                           halfHeight,
                           nearPlane,
                           cameraPreferences.clipEnd);
    } else {
        const qreal zoom = std::max<qreal>(transform.zoom(), 1.0e-8);
        projection.ortho(-viewportSize.width() / (2.0 * zoom),
                         viewportSize.width() / (2.0 * zoom),
                         -viewportSize.height() / (2.0 * zoom),
                         viewportSize.height() / (2.0 * zoom),
                         cameraPreferences.clipStart,
                         2.0 * cameraPreferences.clipEnd);
    }
    return projection * view;
}

} // namespace

BlenderGridRenderer::~BlenderGridRenderer()
{
    if (context_.isValid() && surface_.isValid() &&
        context_.makeCurrent(&surface_)) {
        framebuffer_.reset();
        if (sceneDepthVertexBuffer_.isCreated()) {
            sceneDepthVertexBuffer_.destroy();
        }
        if (sceneDepthVertexArray_.isCreated()) {
            sceneDepthVertexArray_.destroy();
        }
        if (vertexArray_.isCreated()) {
            vertexArray_.destroy();
        }
        program_.removeAllShaders();
        sceneDepthProgram_.removeAllShaders();
        context_.doneCurrent();
    }
}

void BlenderGridRenderer::setAntiAliasingSamples(int samples)
{
    if (samples != 0 && samples != 2 && samples != 4 && samples != 8) {
        return;
    }
    if (antiAliasingSamples_ != samples) {
        antiAliasingSamples_ = samples;
    }
}

int BlenderGridRenderer::antiAliasingSamples() const
{
    return antiAliasingSamples_;
}

bool BlenderGridRenderer::initialize()
{
    if (initializationAttempted_) {
        return initialized_;
    }
    initializationAttempted_ = true;

    context_.setFormat(QSurfaceFormat::defaultFormat());
    if (!context_.create()) {
        qWarning() << "Blender grid renderer: could not create an OpenGL context";
        return false;
    }
    surface_.setFormat(context_.format());
    surface_.create();
    if (!surface_.isValid() || !context_.makeCurrent(&surface_)) {
        qWarning() << "Blender grid renderer: could not activate an offscreen surface";
        return false;
    }

    if (!initializeOpenGLFunctions()) {
        qWarning() << "Blender grid renderer: OpenGL 3.3 functions unavailable";
        context_.doneCurrent();
        return false;
    }
    if (!program_.addShaderFromSourceFile(QOpenGLShader::Vertex,
                                          QStringLiteral(":/classiCAD/shaders/blender_grid.vert")) ||
        !program_.addShaderFromSourceFile(QOpenGLShader::Fragment,
                                          QStringLiteral(":/classiCAD/shaders/blender_grid.frag")) ||
        !program_.link()) {
        qWarning().noquote() << "Blender grid shader setup failed:" << program_.log();
        context_.doneCurrent();
        return false;
    }
    if (!sceneDepthProgram_.addShaderFromSourceFile(
            QOpenGLShader::Vertex,
            QStringLiteral(":/classiCAD/shaders/scene_depth.vert")) ||
        !sceneDepthProgram_.addShaderFromSourceFile(
            QOpenGLShader::Fragment,
            QStringLiteral(":/classiCAD/shaders/scene_depth.frag")) ||
        !sceneDepthProgram_.link()) {
        qWarning().noquote() << "Viewport scene-depth shader setup failed:"
                             << sceneDepthProgram_.log();
        context_.doneCurrent();
        return false;
    }
    if (!vertexArray_.create()) {
        qWarning() << "Blender grid renderer: could not create vertex array";
        context_.doneCurrent();
        return false;
    }
    if (!sceneDepthVertexArray_.create() || !sceneDepthVertexBuffer_.create()) {
        qWarning() << "Blender grid renderer: could not create scene-depth buffers";
        context_.doneCurrent();
        return false;
    }
    initialized_ = true;
    context_.doneCurrent();
    return true;
}

QImage BlenderGridRenderer::render(const ViewportTransform &transform,
                                   const QSize &viewportSize,
                                   qreal devicePixelRatio,
                                   const QVector<Shape> &visibleSceneShapes,
                                   qreal baseGridStep,
                                   const BlenderGridAppearance &appearance)
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0 ||
        !initialize() ||
        !context_.makeCurrent(&surface_)) {
        return {};
    }

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    if (framebuffer_ == nullptr || framebuffer_->size() != pixelSize ||
        framebufferSampleRequest_ != antiAliasingSamples_) {
        framebuffer_.reset();
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setInternalTextureFormat(GL_RGBA8);
        format.setSamples(antiAliasingSamples_);
        framebuffer_ = std::make_unique<QOpenGLFramebufferObject>(pixelSize,
                                                                  format);
        framebufferSampleRequest_ = antiAliasingSamples_;
        if (!framebuffer_->isValid() && antiAliasingSamples_ > 0) {
            qWarning() << "Viewport MSAA" << antiAliasingSamples_
                       << "x unavailable; using single-sample rendering";
            format.setSamples(0);
            framebuffer_ = std::make_unique<QOpenGLFramebufferObject>(pixelSize,
                                                                      format);
        }
    }
    if (!framebuffer_->isValid() || !framebuffer_->bind()) {
        context_.doneCurrent();
        return {};
    }
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (antiAliasingSamples_ > 0) {
        glEnable(GL_MULTISAMPLE);
    } else {
        glDisable(GL_MULTISAMPLE);
    }

    const QByteArray depthGeometryKey =
        viewportDepthGeometryCacheKey(visibleSceneShapes);
    if (!depthGeometryCacheValid_ ||
        depthGeometryCacheKey_ != depthGeometryKey) {
        cachedDepthGeometry_ =
            buildViewportDepthGeometry(visibleSceneShapes);
        depthGeometryCacheKey_ = depthGeometryKey;
        depthGeometryCacheValid_ = true;
        uploadSceneDepthGeometry(cachedDepthGeometry_);
    }
    drawSceneDepth(cachedDepthGeometry_, transform, viewportSize, dpr);
    const bool rendered = drawGrid(transform,
                                   viewportSize,
                                   dpr,
                                   baseGridStep,
                                   appearance);
    QImage image = rendered ? framebuffer_->toImage(true) : QImage();
    framebuffer_->release();
    context_.doneCurrent();
    if (!image.isNull()) {
        image.setDevicePixelRatio(dpr);
    }
    return image;
}

void BlenderGridRenderer::drawSceneDepth(const ViewportDepthGeometry &geometry,
                                         const ViewportTransform &transform,
                                         const QSize &viewportSize,
                                         qreal devicePixelRatio)
{
    if (geometry.lineVertices.isEmpty() && geometry.pointVertices.isEmpty() &&
        geometry.surfaceVertices.isEmpty()) {
        return;
    }

    const QMatrix4x4 matrix = viewProjection(transform, viewportSize, nullptr);
    if (!sceneDepthProgram_.bind()) {
        return;
    }
    QOpenGLVertexArrayObject::Binder vaoBinder(&sceneDepthVertexArray_);
    sceneDepthVertexBuffer_.bind();
    sceneDepthProgram_.setUniformValue("uViewProjection", matrix);
    sceneDepthProgram_.setUniformValue("uPointSize",
                                      static_cast<float>(9.0 * devicePixelRatio));

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glLineWidth(static_cast<float>(std::max<qreal>(2.0 * devicePixelRatio, 1.0)));

    const auto drawVertices = [this](int first,
                                     int count,
                                     GLenum primitive,
                                     bool points) {
        if (count <= 0) {
            return;
        }
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,
                              3,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(QVector3D),
                              reinterpret_cast<const void *>(
                                  static_cast<quintptr>(first) *
                                  sizeof(QVector3D)));
        if (points) {
            glEnable(GL_PROGRAM_POINT_SIZE);
        }
        glDrawArrays(primitive, 0, count);
        if (points) {
            glDisable(GL_PROGRAM_POINT_SIZE);
        }
    };
    drawVertices(0, sceneDepthLineVertexCount_, GL_LINES, false);
    drawVertices(sceneDepthLineVertexCount_,
                 sceneDepthSurfaceVertexCount_,
                 GL_TRIANGLES,
                 false);
    drawVertices(sceneDepthLineVertexCount_ + sceneDepthSurfaceVertexCount_,
                 sceneDepthPointVertexCount_,
                 GL_POINTS,
                 true);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    sceneDepthVertexBuffer_.release();
    sceneDepthProgram_.release();
}

void BlenderGridRenderer::uploadSceneDepthGeometry(
    const ViewportDepthGeometry &geometry)
{
    sceneDepthLineVertexCount_ = geometry.lineVertices.size();
    sceneDepthSurfaceVertexCount_ = geometry.surfaceVertices.size();
    sceneDepthPointVertexCount_ = geometry.pointVertices.size();

    const int totalVertexCount = sceneDepthLineVertexCount_ +
                                 sceneDepthSurfaceVertexCount_ +
                                 sceneDepthPointVertexCount_;
    if (totalVertexCount == 0) {
        return;
    }

    QVector<QVector3D> combinedVertices;
    combinedVertices.reserve(totalVertexCount);
    combinedVertices += geometry.lineVertices;
    combinedVertices += geometry.surfaceVertices;
    combinedVertices += geometry.pointVertices;

    sceneDepthVertexBuffer_.bind();
    sceneDepthVertexBuffer_.allocate(
        combinedVertices.constData(),
        totalVertexCount * static_cast<int>(sizeof(QVector3D)));
    sceneDepthVertexBuffer_.release();
}

bool BlenderGridRenderer::drawGrid(const ViewportTransform &transform,
                                   const QSize &viewportSize,
                                   qreal devicePixelRatio,
                                   qreal baseGridStep,
                                   const BlenderGridAppearance &appearance)
{
    if (!initialized_) {
        return false;
    }

    const BlenderGridFrame gridFrame = resolveBlenderGridFrame(transform,
                                                               viewportSize);
    const bool perspective = transform.isPerspectiveEnabled();
    const int gridLineCount = perspective
                                  ? kPerspectiveGridLineCount
                                  : kOrthographicGridLineCount;
    const WorkPlane plane = gridFrame.plane;
    const Point3D planeOrigin = workPlanePointToWorld({},
                                                       plane,
                                                       gridFrame.planeOffset);
    const Point3D cameraOut = transform.viewDirection();
    const Point3D planeNormal = workPlaneNormal(plane);
    const QPointF focusPlanePosition = gridFrame.cameraRelativeOffset;
    const qreal gridFocusDistance = gridFrame.focusDistance;
    const BlenderGridLevelSelection gridLevel =
        selectBlenderGridLevel(gridFocusDistance,
                               gridFrame.fixedAxisOrthographic,
                               baseGridStep);

    const QVector3D axisU = asVector(subtract(
        workPlanePointToWorld(QPointF(1.0, 0.0), plane, gridFrame.planeOffset),
        planeOrigin));
    const QVector3D axisV = asVector(subtract(
        workPlanePointToWorld(QPointF(0.0, 1.0), plane, gridFrame.planeOffset),
        planeOrigin));
    const QVector3D normal = asVector(planeNormal);
    const QVector3D planeBase = asVector(planeOrigin);
    Point3D renderCamera;
    const QMatrix4x4 matrix = viewProjection(transform,
                                             viewportSize,
                                             &renderCamera);
    const QVector3D eye = asVector(renderCamera);
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QVector2D logicalViewport(static_cast<float>(viewportSize.width()),
                                    static_cast<float>(viewportSize.height()));

    if (!program_.bind()) {
        return false;
    }
    QOpenGLVertexArrayObject::Binder vaoBinder(&vertexArray_);
    program_.setUniformValue("uViewProjection", matrix);
    program_.setUniformValue("uGridOrigin", planeBase);
    program_.setUniformValue("uAxisU", axisU);
    program_.setUniformValue("uAxisV", axisV);
    program_.setUniformValue("uAxisVisibleX", gridFrame.visibleAxes[0] ? 1 : 0);
    program_.setUniformValue("uAxisVisibleY", gridFrame.visibleAxes[1] ? 1 : 0);
    program_.setUniformValue("uAxisVisibleZ", gridFrame.visibleAxes[2] ? 1 : 0);
    program_.setUniformValue("uPlaneNormal", normal);
    program_.setUniformValue("uCameraPosition", eye);
    program_.setUniformValue("uViewDirection", asVector(cameraOut));
    program_.setUniformValue("uPerspective", perspective ? 1 : 0);
    program_.setUniformValue("uZoom", static_cast<float>(transform.zoom()));
    program_.setUniformValue("uGridOffset",
                             QVector2D(static_cast<float>(focusPlanePosition.x()),
                                       static_cast<float>(focusPlanePosition.y())));
    program_.setUniformValue("uStepBase", static_cast<float>(gridLevel.baseStep));
    program_.setUniformValue("uBaseStepIndex", gridLevel.baseStepIndex);
    program_.setUniformValue("uStepCount", gridLevel.stepCount);
    program_.setUniformValue("uLevelFraction",
                             static_cast<float>(gridLevel.levelFraction));
    program_.setUniformValue("uGridLineCount", gridLineCount);
    program_.setUniformValue("uViewportSize", logicalViewport);
    program_.setUniformValue("uFarClipDistance",
                             static_cast<float>(transform.cameraPreferences().clipEnd));
    program_.setUniformValue("uGridColor",
                             colorVector(appearance.gridColor, appearance.opacity));
    program_.setUniformValue("uGridEmphasisColor",
                             colorVector(appearance.emphasisColor, appearance.opacity));
    program_.setUniformValue("uAxisColorX",
                             colorVector(appearance.axisXColor, appearance.opacity));
    program_.setUniformValue("uAxisColorY",
                             colorVector(appearance.axisYColor, appearance.opacity));
    program_.setUniformValue("uAxisColorZ",
                             colorVector(appearance.axisZColor, appearance.opacity));
    program_.setUniformValue("uStippleEnabled",
                             appearance.lowAlphaStipple ? 1 : 0);
    program_.setUniformValue("uStippleThreshold",
                             static_cast<float>(appearance.stippleThreshold));
    program_.setUniformValue("uStippleDashWidth",
                             static_cast<float>(appearance.stippleDashWidth));
    program_.setUniformValue("uGrazingFadeExponent",
                             static_cast<float>(appearance.grazingFadeExponent));
    program_.setUniformValue("uOrthographicEdgeFadeExponent",
                             static_cast<float>(appearance.orthographicEdgeFadeExponent));
    program_.setUniformValue("uFarFadeStart",
                             static_cast<float>(appearance.farFadeStart));
    program_.setUniformValue("uFarFadeEnd",
                             static_cast<float>(appearance.farFadeEnd));
    program_.setUniformValue("uDevicePixelRatio", static_cast<float>(dpr));

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    // Keep the transparent overlay premultiplied for QPainter composition:
    // accumulate RGB with source alpha, and accumulate coverage in alpha.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glLineWidth(static_cast<float>(dpr));
    const int gridVertexCount = 4 * kGridLevelsDrawn * gridLineCount;
    const int iterationCount = perspective ? kPerspectiveGridIterations : 1;
    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        glDepthMask(iteration == 0 ? GL_TRUE : GL_FALSE);
        program_.setUniformValue("uIteration", iteration);
        program_.setUniformValue("uMode", 0);
        glDrawArrays(GL_LINES, 0, gridVertexCount);
        program_.setUniformValue("uMode", 1);
        glDrawArrays(GL_LINES, 0, 6);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    program_.release();
    return true;
}

} // namespace classiCAD
