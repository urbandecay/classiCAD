/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/shape.h"

#include <QColor>

namespace classiCAD {

// The current classiCAD viewport is its editable drawing view. Blender 5.2.2
// draws legacy curve edit wires from TH_WIRE_EDIT; its default 3D View theme
// sets that color to opaque black. Keep this separate from Workbench's fill
// color sources.
inline QColor viewportEditWireThemeColor()
{
    return QColor(0, 0, 0);
}

inline bool isViewportCurveStrokeType(GeometryType type) noexcept
{
    switch (type) {
    case GeometryType::Point:
    case GeometryType::Line:
    case GeometryType::Rectangle:
    case GeometryType::Polygon:
    case GeometryType::Circle:
    case GeometryType::Ellipse:
    case GeometryType::Arc:
    case GeometryType::PolyCurve:
    case GeometryType::Bezier:
    case GeometryType::Nurbs:
        return true;
    default:
        return false;
    }
}

} // namespace classiCAD
