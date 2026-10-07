/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_gpu_surface.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QSurfaceFormat>

namespace classiCAD {

ViewportGpuSurface::ViewportGpuSurface(QWidget *parent)
    : QOpenGLWidget(parent)
    , gridRenderer_(std::make_unique<BlenderGridRenderer>())
    , sceneRenderer_(std::make_unique<ViewportSceneRenderer>())
    , previewRenderer_(std::make_unique<ViewportSceneRenderer>())
    , controlPointRenderer_(std::make_unique<ViewportControlPointRenderer>())
    , surfaceRenderer_(std::make_unique<ViewportSurfaceRenderer>())
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
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

void ViewportGpuSurface::setAntiAliasingSamples(int samples)
{
    gridRenderer_->setAntiAliasingSamples(samples);
    update();
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
        drawCallback_(painter, *gridRenderer_, *sceneRenderer_, *previewRenderer_,
                      *controlPointRenderer_, *surfaceRenderer_);
    }
}

} // namespace classiCAD
