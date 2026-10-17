#include "shape_mapping.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_surface.h"
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
    const qreal normalClosure = curve.dimension == 3
        ? curve.normalCoordinates.first() - curve.normalCoordinates.last()
        : 0.0;
    return std::hypot(std::hypot(closure.x(), closure.y()),
                      normalClosure) <= 1.0e-9;
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

bool appendShapeCurveComponents(Shape *destination,
                                const Shape &addition,
                                const Point3D &frameTranslation,
                                ShapeCurveAppendRange *appendedRange)
{
    const auto finitePoint = [](const Point3D &point) {
        return std::isfinite(point.x) && std::isfinite(point.y) &&
               std::isfinite(point.z);
    };
    if (destination == nullptr || !finitePoint(frameTranslation)) {
        return false;
    }
    if (destination->geometryType == GeometryType::NurbsSurface ||
        destination->geometryType == GeometryType::NurbsSolid ||
        destination->geometryType == GeometryType::PolygonMesh ||
        destination->geometryType == GeometryType::ScalarVolume ||
        destination->geometryType == GeometryType::PointCloud) {
        return false;
    }
    if (addition.geometryType == GeometryType::NurbsSurface ||
        addition.geometryType == GeometryType::NurbsSolid ||
        addition.geometryType == GeometryType::PolygonMesh ||
        addition.geometryType == GeometryType::ScalarVolume ||
        addition.geometryType == GeometryType::PointCloud) {
        return false;
    }

    const auto pointComponent = [](const Shape &shape,
                                   ShapeNurbsCurveComponent *component) {
        if (component == nullptr || shape.geometryType != GeometryType::Point ||
            shape.points.size() != 1) {
            return false;
        }
        const WorkPlaneFrame frame = shapeWorkPlaneFrame(shape);
        if (!isValidWorkPlaneFrame(frame)) {
            return false;
        }
        component->componentIndex = 0;
        component->curve = makeDegreeOneNurbs(
            {shape.points.first(), shape.points.first()});
        component->workPlaneFrame = frame;
        return validateNurbsCurve(component->curve);
    };

    const auto componentsForAppend = [&pointComponent](const Shape &shape) {
        QVector<ShapeNurbsCurveComponent> components;
        if (shape.geometryType == GeometryType::Point) {
            ShapeNurbsCurveComponent component;
            if (pointComponent(shape, &component)) {
                components.append(std::move(component));
            }
            return components;
        }
        return nurbsCurveComponentsForShape(shape);
    };

    QVector<ShapeNurbsCurveComponent> existing =
        componentsForAppend(*destination);
    QVector<ShapeNurbsCurveComponent> added = componentsForAppend(addition);
    if (existing.isEmpty() || added.isEmpty()) {
        return false;
    }

    Shape result = *destination;
    QVector<Shape::NurbsCurve3D> curves;
    QVector<WorkPlaneFrame> frames;
    curves.reserve(existing.size() + added.size());
    frames.reserve(existing.size() + added.size());
    int existingControlPointCount = 0;
    for (ShapeNurbsCurveComponent &component : existing) {
        if (!validateNurbsCurve(component.curve) ||
            !isValidWorkPlaneFrame(component.workPlaneFrame)) {
            return false;
        }
        curves.append(std::move(component.curve));
        frames.append(component.workPlaneFrame);
        existingControlPointCount += curves.last().controlPoints.size();
    }

    ShapeCurveAppendRange range;
    range.firstComponentIndex = curves.size();
    range.firstControlPointIndex = existingControlPointCount;
    for (ShapeNurbsCurveComponent &component : added) {
        if (!validateNurbsCurve(component.curve) ||
            !isValidWorkPlaneFrame(component.workPlaneFrame)) {
            return false;
        }
        component.workPlaneFrame.origin.x += frameTranslation.x;
        component.workPlaneFrame.origin.y += frameTranslation.y;
        component.workPlaneFrame.origin.z += frameTranslation.z;
        if (!isValidWorkPlaneFrame(component.workPlaneFrame)) {
            return false;
        }
        range.controlPointCount += component.curve.controlPoints.size();
        curves.append(std::move(component.curve));
        frames.append(component.workPlaneFrame);
    }
    range.componentCount = curves.size() - range.firstComponentIndex;

    result.geometryType = GeometryType::PolyCurve;
    result.nurbs = Shape::NurbsCurve3D{};
    result.components = curves;
    result.componentWorkPlaneFrames = frames;
    result.points.clear();
    result.arcMode = ArcMode::TwoPoint;
    result.arcSweep = 0.0;
    result.subdivisionParameters.clear();

    for (const Shape::NurbsCurve3D &curve : curves) {
        QPointF start;
        QPointF end;
        if (!nurbsCurveEndpoints(curve, &start, &end)) {
            return false;
        }
        if (result.points.isEmpty()) {
            result.points.append(start);
        }
        result.points.append(end);
    }

    QVector<quint64> weldGroups = destination->controlPointWeldGroups;
    if (weldGroups.size() > existingControlPointCount) {
        weldGroups.resize(existingControlPointCount);
    } else if (weldGroups.size() < existingControlPointCount) {
        weldGroups.resize(existingControlPointCount);
        for (int index = destination->controlPointWeldGroups.size();
             index < weldGroups.size(); ++index) {
            weldGroups[index] = 0;
        }
    }
    int additionControlPointCount = 0;
    for (int index = range.firstComponentIndex; index < curves.size(); ++index) {
        additionControlPointCount += curves[index].controlPoints.size();
    }
    const bool additionGroupsMatch =
        addition.geometryType != GeometryType::NurbsSurface &&
        addition.geometryType != GeometryType::NurbsSolid &&
        addition.controlPointWeldGroups.size() == additionControlPointCount;
    if (additionGroupsMatch) {
        for (const quint64 group : addition.controlPointWeldGroups) {
            weldGroups.append(group);
        }
    } else {
        weldGroups.resize(existingControlPointCount + additionControlPointCount);
        for (int index = existingControlPointCount;
             index < weldGroups.size(); ++index) {
            weldGroups[index] = 0;
        }
    }
    result.controlPointWeldGroups = std::move(weldGroups);

    QVector<int> displayResolutions;
    if (destination->geometryType == GeometryType::PolyCurve &&
        destination->componentCurveDisplayResolutionsU.size() == existing.size()) {
        displayResolutions = destination->componentCurveDisplayResolutionsU;
    } else {
        displayResolutions.fill(destination->curveDisplayResolutionU,
                                existing.size());
    }
    const QVector<int> addedDisplayResolutions =
        addition.geometryType == GeometryType::PolyCurve &&
                addition.componentCurveDisplayResolutionsU.size() == added.size()
            ? addition.componentCurveDisplayResolutionsU
            : QVector<int>(added.size(), addition.curveDisplayResolutionU);
    displayResolutions += addedDisplayResolutions;
    result.componentCurveDisplayResolutionsU = std::move(displayResolutions);
    result.curveDisplayResolutionU = 0;

    *destination = std::move(result);
    if (appendedRange != nullptr) {
        *appendedRange = range;
    }
    return true;
}

QVector<QPointF> pictureFrameCorners(const Shape &shape)
{
    return shape.geometryType == GeometryType::Picture && shape.points.size() == 4
               ? shape.points
               : QVector<QPointF>{};
}

} // namespace classiCAD
