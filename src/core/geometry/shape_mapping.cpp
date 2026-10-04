#include "shape_mapping.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/work_plane.h"

#include <cmath>

namespace classiCAD {
namespace {

QVector<QPointF> rectangleVerticesForShape(const Shape &shape)
{
    if (shape.geometryType != GeometryType::Rectangle ||
        shape.points.size() < 2) {
        return {};
    }
    if (validateNurbsCurve(shape.nurbs) &&
        shape.nurbs.controlPoints.size() == 5) {
        return shape.nurbs.controlPoints.mid(0, 4);
    }
    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1],
                shape.points[2], shape.points[3]};
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

} // namespace

bool isClosedPolygonNurbs(const Shape::NurbsCurve2D &curve)
{
    if (!validateNurbsCurve(curve) || curve.degree != 1 ||
        curve.controlPoints.size() < 4) {
        return false;
    }
    const QPointF closure = curve.controlPoints.first() -
                            curve.controlPoints.last();
    return std::hypot(closure.x(), closure.y()) <= 1.0e-9;
}

WorkPlaneFrame shapeWorkPlaneFrame(const Shape &shape)
{
    return isValidWorkPlaneFrame(shape.workPlaneFrame)
               ? shape.workPlaneFrame
               : makeWorkPlaneFrame(shape.workPlane, shape.workPlaneOffset);
}

WorkPlaneFrame shapeComponentWorkPlaneFrame(const Shape &shape,
                                            int componentIndex)
{
    if (componentIndex >= 0 &&
        componentIndex < shape.componentWorkPlaneFrames.size() &&
        isValidWorkPlaneFrame(shape.componentWorkPlaneFrames[componentIndex])) {
        return shape.componentWorkPlaneFrames[componentIndex];
    }
    return shapeWorkPlaneFrame(shape);
}

Point3D shapePointToWorld(const Shape &shape, const QPointF &point)
{
    return workPlaneFramePointToWorld(point, shapeWorkPlaneFrame(shape));
}

Point3D shapeComponentPointToWorld(const Shape &shape,
                                   int componentIndex,
                                   const QPointF &point)
{
    return workPlaneFramePointToWorld(
        point, shapeComponentWorkPlaneFrame(shape, componentIndex));
}

QPointF shapeWorldPointToLocal(const Shape &shape, const Point3D &point)
{
    return worldPointToWorkPlaneFrame(point, shapeWorkPlaneFrame(shape));
}

QVector<QPointF> polygonVerticesForShape(const Shape &shape)
{
    if (shape.geometryType != GeometryType::Polygon ||
        !isClosedPolygonNurbs(shape.nurbs)) {
        return {};
    }
    QVector<QPointF> vertices = shape.nurbs.controlPoints;
    vertices.removeLast();
    return vertices.size() >= 3 ? vertices : QVector<QPointF>{};
}

QVector<ShapeNurbsCurveComponent> nurbsCurveComponentsForShape(
    const Shape &shape)
{
    QVector<ShapeNurbsCurveComponent> components;
    if (shape.geometryType == GeometryType::PolyCurve) {
        components.reserve(shape.components.size());
        for (int index = 0; index < shape.components.size(); ++index) {
            components.append({index,
                               shape.components[index],
                               shapeComponentWorkPlaneFrame(shape, index)});
        }
        return components;
    }

    const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
    if (validateNurbsCurve(shape.nurbs)) {
        components.append({0, shape.nurbs, frame});
        return components;
    }

    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        components.append({0, makeDegreeOneNurbs(shape.points), frame});
        return components;
    }

    if ((shape.geometryType == GeometryType::Bezier ||
         shape.geometryType == GeometryType::Nurbs) &&
        shape.points.size() >= 2) {
        components.append({0, makeBezierNurbs(shape.points), frame});
        return components;
    }

    QVector<QPointF> vertices;
    if (shape.geometryType == GeometryType::Polygon) {
        vertices = polygonVerticesForShape(shape);
    } else if (shape.geometryType == GeometryType::Rectangle) {
        vertices = rectangleVerticesForShape(shape);
    }
    const int minimumVertices = shape.geometryType == GeometryType::Polygon
                                    ? 3
                                    : 4;
    if (vertices.size() < minimumVertices) {
        return components;
    }
    components.reserve(vertices.size());
    for (int index = 0; index < vertices.size(); ++index) {
        components.append({index,
                           makeDegreeOneNurbs(
                               {vertices[index],
                                vertices[(index + 1) % vertices.size()]}),
                           frame});
    }
    return components;
}

QVector<QPointF> pictureFrameCorners(const Shape &shape)
{
    return shape.geometryType == GeometryType::Picture && shape.points.size() == 4
               ? shape.points
               : QVector<QPointF>{};
}

} // namespace classiCAD
