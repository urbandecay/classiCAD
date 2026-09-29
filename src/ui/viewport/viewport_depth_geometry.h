/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/model.h"

#include <QVector>
#include <QVector3D>
#include <QByteArray>

namespace classiCAD {

struct ViewportDepthGeometry {
    // Vertex arrays are already expanded for GL_LINES/GL_POINTS/GL_TRIANGLES.
    QVector<QVector3D> lineVertices;
    QVector<QVector3D> pointVertices;
    QVector<QVector3D> surfaceVertices;
};

ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<Shape> &visibleSceneShapes);
QByteArray viewportDepthGeometryCacheKey(
    const QVector<Shape> &visibleSceneShapes);

} // namespace classiCAD
