#include "join_tool.h"

#include "core/commands/join_command.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/curve_join.h"
#include "core/geometry/shape_mapping.h"
#include "core/document/selection_model.h"
#include "tool_context.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <Qt>

namespace classiCAD {
namespace {

bool isJoinableShape(const Shape &shape)
{
    if (shape.geometryType == GeometryType::PolyCurve) {
        if (shape.components.isEmpty()) {
            return false;
        }
        for (const Shape::NurbsCurve2D &component : shape.components) {
            if (!validateNurbsCurve(component)) {
                return false;
            }
        }
        return true;
    }

    return shape.geometryType == GeometryType::Line ||
           shape.geometryType == GeometryType::Arc ||
           shape.geometryType == GeometryType::Bezier ||
           shape.geometryType == GeometryType::Nurbs ||
           shape.geometryType == GeometryType::Ellipse;
}

bool appendJoinComponents(const Shape &shape,
                          QVector<Shape::NurbsCurve2D> *components)
{
    if (components == nullptr || !isJoinableShape(shape)) {
        return false;
    }
    if (shape.geometryType == GeometryType::PolyCurve) {
        *components += shape.components;
        return true;
    }
    if (validateNurbsCurve(shape.nurbs)) {
        components->append(shape.nurbs);
        return true;
    }
    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        const Shape::NurbsCurve2D line = makeDegreeOneNurbs(shape.points);
        if (validateNurbsCurve(line)) {
            components->append(line);
            return true;
        }
    }
    return false;
}

qreal nearestJoinEndpointGap(const QVector<Shape::NurbsCurve2D> &components)
{
    qreal nearestGap = std::numeric_limits<qreal>::infinity();
    for (int first = 0; first < components.size(); ++first) {
        QPointF firstStart;
        QPointF firstEnd;
        if (!nurbsCurveEndpoints(components[first], &firstStart, &firstEnd)) {
            continue;
        }
        for (int second = first + 1; second < components.size(); ++second) {
            QPointF secondStart;
            QPointF secondEnd;
            if (!nurbsCurveEndpoints(components[second], &secondStart, &secondEnd)) {
                continue;
            }
            for (const QPointF &firstPoint : {firstStart, firstEnd}) {
                for (const QPointF &secondPoint : {secondStart, secondEnd}) {
                    nearestGap = std::min(
                        nearestGap,
                        std::hypot(firstPoint.x() - secondPoint.x(),
                                   firstPoint.y() - secondPoint.y()));
                }
            }
        }
    }
    return nearestGap;
}

} // namespace

void JoinTool::begin(const QVector<ObjectId> &preselectedObjectIds)
{
    active_ = true;
    selectedObjectIds_ = preselectedObjectIds;
}

void JoinTool::cancel()
{
    active_ = false;
    selectedObjectIds_.clear();
}

void JoinTool::complete()
{
    cancel();
}

JoinClickAction JoinTool::handleObjectClick(ObjectId objectId, bool joinable)
{
    if (!active_) {
        return JoinClickAction::Ignored;
    }
    if (!joinable || !objectId.isValid()) {
        return JoinClickAction::InvalidTarget;
    }
    if (contains(objectId)) {
        return JoinClickAction::AlreadySelected;
    }
    if (!addObject(objectId)) {
        return JoinClickAction::Ignored;
    }
    return selectedObjectIds_.size() >= 2
               ? JoinClickAction::ReadyToJoin
               : JoinClickAction::Added;
}

JoinKeyAction JoinTool::handleKey(int key) const
{
    if (!active_) {
        return JoinKeyAction::Unhandled;
    }
    if (key == Qt::Key_Escape) {
        return JoinKeyAction::Cancel;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        return JoinKeyAction::Complete;
    }
    return JoinKeyAction::Unhandled;
}

JoinExecutionResult JoinTool::executeJoin(qreal endpointTolerance,
                                          ToolContext &context)
{
    JoinExecutionResult result;
    if (!active_) {
        result.failure = JoinExecutionFailure::Inactive;
        return result;
    }
    if (selectedObjectIds_.isEmpty()) {
        result.failure = JoinExecutionFailure::NeedTwoCurves;
        return result;
    }

    Document &document = context.document();
    const ObjectId referenceId = selectedObjectIds_.first();
    const SceneObject *referenceObject = document.object(referenceId);
    if (referenceObject == nullptr) {
        result.failure = JoinExecutionFailure::CurvesUnavailable;
        return result;
    }

    const Shape &referenceShape = referenceObject->geometry;
    const LayerId joinedLayerId = referenceObject->layerId;
    const WorkPlaneFrame joinFrame = shapeWorkPlaneFrame(referenceShape);
    const WorkPlane joinWorkPlane = referenceShape.workPlane;
    const qreal joinWorkPlaneOffset = referenceShape.workPlaneOffset;
    QVector<Shape::NurbsCurve2D> components;
    QVector<WorkPlaneFrame> componentFrames;
    bool mixedPlanes = false;
    for (const ObjectId objectId : selectedObjectIds_) {
        const SceneObject *sourceObject = document.object(objectId);
        if (sourceObject == nullptr) {
            result.failure = JoinExecutionFailure::InvalidCurveSelection;
            return result;
        }
        const Shape &sourceShape = sourceObject->geometry;
        const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(sourceShape);
        const int firstComponent = components.size();
        if (!appendJoinComponents(sourceShape, &components)) {
            result.failure = JoinExecutionFailure::InvalidCurveSelection;
            return result;
        }
        for (int componentIndex = firstComponent;
             componentIndex < components.size();
             ++componentIndex) {
            const int sourceComponentIndex = componentIndex - firstComponent;
            const WorkPlaneFrame componentFrame =
                sourceShape.geometryType == GeometryType::PolyCurve
                    ? shapeComponentWorkPlaneFrame(sourceShape,
                                                   sourceComponentIndex)
                    : sourceFrame;
            componentFrames.append(componentFrame);
            mixedPlanes = mixedPlanes ||
                          !workPlaneFramesCoplanar(joinFrame, componentFrame);
        }
    }

    result.componentCount = components.size();
    result.sourceObjectCount = selectedObjectIds_.size();
    if (components.size() < 2) {
        result.failure = JoinExecutionFailure::NeedTwoCurves;
        return result;
    }

    int mergeCount = 0;
    if (mixedPlanes) {
        mergeCount += fuseOverlappingNurbsLineComponentsInWorld(
            &components, &componentFrames, endpointTolerance);
        result.componentCount = components.size();
        QVector<Shape::NurbsCurve2D> orderedComponents;
        QVector<WorkPlaneFrame> orderedFrames;
        if (!orderConnectedNurbsCurvesInWorld(components,
                                               componentFrames,
                                               &orderedComponents,
                                               &orderedFrames,
                                               endpointTolerance)) {
            result.failure = JoinExecutionFailure::DisconnectedCurves;
            return result;
        }
        components = std::move(orderedComponents);
        componentFrames = std::move(orderedFrames);
        result.componentCount = components.size();
        if (!closeConnectedNurbsCurveGapsInWorld(&components,
                                                  &componentFrames,
                                                  endpointTolerance) ||
            !connectedNurbsCurvesAreContinuousInWorld(components,
                                                       componentFrames,
                                                       endpointTolerance)) {
            result.failure = JoinExecutionFailure::DisconnectedCurves;
            return result;
        }
    } else {
        for (int componentIndex = 0;
             componentIndex < components.size();
             ++componentIndex) {
            for (QPointF &controlPoint : components[componentIndex].controlPoints) {
                controlPoint = worldPointToWorkPlaneFrame(
                    workPlaneFramePointToWorld(
                        controlPoint, componentFrames[componentIndex]),
                    joinFrame);
            }
        }
        componentFrames.clear();
        mergeCount += fuseOverlappingNurbsLineComponents(&components,
                                                          endpointTolerance);
        result.componentCount = components.size();
        if (!connectedNurbsCurvesAreContinuous(components,
                                               endpointTolerance)) {
            QVector<Shape::NurbsCurve2D> orderedComponents;
            if (!orderConnectedNurbsCurves(components,
                                            &orderedComponents,
                                            endpointTolerance)) {
                result.failure = JoinExecutionFailure::DisconnectedCurves;
                result.nearestEndpointGap = nearestJoinEndpointGap(components);
                return result;
            }
            components = std::move(orderedComponents);
            result.componentCount = components.size();
        }
        if (!closeConnectedNurbsCurveGaps(&components, endpointTolerance) ||
            !connectedNurbsCurvesAreContinuous(components,
                                               endpointTolerance)) {
            result.failure = JoinExecutionFailure::DisconnectedCurves;
            result.nearestEndpointGap = nearestJoinEndpointGap(components);
            return result;
        }
    }

    result.componentCount = components.size();
    result.lineMergeCount = mergeCount;
    int insertionIndex = document.size();
    for (const ObjectId objectId : selectedObjectIds_) {
        const int index = document.indexOf(objectId);
        if (index < 0) {
            result.failure = JoinExecutionFailure::InvalidCurveSelection;
            return result;
        }
        insertionIndex = std::min(insertionIndex, index);
    }

    JoinCommandPlan plan;
    if (!buildJoinCommandPlan(components,
                              componentFrames,
                              mixedPlanes,
                              joinFrame,
                              joinWorkPlane,
                              joinWorkPlaneOffset,
                              joinedLayerId,
                              selectedObjectIds_,
                              insertionIndex,
                              &plan)) {
        result.failure = JoinExecutionFailure::CommandFailed;
        return result;
    }
    DocumentTransaction transaction = context.beginTransaction();
    if (!applyJoinCommand(document, transaction, plan,
                          &result.joinedObjectId) ||
        !context.commitTransaction(transaction)) {
        result.failure = JoinExecutionFailure::CommandFailed;
        result.joinedObjectId = ObjectId::invalid();
        return result;
    }

    context.selection().setObjectIds({result.joinedObjectId},
                                     result.joinedObjectId);
    context.notifySelectionChanged();
    complete();
    result.committed = true;
    return result;
}

bool JoinTool::addObject(ObjectId objectId)
{
    if (!active_ || !objectId.isValid() || contains(objectId)) {
        return false;
    }
    selectedObjectIds_.append(objectId);
    return true;
}

bool JoinTool::contains(ObjectId objectId) const
{
    return selectedObjectIds_.contains(objectId);
}

bool JoinTool::isActive() const
{
    return active_;
}

int JoinTool::selectedCount() const
{
    return selectedObjectIds_.size();
}

QString JoinTool::prompt() const
{
    if (!active_) {
        return QString();
    }
    return QStringLiteral("Join: %1 curves selected  •  Click connected curves to join  •  Esc to cancel")
        .arg(selectedObjectIds_.size());
}

const QVector<ObjectId> &JoinTool::selectedObjectIds() const
{
    return selectedObjectIds_;
}

} // namespace classiCAD
