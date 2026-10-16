#pragma once

#include "work_plane.h"

#include <QVector>
#include <QVector4D>

#include <cmath>

namespace classiCAD {

// A Blender Workbench point cloud remains one scene object. Positions are
// world-space XYZ after import transforms; radii and source indices remain
// paired with each point. Colors are optional per-point scene-linear RGBA.
struct PointCloud3D {
    QVector<Point3D> positions;
    QVector<qreal> radii;
    QVector<QVector4D> colors;
    QVector<int> sourceIndices;
};

inline bool validatePointCloud(const PointCloud3D &cloud)
{
    if (cloud.positions.isEmpty() || cloud.positions.size() > 1'000'000 ||
        cloud.radii.size() != cloud.positions.size() ||
        (!cloud.colors.isEmpty() &&
         cloud.colors.size() != cloud.positions.size()) ||
        (!cloud.sourceIndices.isEmpty() &&
         cloud.sourceIndices.size() != cloud.positions.size())) return false;
    for (qsizetype index = 0; index < cloud.positions.size(); ++index) {
        const Point3D &position = cloud.positions[index];
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) ||
            !std::isfinite(cloud.radii[index]) || cloud.radii[index] < 0.0)
            return false;
        if (!cloud.colors.isEmpty()) {
            const QVector4D &color = cloud.colors[index];
            if (!std::isfinite(color.x()) || !std::isfinite(color.y()) ||
                !std::isfinite(color.z()) || !std::isfinite(color.w()))
                return false;
        }
        if (!cloud.sourceIndices.isEmpty() && cloud.sourceIndices[index] < 0)
            return false;
    }
    return true;
}

} // namespace classiCAD
