/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "blender_grid_renderer.h"
#include "viewport_control_point_renderer.h"
#include "viewport_scene_renderer.h"

#include <QOpenGLWidget>

#include <functional>
#include <memory>

class QPainter;

namespace classiCAD {

// Presents the viewport in Qt's on-screen OpenGL framebuffer. The owning
// widget retains input/tool state and the raster fallback.
class ViewportGpuSurface final : public QOpenGLWidget {
public:
    using DrawCallback = std::function<void(QPainter &, BlenderGridRenderer &,
                                            ViewportSceneRenderer &,
                                            ViewportSceneRenderer &,
                                            ViewportControlPointRenderer &)>;

    explicit ViewportGpuSurface(QWidget *parent = nullptr);
    ~ViewportGpuSurface() override;

    static bool isSupported();
    void setDrawCallback(DrawCallback callback);
    void setAntiAliasingSamples(int samples);
    bool pickScenePoint(const QPointF &screenPosition,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        const QVector<Shape> &visibleSceneShapes,
                        Point3D *worldPoint);

protected:
    void paintGL() override;

private:
    DrawCallback drawCallback_;
    std::unique_ptr<BlenderGridRenderer> gridRenderer_;
    std::unique_ptr<ViewportSceneRenderer> sceneRenderer_;
    std::unique_ptr<ViewportSceneRenderer> previewRenderer_;
    std::unique_ptr<ViewportControlPointRenderer> controlPointRenderer_;
};

} // namespace classiCAD
