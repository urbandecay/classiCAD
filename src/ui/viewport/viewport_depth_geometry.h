/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/shape.h"
#include "core/document/object_id.h"
#include "services/sampling/surface_tessellation_cache.h"
#include "viewport_render_frame.h"

#include <QVector>
#include <QVector3D>
#include <QByteArray>

namespace classiCAD {

struct ViewportDepthGeometry {
    // Vertex arrays are already expanded for GL_LINES/GL_POINTS/GL_TRIANGLES.
    QVector<QVector3D> lineVertices;
    // Preserve double-precision world samples for the CPU painter fallback.
    // QVector3D is a GPU upload format and is too lossy for large CAD scenes.
    QVector<Point3D> preciseLineVertices;
    QVector<QVector3D> pointVertices;
    QVector<QVector3D> surfaceVertices;
};

ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<Shape> &visibleSceneShapes);
ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    const SurfaceTessellationCache *surfaceTessellationCache = nullptr,
    bool applyOffsets = true);
ViewportDepthGeometry buildViewportDepthGeometry(
    const ViewportRenderObject &sceneObject,
    const SurfaceTessellationCache *surfaceTessellationCache = nullptr);
ViewportDepthGeometry buildViewportDepthGeometry(const Shape &shape);
QByteArray viewportDepthGeometryCacheKey(
    const QVector<Shape> &visibleSceneShapes);
QByteArray viewportDepthGeometryCacheKey(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    bool includeOffsets = true);

} // namespace classiCAD
