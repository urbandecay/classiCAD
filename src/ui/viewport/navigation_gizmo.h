/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"

#include <QPainter>
#include <QPointF>
#include <QSize>

namespace classiCAD {

enum class BlenderNavigationAction {
    None,
    Orbit,
    Axis,
    Zoom,
    Pan,
    Camera,
    Projection,
};

struct BlenderNavigationHit {
    BlenderNavigationAction action = BlenderNavigationAction::None;
    Point3D direction;
};

class ViewportNavigationGizmo final {
public:
    explicit ViewportNavigationGizmo(const ViewportTransform &transform);

    void draw(QPainter &painter,
              const QSize &viewportSize,
              const QPointF &hoverPosition) const;
    BlenderNavigationHit hitAt(const QPointF &screenPosition,
                              const QSize &viewportSize) const;

private:
    void drawContents(QPainter &painter,
                      const QSize &viewportSize,
                      const QPointF &hoverPosition) const;

    const ViewportTransform &transform_;
};

} // namespace classiCAD
