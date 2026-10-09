#pragma once

#include "core/geometry/work_plane.h"

#include <QPointF>
#include <QString>

namespace classiCAD {

enum class SnapType {
    None,
    Endpoint,
    Midpoint,
    Intersection,
    Center,
    Perpendicular,
    Tangent,
    ControlPoint,
    Near,
};

struct SnapResult {
    SnapType type = SnapType::None;
    QPointF point;
    Point3D worldPoint;
    bool hasWorldPoint = false;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

struct SnapCandidate {
    SnapCandidate() = default;
    SnapCandidate(SnapType snapType,
                  const QPointF &localPoint,
                  int sourceShapeIndex = -1,
                  int sourceComponentIndex = -1)
        : type(snapType)
        , point(localPoint)
        , shapeIndex(sourceShapeIndex)
        , componentIndex(sourceComponentIndex)
    {
    }

    SnapType type = SnapType::None;
    QPointF point;
    int shapeIndex = -1;
    int componentIndex = -1;
    int controlPointIndex = -1;
    Point3D worldPoint;
    bool hasWorldPoint = false;
};

struct DragSnapResult {
    SnapType type = SnapType::None;
    QPointF sourcePoint;
    QPointF targetPoint;
    QPointF translation;
    int targetShapeIndex = -1;
    int targetComponentIndex = -1;
    Point3D worldSourcePoint;
    Point3D worldTargetPoint;
    Point3D worldTranslation;
    bool hasWorldTranslation = false;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

QString snapTypeName(SnapType type);

} // namespace classiCAD
