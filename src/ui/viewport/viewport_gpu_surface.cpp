/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_gpu_surface.h"

#include <QColorSpace>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QSurfaceFormat>

namespace classiCAD {

ViewportGpuSurface::ViewportGpuSurface(QWidget *parent)
    : QOpenGLWidget(parent)
    , gridRenderer_(std::make_unique<BlenderGridRenderer>())
    , aaRenderer_(std::make_unique<ViewportAaRenderer>())
    , sceneRenderer_(std::make_unique<ViewportSceneRenderer>())
    , previewRenderer_(std::make_unique<ViewportSceneRenderer>())
    , controlPointRenderer_(std::make_unique<ViewportControlPointRenderer>())
    , surfaceRenderer_(std::make_unique<ViewportSurfaceRenderer>())
{
    // The resolved viewport contains display-sRGB bytes. With a single-sample
    // widget Qt samples this texture directly during window composition; an
    // sRGB texture would decode those bytes and darken the entire viewport.
    // Keep sRGB decoding in the AA scene target, not the Qt presentation target.
    setTextureFormat(GL_RGBA8);
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setColorSpace(QColorSpace(QColorSpace::SRgb));
    format.setDepthBufferSize(24);
    // The AA scene target has its own stencil attachment. Request one for the
    // non-AA widget framebuffer so Workbench shadow volumes work in both paths.
    format.setStencilBufferSize(8);
    // AA is resolved by ViewportAaRenderer. A multisampled widget FBO cannot
    // receive a depth blit from its single-sample Workbench scene target.
    format.setSamples(0);
    setFormat(format);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
}

ViewportGpuSurface::~ViewportGpuSurface()
{
    if (isValid()) {
        makeCurrent();
    }
    controlPointRenderer_.reset();
    surfaceRenderer_.reset();
    previewRenderer_.reset();
    sceneRenderer_.reset();
    aaRenderer_.reset();
    gridRenderer_.reset();
    if (isValid()) {
        doneCurrent();
    }
}

bool ViewportGpuSurface::isSupported()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("offscreen") ||
        platform == QStringLiteral("minimal")) {
        return false;
    }
    QOpenGLContext context;
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    context.setFormat(format);
    if (!context.create()) {
        return false;
    }
    const QSurfaceFormat actualFormat = context.format();
    if (context.isOpenGLES() || actualFormat.majorVersion() < 3 ||
        (actualFormat.majorVersion() == 3 &&
         actualFormat.minorVersion() < 3)) {
        return false;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        return false;
    }
    context.doneCurrent();
    return true;
}

void ViewportGpuSurface::setDrawCallback(DrawCallback callback)
{
    drawCallback_ = std::move(callback);
    update();
}

void ViewportGpuSurface::setDrawCallback(LegacyDrawCallback callback)
{
    if (!callback) {
        drawCallback_ = {};
        update();
        return;
    }
    drawCallback_ = [legacy = std::move(callback)](
                        QPainter &painter,
                        BlenderGridRenderer &gridRenderer,
                        ViewportAaRenderer &,
                        ViewportSceneRenderer &sceneRenderer,
                        ViewportSceneRenderer &previewRenderer,
                        ViewportControlPointRenderer &controlPointRenderer,
                        ViewportSurfaceRenderer &surfaceRenderer) {
        legacy(painter, gridRenderer, sceneRenderer, previewRenderer,
               controlPointRenderer, surfaceRenderer);
    };
    update();
}

void ViewportGpuSurface::setAntiAliasingMode(int mode)
{
    aaRenderer_->setMode(mode);
    update();
}

void ViewportGpuSurface::setAntiAliasingSamples(int samples)
{
    const int mode = samples == 2 ? 1 : samples == 4 ? 5 : samples;
    setAntiAliasingMode(mode);
}

void ViewportGpuSurface::setSurfaceTessellationCache(
    const SurfaceTessellationCache *surfaceTessellationCache)
{
    gridRenderer_->setSurfaceTessellationCache(surfaceTessellationCache);
}

bool ViewportGpuSurface::pickScenePoint(
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    Point3D *worldPoint)
{
    if (!isValid() || worldPoint == nullptr) {
        return false;
    }
    makeCurrent();
    const bool picked = gridRenderer_->pickScenePoint(
        screenPosition, transform, viewportSize, devicePixelRatioF(),
        visibleSceneShapes, worldPoint);
    doneCurrent();
    return picked;
}

bool ViewportGpuSurface::pickComponentElement(
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    const QVector<ViewportComponentPickCandidate> &candidates,
    int *candidateIndex)
{
    if (!isValid() || candidateIndex == nullptr) {
        return false;
    }
    makeCurrent();
    const bool picked = gridRenderer_->pickComponentElement(
        screenPosition, transform, viewportSize, devicePixelRatioF(),
        visibleSceneShapes, candidates, candidateIndex);
    doneCurrent();
    return picked;
}

bool ViewportGpuSurface::pickComponentElementsInRect(
    const QRectF &selectionRect,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    const QVector<ViewportComponentPickCandidate> &candidates,
    bool depthTest,
    QVector<int> *candidateIndices)
{
    if (!isValid() || candidateIndices == nullptr) {
        return false;
    }
    makeCurrent();
    const bool picked = gridRenderer_->pickComponentElementsInRect(
        selectionRect, transform, viewportSize, devicePixelRatioF(),
        visibleSceneShapes, candidates, depthTest, candidateIndices);
    doneCurrent();
    return picked;
}

bool ViewportGpuSurface::pickScenePoint(
    const QPointF &screenPosition,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const QVector<Shape> &visibleSceneShapes,
    Point3D *worldPoint)
{
    QVector<ViewportRenderObject> depthShapes;
    depthShapes.reserve(visibleSceneShapes.size());
    for (const Shape &shape : visibleSceneShapes) {
        ViewportRenderObject entry;
        entry.shape = shape;
        entry.cacheable = false;
        depthShapes.append(std::move(entry));
    }
    return pickScenePoint(screenPosition,
                          transform,
                          viewportSize,
                          depthShapes,
                          worldPoint);
}

void ViewportGpuSurface::paintGL()
{
    if (drawCallback_) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        drawCallback_(painter, *gridRenderer_, *aaRenderer_, *sceneRenderer_, *previewRenderer_,
                      *controlPointRenderer_, *surfaceRenderer_);
    }
}

} // namespace classiCAD
