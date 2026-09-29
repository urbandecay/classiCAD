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
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    setFormat(format);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
}

ViewportGpuSurface::~ViewportGpuSurface()
{
    if (isValid()) {
        makeCurrent();
    }
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

void ViewportGpuSurface::paintGL()
{
    if (drawCallback_) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        drawCallback_(painter, *gridRenderer_, *sceneRenderer_);
    }
}

} // namespace classiCAD
