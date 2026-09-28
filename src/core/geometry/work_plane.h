#pragma once

#include <QPointF>
#include <QString>

#include <algorithm>
#include <cmath>

namespace classiCAD {

// Curves remain exact 2D NURBS in their local construction plane. This plane
// maps that local XY data into the application's right-handed world XYZ.
enum class WorkPlane {
    XY = 0,
    XZ = 1,
    YZ = 2,
};

struct Point3D {
    qreal x = 0.0;
    qreal y = 0.0;
    qreal z = 0.0;
};

bool workPlaneFromValue(int value, WorkPlane *plane);
QString workPlaneName(WorkPlane plane);
Point3D workPlanePointToWorld(const QPointF &point,
                              WorkPlane plane,
                              qreal offset = 0.0);
QPointF worldPointToWorkPlane(const Point3D &point, WorkPlane plane);
qreal signedDistanceFromWorkPlane(const Point3D &point,
                                  WorkPlane plane,
                                  qreal offset = 0.0);
Point3D workPlaneNormal(WorkPlane plane);

inline bool workPlaneMatches(WorkPlane firstPlane,
                             qreal firstOffset,
                             WorkPlane secondPlane,
                             qreal secondOffset)
{
    const qreal scale = std::max<qreal>({1.0, std::abs(firstOffset), std::abs(secondOffset)});
    return firstPlane == secondPlane &&
           std::abs(firstOffset - secondOffset) <= 1.0e-9 * scale;
}

} // namespace classiCAD
