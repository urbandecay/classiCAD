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
    // Keep component identity matching in double precision as well; GPU point
    // coordinates are not precise enough to distinguish small CAD features.
    QVector<Point3D> precisePointVertices;
    QVector<QVector3D> surfaceVertices;
    // One smooth normal per expanded GL_TRIANGLES vertex.
    QVector<QVector3D> surfaceNormals;
    struct SurfaceBuildTiming {
        int faceIndex = -1;
        int degreeU = 0;
        int degreeV = 0;
        int controlVertexCountU = 0;
        int controlVertexCountV = 0;
        int trimLoopCount = 0;
        int sampledVertexCount = 0;
        int triangleCount = 0;
        int wireSegmentCount = 0;
        bool tessellationValid = false;
        PreparedNurbsSurfaceTessellation::Strategy tessellationStrategy =
            PreparedNurbsSurfaceTessellation::Strategy::Unprepared;
        SurfaceTessellationCache::AcquisitionStats::Path cachePath =
            SurfaceTessellationCache::AcquisitionStats::Path::None;
        qint64 acquisitionMicroseconds = 0;
        qint64 tessellationPrepareMicroseconds = 0;
        qint64 topologyUpdateMicroseconds = 0;
        PreparedNurbsSurfaceTessellation::PreparationStats preparation;
        qint64 wireframeMicroseconds = 0;
        qint64 normalPreparationMicroseconds = 0;
        qint64 triangleExpansionMicroseconds = 0;
    };
    QVector<SurfaceBuildTiming> surfaceBuildTimings;
};

ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<Shape> &visibleSceneShapes);
ViewportDepthGeometry buildViewportDepthGeometry(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    const SurfaceTessellationCache *surfaceTessellationCache = nullptr,
    bool applyOffsets = true);
ViewportDepthGeometry buildViewportDepthGeometry(
    const ViewportRenderObject &sceneObject,
    const SurfaceTessellationCache *surfaceTessellationCache = nullptr,
    bool collectSurfaceBuildTimings = false);
ViewportDepthGeometry buildViewportDepthGeometry(const Shape &shape);
QByteArray viewportDepthGeometryCacheKey(
    const QVector<Shape> &visibleSceneShapes);
QByteArray viewportDepthGeometryCacheKey(
    const QVector<ViewportRenderObject> &visibleSceneShapes,
    bool includeOffsets = true);

} // namespace classiCAD
