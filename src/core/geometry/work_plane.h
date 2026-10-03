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

// Curves stay as local 2D NURBS. A frame supplies the local XY basis and its
// placement in world space, so drawing tools can share one plane-aware input
// path without introducing a separate spatial-curve representation.
struct WorkPlaneFrame {
    Point3D origin;
    Point3D xAxis{1.0, 0.0, 0.0};
    Point3D yAxis{0.0, 1.0, 0.0};
    Point3D normal{0.0, 0.0, 1.0};
    bool valid = false;
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

WorkPlaneFrame makeWorkPlaneFrame(WorkPlane plane, qreal offset = 0.0);
WorkPlaneFrame makeWorkPlaneFrameFromNormal(const Point3D &origin,
                                            const Point3D &normal,
                                            const Point3D &preferredXAxis =
                                                {1.0, 0.0, 0.0});
bool isValidWorkPlaneFrame(const WorkPlaneFrame &frame);
Point3D workPlaneFramePointToWorld(const QPointF &point,
                                   const WorkPlaneFrame &frame);
QPointF worldPointToWorkPlaneFrame(const Point3D &point,
                                   const WorkPlaneFrame &frame);
qreal signedDistanceFromWorkPlaneFrame(const Point3D &point,
                                       const WorkPlaneFrame &frame);
bool workPlaneFramesMatch(const WorkPlaneFrame &first,
                          const WorkPlaneFrame &second);
bool workPlaneFramesCoplanar(const WorkPlaneFrame &first,
                             const WorkPlaneFrame &second,
                             qreal distanceTolerance = 1.0e-7);

inline bool workPlaneMatches(WorkPlane firstPlane,
                             qreal firstOffset,
                             WorkPlane secondPlane,
                             qreal secondOffset)
{
    const qreal scale = std::max<qreal>({1.0, std::abs(firstOffset), std::abs(secondOffset)});
    return firstPlane == secondPlane &&
           std::abs(firstOffset - secondOffset) <= 1.0e-9 * scale;
}

inline bool workPlaneMatches(const WorkPlaneFrame &first,
                             const WorkPlaneFrame &second)
{
    return workPlaneFramesMatch(first, second);
}

} // namespace classiCAD
