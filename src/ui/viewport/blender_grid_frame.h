/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "services/viewport/viewport_transform.h"

#include <QPointF>
#include <QSize>

#include <array>

namespace classiCAD {

struct BlenderGridFrame {
    WorkPlane plane = WorkPlane::XY;
    qreal planeOffset = 0.0;
    QPointF cameraRelativeOffset;
    qreal focusDistance = 1.0;
    bool fixedAxisOrthographic = false;
    // Blender's default axis display shows X and Y; Z is opt-in.
    std::array<bool, 3> visibleAxes{{true, true, false}};
};

BlenderGridFrame resolveBlenderGridFrame(const ViewportTransform &transform,
                                         const QSize &viewportSize);

} // namespace classiCAD
