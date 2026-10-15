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
    // Axes that lie in the resolved grid plane. Saved overlay flags decide
    // which of these axes are shown.
    std::array<bool, 3> visibleAxes{{true, true, false}};
};

BlenderGridFrame resolveBlenderGridFrame(const ViewportTransform &transform,
                                         const QSize &viewportSize);

} // namespace classiCAD
