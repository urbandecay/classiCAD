/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "viewport_shading.h"
#include "viewport_render_frame.h"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

namespace classiCAD {

// Draws display triangles derived from exact NURBS surfaces. The saved
// NurbsSurface3D remains authoritative; this renderer consumes the cached
// tessellation shared with hit testing and scene-depth rendering.
class ViewportSurfaceRenderer final : protected QOpenGLFunctions_3_3_Core {
public:
    ~ViewportSurfaceRenderer();

    bool draw(const QVector<ViewportRenderObject> &objects,
              const ViewportTransform &transform,
              const QSize &viewportSize,
              qreal devicePixelRatio,
              const ViewportShadingSettings &settings,
              bool previewOverlay = false,
              bool clearDepth = true);

private:
    struct SurfaceVertex {
        float x;
        float y;
        float z;
        float nx;
        float ny;
        float nz;
    };

    struct DrawRange {
        int first = 0;
        int count = 0;
        Point3D offset;
        bool selected = false;
    };

    bool initialize();
    void prepareGeometry(const QVector<ViewportRenderObject> &objects);

    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QVector<SurfaceVertex> vertices_;
    QVector<DrawRange> ranges_;
    QByteArray geometryKey_;
    bool geometryDirty_ = true;
    bool initializationAttempted_ = false;
    bool initialized_ = false;
};

} // namespace classiCAD
