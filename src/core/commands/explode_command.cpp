#include "explode_command.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/shape_mapping.h"

namespace classiCAD {
namespace {

QVector<QPointF> rectangleVertices(const Shape &shape)
{
    if (shape.geometryType == GeometryType::Rectangle &&
        validateNurbsCurve(shape.nurbs) &&
        shape.nurbs.controlPoints.size() == 5) {
        return shape.nurbs.controlPoints.mid(0, 4);
    }
    if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
        return {};
    }
    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

QVector<QPointF> polyCurvePoints(
    const QVector<Shape::NurbsCurve2D> &components)
{
    const auto endpoints = [](const Shape::NurbsCurve2D &curve,
                              QPointF *start,
                              QPointF *end) {
        if (!validateNurbsCurve(curve) || (start == nullptr && end == nullptr)) {
            return false;
        }
        const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
        if (fullKnots.size() <= curve.controlPoints.size()) {
            return false;
        }
        return (start == nullptr ||
                evaluateNurbsPoint(curve, fullKnots[curve.degree], start)) &&
               (end == nullptr || evaluateNurbsPoint(
                                      curve,
                                      fullKnots[curve.controlPoints.size()],
                                      end));
    };
    QVector<QPointF> points;
    for (int index = 0; index < components.size(); ++index) {
        QPointF start;
        QPointF end;
        if (!endpoints(components[index], &start, &end)) {
            continue;
        }
        if (index == 0) {
            points.append(start);
        }
        points.append(end);
    }
    return points;
}

bool componentsForExplode(const Shape &shape,
                          QVector<Shape::NurbsCurve2D> *components)
{
    if (components == nullptr) {
        return false;
    }
    components->clear();
    if (shape.geometryType == GeometryType::PolyCurve) {
        if (shape.components.isEmpty()) {
            return false;
        }
        *components = shape.components;
    } else if (shape.geometryType == GeometryType::Rectangle) {
        const QVector<QPointF> vertices = rectangleVertices(shape);
        if (vertices.size() < 4) {
            return false;
        }
        components->reserve(4);
        for (int index = 0; index < 4; ++index) {
            components->append(makeDegreeOneNurbs(
                {vertices[index], vertices[(index + 1) % 4]}));
        }
    } else {
        return false;
    }

    for (const Shape::NurbsCurve2D &component : *components) {
        if (!validateNurbsCurve(component)) {
            components->clear();
            return false;
        }
    }
    return !components->isEmpty();
}

} // namespace

bool buildExplodeCommandPlan(const Document &document,
                             const QVector<ObjectId> &selectedObjectIds,
                             ExplodeCommandPlan *plan)
{
    if (plan == nullptr) {
        return false;
    }
    *plan = ExplodeCommandPlan{};
    int explodeableShapeCount = 0;
    for (const SceneObject &object : document.objects()) {
        if (!selectedObjectIds.contains(object.id) ||
            !document.isObjectEditable(object.id)) {
            continue;
        }
        QVector<Shape::NurbsCurve2D> components;
        if (componentsForExplode(object.geometry, &components)) {
            ++explodeableShapeCount;
            plan->outputComponentCount += components.size();
        }
    }
    if (explodeableShapeCount == 0) {
        return true;
    }

    plan->sourceObjectCount = explodeableShapeCount;
    plan->replacementObjects.reserve(document.objects().size() +
                                     plan->outputComponentCount -
                                     explodeableShapeCount);
    for (const SceneObject &sourceObject : document.objects()) {
        const Shape &source = sourceObject.geometry;
        const bool selectedSource = selectedObjectIds.contains(sourceObject.id);
        QVector<Shape::NurbsCurve2D> components;
        const bool replaceSource = selectedSource &&
                                   document.isObjectEditable(sourceObject.id) &&
                                   componentsForExplode(source, &components);
        if (!replaceSource) {
            const int newIndex = plan->replacementObjects.size();
            plan->replacementObjects.append(sourceObject);
            if (selectedSource) {
                plan->selectedObjectIndices.append(newIndex);
            }
            continue;
        }

        for (int componentIndex = 0; componentIndex < components.size();
             ++componentIndex) {
            const Shape::NurbsCurve2D &component = components[componentIndex];
            const WorkPlaneFrame componentFrame =
                source.geometryType == GeometryType::PolyCurve
                    ? shapeComponentWorkPlaneFrame(source, componentIndex)
                    : shapeWorkPlaneFrame(source);
            const int newIndex = plan->replacementObjects.size();
            SceneObject componentObject;
            componentObject.layerId = sourceObject.layerId;
            if (source.geometryType == GeometryType::Rectangle) {
                componentObject.geometry.geometryType = GeometryType::Line;
                componentObject.geometry.points = component.controlPoints;
                componentObject.geometry.nurbs = component;
            } else {
                componentObject.geometry.geometryType = GeometryType::PolyCurve;
                componentObject.geometry.points = polyCurvePoints({component});
                componentObject.geometry.arcMode = ArcMode::TwoPoint;
                componentObject.geometry.components = {component};
            }
            componentObject.geometry.workPlane = source.workPlane;
            componentObject.geometry.workPlaneOffset = source.workPlaneOffset;
            componentObject.geometry.workPlaneFrame = componentFrame;
            plan->replacementObjects.append(componentObject);
            plan->selectedObjectIndices.append(newIndex);
        }
    }
    return !plan->replacementObjects.isEmpty();
}

bool applyExplodeCommand(DocumentTransaction &transaction,
                         const ExplodeCommandPlan &plan)
{
    if (plan.sourceObjectCount <= 0 || plan.replacementObjects.isEmpty()) {
        return false;
    }
    transaction.replaceObjects(plan.replacementObjects);
    return true;
}

} // namespace classiCAD
