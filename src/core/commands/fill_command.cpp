#include "fill_command.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "core/geometry/nurbs_surface_factory.h"

#include <algorithm>
#include <cmath>

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

bool makeBoundaryCurve(const Shape &shape,
                       Shape::NurbsCurve2D *curve,
                       WorkPlaneFrame *frame)
{
    if (curve == nullptr || frame == nullptr) {
        return false;
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        if (shape.components.isEmpty()) {
            return false;
        }
        if (shape.components.size() == 1) {
            *curve = shape.components.first();
            *frame = shapeComponentWorkPlaneFrame(shape, 0);
            return validateNurbsCurve(*curve) && isValidWorkPlaneFrame(*frame);
        }

        *frame = shapeComponentWorkPlaneFrame(shape, 0);
        if (!isValidWorkPlaneFrame(*frame)) {
            return false;
        }
        QVector<QVector<QPointF>> componentPaths;
        componentPaths.reserve(shape.components.size());
        for (int componentIndex = 0;
             componentIndex < shape.components.size();
             ++componentIndex) {
            const Shape::NurbsCurve2D &component = shape.components[componentIndex];
            const WorkPlaneFrame componentFrame =
                shapeComponentWorkPlaneFrame(shape, componentIndex);
            if (!validateNurbsCurve(component) || component.degree != 1 ||
                component.controlPoints.size() < 2 ||
                !workPlaneFramesCoplanar(*frame, componentFrame)) {
                return false;
            }

            QVector<QPointF> path;
            path.reserve(component.controlPoints.size());
            for (const QPointF &point : component.controlPoints) {
                path.append(worldPointToWorkPlaneFrame(
                    workPlaneFramePointToWorld(point, componentFrame), *frame));
            }
            componentPaths.append(path);
        }

        QVector<QPointF> boundary = componentPaths.takeFirst();
        QVector<bool> consumed(componentPaths.size(), false);
        for (int joinedCount = 0; joinedCount < componentPaths.size();
             ++joinedCount) {
            bool joined = false;
            for (int candidateIndex = 0;
                 candidateIndex < componentPaths.size();
                 ++candidateIndex) {
                if (consumed[candidateIndex]) {
                    continue;
                }
                QVector<QPointF> candidate = componentPaths[candidateIndex];
                const QPointF &tail = boundary.last();
                const qreal scale = std::max<qreal>(
                    {1.0, std::abs(tail.x()), std::abs(tail.y()),
                     std::abs(candidate.first().x()),
                     std::abs(candidate.first().y()),
                     std::abs(candidate.last().x()),
                     std::abs(candidate.last().y())});
                const qreal tolerance = scale * 1.0e-7;
                const auto endpointsMatch = [tolerance](
                    const QPointF &first, const QPointF &second) {
                    return std::hypot(first.x() - second.x(),
                                      first.y() - second.y()) <= tolerance;
                };
                if (endpointsMatch(tail, candidate.last())) {
                    std::reverse(candidate.begin(), candidate.end());
                } else if (!endpointsMatch(tail, candidate.first())) {
                    continue;
                }
                for (int pointIndex = 1; pointIndex < candidate.size();
                     ++pointIndex) {
                    boundary.append(candidate[pointIndex]);
                }
                consumed[candidateIndex] = true;
                joined = true;
                break;
            }
            if (!joined) {
                return false;
            }
        }

        if (boundary.size() < 4) {
            return false;
        }
        const QPointF first = boundary.first();
        const QPointF last = boundary.last();
        const qreal closureScale = std::max<qreal>(
            {1.0, std::abs(first.x()), std::abs(first.y()),
             std::abs(last.x()), std::abs(last.y())});
        if (std::hypot(first.x() - last.x(), first.y() - last.y()) >
            closureScale * 1.0e-7) {
            return false;
        }
        boundary.last() = first;
        *curve = makeDegreeOneNurbs(boundary);
        return validateNurbsCurve(*curve);
    }

    *frame = shapeWorkPlaneFrame(shape);
    if (!isValidWorkPlaneFrame(*frame)) {
        return false;
    }
    if (validateNurbsCurve(shape.nurbs)) {
        *curve = shape.nurbs;
        return true;
    }

    QVector<QPointF> vertices;
    if (shape.geometryType == GeometryType::Rectangle) {
        vertices = rectangleVertices(shape);
    } else if (shape.geometryType == GeometryType::Polygon) {
        vertices = polygonVerticesForShape(shape);
    }
    if (vertices.size() < 3) {
        return false;
    }
    vertices.append(vertices.first());
    *curve = makeDegreeOneNurbs(vertices);
    return validateNurbsCurve(*curve);
}

} // namespace

bool buildFillCommandPlan(const Document &document,
                          const QVector<ObjectId> &selectedObjectIds,
                          FillCommandPlan *plan)
{
    if (plan == nullptr) {
        return false;
    }
    *plan = FillCommandPlan{};
    for (const ObjectId objectId : selectedObjectIds) {
        if (!document.isObjectEditable(objectId)) {
            continue;
        }
        const Shape *shape = document.shape(objectId);
        if (shape == nullptr) {
            continue;
        }

        Shape::NurbsCurve2D boundary;
        WorkPlaneFrame frame;
        Shape::NurbsSurface3D surface;
        if (makeBoundaryCurve(*shape, &boundary, &frame) &&
            makeNurbsPlanarFillSurface(boundary, frame, &surface) &&
            validateNurbsSurface(surface)) {
            plan->candidates.append({objectId, std::move(surface)});
        }
    }
    return true;
}

bool applyFillCommand(DocumentTransaction &transaction,
                      const FillCommandPlan &plan,
                      int *filledCount)
{
    if (filledCount != nullptr) {
        *filledCount = 0;
    }
    if (plan.candidates.isEmpty()) {
        return false;
    }
    for (const FillCommandCandidate &candidate : plan.candidates) {
        Shape *shape = transaction.editGeometry(candidate.objectId);
        if (shape == nullptr) {
            return false;
        }
        shape->geometryType = GeometryType::NurbsSurface;
        shape->points.clear();
        shape->nurbs = Shape::NurbsCurve2D{};
        shape->subdivisionParameters.clear();
        shape->components.clear();
        shape->componentWorkPlaneFrames.clear();
        shape->nurbsSurface = candidate.surface;
    }
    if (filledCount != nullptr) {
        *filledCount = plan.candidates.size();
    }
    return true;
}

} // namespace classiCAD
