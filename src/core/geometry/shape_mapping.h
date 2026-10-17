#pragma once

#include "core/document/shape.h"

namespace classiCAD {

struct ShapeNurbsCurveComponent {
    int componentIndex = -1;
    Shape::NurbsCurve2D curve;
    WorkPlaneFrame workPlaneFrame;
};

struct ShapeCurveAppendRange {
    int firstComponentIndex = -1;
    int componentCount = 0;
    int firstControlPointIndex = -1;
    int controlPointCount = 0;
};

// Maps local curve (u, v, w) coordinates through the work-plane frame. Basic
// point geometry continues to use only local (u, v).
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
// Append point or curve geometry to a curve object as independent NURBS
// components. Surface and solid additions are rejected because converting
// their boundaries to curves would change their geometry type. The frame
// translation converts incoming component frames into the destination
// object's local space when required.
bool appendShapeCurveComponents(Shape *destination,
                                const Shape &addition,
                                const Point3D &frameTranslation = {},
                                ShapeCurveAppendRange *appendedRange = nullptr);
QVector<QPointF> pictureFrameCorners(const Shape &shape);
bool isClosedPolygonNurbs(const Shape::NurbsCurve2D &curve);

} // namespace classiCAD
