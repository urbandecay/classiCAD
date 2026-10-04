#include "join_command.h"

#include "core/geometry/curve_evaluator.h"
#include "core/geometry/shape_mapping.h"

namespace classiCAD {
namespace {

QVector<QPointF> polyCurvePoints(
    const QVector<Shape::NurbsCurve2D> &components)
{
    QVector<QPointF> points;
    for (int index = 0; index < components.size(); ++index) {
        QPointF start;
        QPointF end;
        if (!nurbsCurveEndpoints(components[index], &start, &end)) {
            continue;
        }
        if (index == 0) {
            points.append(start);
        }
        points.append(end);
    }
    return points;
}

} // namespace

bool buildJoinCommandPlan(
    const QVector<Shape::NurbsCurve2D> &components,
    const QVector<WorkPlaneFrame> &componentFrames,
    bool mixedPlanes,
    const WorkPlaneFrame &joinFrame,
    WorkPlane joinWorkPlane,
    qreal joinWorkPlaneOffset,
    LayerId joinedLayerId,
    const QVector<ObjectId> &sourceObjectIds,
    int insertionIndex,
    JoinCommandPlan *plan)
{
    if (plan == nullptr || components.isEmpty() || sourceObjectIds.isEmpty() ||
        insertionIndex < 0 || !isValidWorkPlaneFrame(joinFrame) ||
        (mixedPlanes && components.size() > 1 &&
         componentFrames.size() != components.size())) {
        return false;
    }
    for (const Shape::NurbsCurve2D &component : components) {
        if (!validateNurbsCurve(component)) {
            return false;
        }
    }

    Shape joined;
    const WorkPlaneFrame resultFrame = mixedPlanes && components.size() == 1
                                           ? componentFrames.first()
                                           : joinFrame;
    if (!isValidWorkPlaneFrame(resultFrame)) {
        return false;
    }
    if (components.size() == 1) {
        joined.geometryType = GeometryType::Nurbs;
        joined.nurbs = components.first();
        joined.points = joined.nurbs.controlPoints;
    } else {
        joined.geometryType = GeometryType::PolyCurve;
        if (mixedPlanes) {
            for (int index = 0; index < components.size(); ++index) {
                QPointF start;
                QPointF end;
                if (!nurbsCurveEndpoints(components[index], &start, &end)) {
                    continue;
                }
                if (index == 0) {
                    joined.points.append(worldPointToWorkPlaneFrame(
                        workPlaneFramePointToWorld(start, componentFrames[index]),
                        joinFrame));
                }
                joined.points.append(worldPointToWorkPlaneFrame(
                    workPlaneFramePointToWorld(end, componentFrames[index]),
                    joinFrame));
            }
            joined.componentWorkPlaneFrames = componentFrames;
        } else {
            joined.points = polyCurvePoints(components);
        }
        joined.components = components;
    }
    joined.workPlane = joinWorkPlane;
    joined.workPlaneOffset = joinWorkPlaneOffset;
    joined.workPlaneFrame = resultFrame;

    *plan = JoinCommandPlan{};
    plan->sourceObjectIds = sourceObjectIds;
    plan->joinedObject.layerId = joinedLayerId;
    plan->joinedObject.geometry = std::move(joined);
    plan->insertionIndex = insertionIndex;
    return true;
}

bool applyJoinCommand(const Document &document,
                      DocumentTransaction &transaction,
                      const JoinCommandPlan &plan,
                      ObjectId *joinedObjectId)
{
    if (joinedObjectId != nullptr) {
        *joinedObjectId = ObjectId::invalid();
    }
    if (plan.sourceObjectIds.isEmpty() || plan.insertionIndex < 0) {
        return false;
    }
    for (const ObjectId sourceObjectId : plan.sourceObjectIds) {
        if (!document.isObjectEditable(sourceObjectId)) {
            return false;
        }
    }
    for (const ObjectId sourceObjectId : plan.sourceObjectIds) {
        if (!transaction.removeObject(sourceObjectId)) {
            return false;
        }
    }
    const ObjectId result = transaction.insertObject(plan.insertionIndex,
                                                     plan.joinedObject);
    if (!result.isValid()) {
        return false;
    }
    if (joinedObjectId != nullptr) {
        *joinedObjectId = result;
    }
    return true;
}

} // namespace classiCAD
