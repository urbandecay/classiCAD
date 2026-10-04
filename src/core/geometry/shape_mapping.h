#pragma once

#include "core/document/shape.h"

namespace classiCAD {

struct ShapeNurbsCurveComponent {
    int componentIndex = -1;
    Shape::NurbsCurve2D curve;
    WorkPlaneFrame workPlaneFrame;
};

// Maps the local XY coordinates stored by a shape through its work plane.
WorkPlaneFrame shapeWorkPlaneFrame(const Shape &shape);
WorkPlaneFrame shapeComponentWorkPlaneFrame(const Shape &shape,
                                            int componentIndex);
Point3D shapePointToWorld(const Shape &shape, const QPointF &point);
Point3D shapeComponentPointToWorld(const Shape &shape,
                                  int componentIndex,
                                  const QPointF &point);
QPointF shapeWorldPointToLocal(const Shape &shape, const Point3D &point);

QVector<QPointF> polygonVerticesForShape(const Shape &shape);
QVector<ShapeNurbsCurveComponent> nurbsCurveComponentsForShape(
    const Shape &shape);
QVector<QPointF> pictureFrameCorners(const Shape &shape);
bool isClosedPolygonNurbs(const Shape::NurbsCurve2D &curve);

} // namespace classiCAD
