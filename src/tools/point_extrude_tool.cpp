#include "point_extrude_tool.h"

#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"

#include "tool_context.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/geometry/nurbs_solid_face_extrusion.h"
#include "core/debug_log.h"
#include "core/serialization/shape_json_codec.h"

#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace classiCAD {
namespace {

Point3D subtract(const Point3D &first, const Point3D &second)
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Point3D add(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D multiply(const Point3D &point, qreal scalar)
{
    return {point.x * scalar, point.y * scalar, point.z * scalar};
}

qreal dot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

qreal length(const Point3D &point)
{
    return std::sqrt(dot(point, point));
}

Point3D normalized(const Point3D &point)
{
    const qreal magnitude = length(point);
    return magnitude > 1.0e-12 ? multiply(point, 1.0 / magnitude) : Point3D{};
}

Point3D axisDirection(int key)
{
    switch (key) {
    case Qt::Key_X: return {1.0, 0.0, 0.0};
    case Qt::Key_Y: return {0.0, 1.0, 0.0};
    case Qt::Key_Z: return {0.0, 0.0, 1.0};
    default: return {};
    }
}

} // namespace

ToolId PointExtrudeTool::id() const
{
    return ToolId::PointExtrude;
}

void PointExtrudeTool::setControlPointSources(
    const QVector<ControlPointSource> &sources)
{
    stagedControlPointSources_ = sources;
    stagedControlPointSourcesRequested_ = true;
}

void PointExtrudeTool::setCurveSources(
    const QVector<CurveSource> &sources)
{
    stagedCurveSources_ = sources;
}

void PointExtrudeTool::setSurfaceFaceSources(
    const QVector<SurfaceFaceSource> &sources)
{
    stagedSurfaceFaceSources_ = sources;
}

void PointExtrudeTool::setSolidFaceSources(
    const QVector<SolidFaceSource> &sources)
{
    stagedSolidFaceSources_ = sources;
}

void PointExtrudeTool::begin(ToolContext &context)
{
    const QVector<ControlPointSource> stagedSources =
        stagedControlPointSources_;
    const QVector<CurveSource> stagedCurves = stagedCurveSources_;
    const QVector<SurfaceFaceSource> stagedSurfaceFaces =
        stagedSurfaceFaceSources_;
    const QVector<SolidFaceSource> stagedSolidFaces = stagedSolidFaceSources_;
    const bool useStagedSources = stagedControlPointSourcesRequested_;
    stagedControlPointSources_.clear();
    stagedCurveSources_.clear();
    stagedSurfaceFaceSources_.clear();
    stagedSolidFaceSources_.clear();
    stagedControlPointSourcesRequested_ = false;
    sourcePoints_.clear();
    completedExtrusionObjectIds_.clear();
    completedFaceExtrusions_.clear();
    candidateShapes_.clear();
    candidateFaceIndices_.clear();
    displayShapes_.clear();
    displayFaceIndices_.clear();
    candidateError_.clear();
    lastDiagnostic_.clear();
    inputFrame_ = {};
    cursorPoint_ = {};
    lastInput_ = {};
    constraintAxisKey_ = 0;
    normalConstraint_ = false;
    hasLastInput_ = false;
    hasCursorPoint_ = false;
    snap_ = {};

    if (useStagedSources) {
        for (const ControlPointSource &staged : stagedSources) {
            if (!isValidWorkPlaneFrame(staged.workPlaneFrame)) {
                continue;
            }
            SourcePoint source;
            source.objectId = staged.objectId;
            source.worldPoint = staged.worldPoint;
            source.workPlaneFrame = staged.workPlaneFrame;
            source.sourceControlPointIndex = staged.controlPointIndex;
            source.workPlaneFrame.origin = staged.worldPoint;
            sourcePoints_.append(source);
        }
        for (const CurveSource &staged : stagedCurves) {
            if (!validateNurbsCurve(staged.curve) ||
                !isValidWorkPlaneFrame(staged.workPlaneFrame)) {
                continue;
            }
            SourcePoint source;
            source.objectId = staged.objectId;
            source.curve = staged.curve;
            source.workPlaneFrame = staged.workPlaneFrame;
            source.sourceControlPointIndices =
                staged.sourceControlPointIndices;
            const qreal normalCoordinate = source.curve.dimension == 3
                                               ? source.curve.normalCoordinates.first()
                                               : 0.0;
            source.worldPoint = workPlaneFramePointToWorld(
                source.curve.controlPoints.first(), normalCoordinate,
                source.workPlaneFrame);
            sourcePoints_.append(source);
        }
    } else {
        const QVector<ObjectId> &selectedObjects = context.selection().objectIds();
        for (const ObjectId selectedObjectId : selectedObjects) {
        const Shape *selectedShape = context.document().shape(selectedObjectId);
        if (selectedShape == nullptr ||
            !context.document().isObjectVisible(selectedObjectId) ||
            !context.document().isObjectEditable(selectedObjectId)) {
            continue;
        }
        if (selectedShape != nullptr &&
            selectedShape->geometryType == GeometryType::Point &&
            selectedShape->points.size() == 1 &&
            context.document().isObjectVisible(selectedObjectId) &&
            context.document().isObjectEditable(selectedObjectId)) {
            const SceneObject *sceneObject =
                context.document().object(selectedObjectId);
            const Point3D placement = sceneObject != nullptr
                                          ? sceneObject->placementTranslation
                                          : Point3D{};
            SourcePoint source;
            source.objectId = selectedObjectId;
            source.worldPoint = shapePointToWorld(*selectedShape,
                                                  selectedShape->points.first());
            source.sourceControlPointIndex = 0;
            source.worldPoint.x += placement.x;
            source.worldPoint.y += placement.y;
            source.worldPoint.z += placement.z;
            source.workPlaneFrame = shapeWorkPlaneFrame(*selectedShape);
            source.workPlaneFrame.origin = source.worldPoint;
            if (isValidWorkPlaneFrame(source.workPlaneFrame)) {
                sourcePoints_.append(source);
            }
        } else if (selectedShape->geometryType == GeometryType::NurbsSurface) {
            SourcePoint source;
            const SceneObject *sceneObject =
                context.document().object(selectedObjectId);
            source.surface = selectedShape->nurbsSurface;
            if (sceneObject != nullptr) {
                const Point3D &offset = sceneObject->placementTranslation;
                for (Point3D &point : source.surface.controlPoints) {
                    point.x += offset.x;
                    point.y += offset.y;
                    point.z += offset.z;
                }
            }
            if (!nurbsSolidBaseFrame(source.surface,
                                     &source.workPlaneFrame)) continue;
            source.objectId = selectedObjectId;
            source.worldPoint = source.workPlaneFrame.origin;
            sourcePoints_.append(source);
        } else {
            const auto curves = context.curveSampler().curvesForShape(*selectedShape);
            int firstControlPointIndex = 0;
            for (int index = 0; index < curves.size(); ++index) {
                const int curveFirstControlPointIndex =
                    firstControlPointIndex;
                firstControlPointIndex += curves[index].controlPoints.size();
                if (!validateNurbsCurve(curves[index])) continue;
                SourcePoint source;
                source.objectId = selectedObjectId;
                source.curve = curves[index];
                source.sourceControlPointIndices.reserve(
                    curves[index].controlPoints.size());
                for (int controlPointIndex = 0;
                     controlPointIndex < curves[index].controlPoints.size();
                     ++controlPointIndex) {
                    source.sourceControlPointIndices.append(
                        curveFirstControlPointIndex + controlPointIndex);
                }
                const SceneObject *sceneObject =
                    context.document().object(selectedObjectId);
                const Point3D placement = sceneObject != nullptr
                                              ? sceneObject->placementTranslation
                                              : Point3D{};
                source.workPlaneFrame = selectedShape->geometryType == GeometryType::PolyCurve
                    ? shapeComponentWorkPlaneFrame(*selectedShape, index)
                    : shapeWorkPlaneFrame(*selectedShape);
                if (!isValidWorkPlaneFrame(source.workPlaneFrame)) continue;
                source.workPlaneFrame.origin.x += placement.x;
                source.workPlaneFrame.origin.y += placement.y;
                source.workPlaneFrame.origin.z += placement.z;
                const qreal normalCoordinate = source.curve.dimension == 3
                                                   ? source.curve.normalCoordinates.first()
                                                   : 0.0;
                source.worldPoint = workPlaneFramePointToWorld(
                    source.curve.controlPoints.first(), normalCoordinate,
                    source.workPlaneFrame);
                sourcePoints_.append(source);
            }
        }
        }
    }

    for (const SurfaceFaceSource &staged : stagedSurfaceFaces) {
        WorkPlaneFrame surfaceFrame;
        if (!staged.objectId.isValid() ||
            !nurbsSolidBaseFrame(staged.surface, &surfaceFrame) ||
            !isValidWorkPlaneFrame(staged.workPlaneFrame)) {
            continue;
        }
        SourcePoint source;
        source.objectId = staged.objectId;
        source.surface = staged.surface;
        source.workPlaneFrame = staged.workPlaneFrame;
        source.workPlaneFrame.origin = staged.worldPoint;
        source.worldPoint = staged.worldPoint;
        sourcePoints_.append(source);
    }

    for (const SolidFaceSource &staged : stagedSolidFaces) {
        const int faceCount = nurbsSolidFaces(staged.solid).size();
        if (!staged.objectId.isValid() || staged.faceIndex < 0 ||
            staged.faceIndex >= faceCount ||
            !validateNurbsSolid(staged.solid) ||
            !isValidWorkPlaneFrame(staged.workPlaneFrame)) {
            continue;
        }
        SourcePoint source;
        source.objectId = staged.objectId;
        source.solid = staged.solid;
        source.solidFaceIndex = staged.faceIndex;
        source.workPlaneFrame = staged.workPlaneFrame;
        source.workPlaneFrame.origin = staged.worldPoint;
        source.worldPoint = staged.worldPoint;
        sourcePoints_.append(source);
    }

    if (!sourcePoints_.isEmpty()) {
        for (const SourcePoint &source : sourcePoints_) {
            if (source.solidFaceIndex < 0) continue;
            Shape shape;
            shape.geometryType = GeometryType::NurbsSolid;
            shape.nurbsSolid = source.solid;
            DebugLog::instance().write(
                QStringLiteral("extrude source object=%1 face=%2 geometry=%3")
                    .arg(source.objectId.value()).arg(source.solidFaceIndex)
                    .arg(QString::fromUtf8(QJsonDocument(shapeToJson(shape))
                                              .toJson(QJsonDocument::Compact))));
        }
        int constraintSourceIndex = -1;
        // Face-derived sources can be staged after ordinary vertices and
        // curves. Use a solid face as the shared direction reference even
        // when the user selected mixed geometry.
        for (int index = 0; index < sourcePoints_.size(); ++index) {
            if (sourcePoints_[index].solidFaceIndex >= 0) {
                constraintSourceIndex = index;
                break;
            }
        }
        if (constraintSourceIndex < 0) {
            for (int index = 0; index < sourcePoints_.size(); ++index) {
                if (!sourcePoints_[index].surface.controlPoints.isEmpty()) {
                    constraintSourceIndex = index;
                    break;
                }
            }
        }
        if (constraintSourceIndex < 0) {
            constraintSourceIndex = 0;
        }
        normalConstraint_ =
            !sourcePoints_[constraintSourceIndex].surface.controlPoints.isEmpty() ||
            sourcePoints_[constraintSourceIndex].solidFaceIndex >= 0;
        inputFrame_ = sourcePoints_[constraintSourceIndex].workPlaneFrame;
        inputFrame_.origin = sourcePoints_[constraintSourceIndex].worldPoint;
        cursorPoint_ = sourcePoints_.first().worldPoint;
        context.viewportTransform().setWorkPlaneFrame(inputFrame_);
    }

    status_.state = ToolLifecycleState::Active;
    updateStatus();
    publish(context);
}

bool PointExtrudeTool::handleMousePress(const ToolInput &input,
                                        ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }
    if (input.button != Qt::LeftButton || sourcePoints_.isEmpty()) {
        return false;
    }

    lastInput_ = input;
    hasLastInput_ = true;
    cursorPoint_ = resolveTarget(input, context);
    hasCursorPoint_ = true;
    updateStatus();
    if (status_.canCommit) {
        commit(context);
    } else {
        publish(context);
    }
    return true;
}

bool PointExtrudeTool::handleMouseMove(const ToolInput &input,
                                       ToolContext &context)
{
    if (sourcePoints_.isEmpty()) {
        return false;
    }

    lastInput_ = input;
    hasLastInput_ = true;
    cursorPoint_ = resolveTarget(input, context);
    hasCursorPoint_ = true;
    updateStatus();
    publish(context);
    return true;
}

bool PointExtrudeTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key == Qt::Key_Escape) {
        cancel(context);
        context.finishTool(ToolId::Select);
        return true;
    }
    if (input.key == Qt::Key_W) {
        if (!input.autoRepeat) toggleWeld(context);
        return true;
    }
    if (input.key == Qt::Key_M) {
        if (!input.autoRepeat) toggleManifold(context);
        return true;
    }
    if (input.key == Qt::Key_X || input.key == Qt::Key_Y ||
        input.key == Qt::Key_Z) {
        normalConstraint_ = false;
        constraintAxisKey_ = constraintAxisKey_ == input.key ? 0 : input.key;
        if (hasLastInput_ && !sourcePoints_.isEmpty()) {
            cursorPoint_ = resolveTarget(lastInput_, context);
            hasCursorPoint_ = true;
        }
        updateStatus();
        publish(context);
        return true;
    }
    if (input.key == Qt::Key_Return || input.key == Qt::Key_Enter ||
        input.key == Qt::Key_Space) {
        commit(context);
        return true;
    }
    return false;
}

void PointExtrudeTool::cancel(ToolContext &context)
{
    sourcePoints_.clear();
    candidateShapes_.clear();
    candidateFaceIndices_.clear();
    displayShapes_.clear();
    displayFaceIndices_.clear();
    candidateError_.clear();
    completedExtrusionObjectIds_.clear();
    completedFaceExtrusions_.clear();
    constraintAxisKey_ = 0;
    normalConstraint_ = false;
    hasLastInput_ = false;
    hasCursorPoint_ = false;
    snap_ = {};
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    status_.text = QStringLiteral("Extrude cancelled");
    publish(context);
}

void PointExtrudeTool::commit(ToolContext &context)
{
    if (sourcePoints_.isEmpty() || !hasCursorPoint_ || !status_.canCommit) {
        return;
    }

    const QVector<int> generatedFaceIndices = candidateFaceIndices_;
    QVector<Shape> lines = candidateShapes_;
    if (lines.isEmpty() || lines.size() != sourcePoints_.size()) {
        status_.text = QStringLiteral("Extrude failed to create geometry");
        status_.canCommit = false;
        publish(context);
        return;
    }

    DocumentTransaction transaction = context.beginTransaction();
    if (weldEnabled_) {
        quint64 nextWeldGroup = 1;
        for (int objectIndex = 0;
             objectIndex < context.document().size(); ++objectIndex) {
            const Shape *shape = context.document().shape(
                context.document().objectIdAt(objectIndex));
            if (shape == nullptr) {
                continue;
            }
            for (const quint64 group : shape->controlPointWeldGroups) {
                if (group >= nextWeldGroup &&
                    group < std::numeric_limits<quint64>::max()) {
                    nextWeldGroup = group + 1;
                }
            }
        }

        const auto controlPointCount = [](const Shape &shape) -> int {
            if (shape.geometryType == GeometryType::Point) {
                return shape.points.size();
            }
            if (shape.geometryType == GeometryType::NurbsSurface) {
                return shape.nurbsSurface.controlPoints.size();
            }
            if (shape.geometryType == GeometryType::PolyCurve) {
                int count = 0;
                for (const Shape::NurbsCurve2D &component : shape.components) {
                    count += component.controlPoints.size();
                }
                return count;
            }
            switch (shape.geometryType) {
            case GeometryType::Line:
            case GeometryType::Arc:
            case GeometryType::Bezier:
            case GeometryType::Nurbs:
            case GeometryType::Rectangle:
            case GeometryType::Circle:
            case GeometryType::Ellipse:
            case GeometryType::Polygon:
                return shape.nurbs.controlPoints.size();
            default:
                return 0;
            }
        };
        const auto ensureGroups = [&controlPointCount](Shape *shape) {
            if (shape == nullptr) {
                return false;
            }
            const int count = controlPointCount(*shape);
            if (count <= 0) {
                return false;
            }
            if (shape->controlPointWeldGroups.isEmpty()) {
                shape->controlPointWeldGroups.fill(0, count);
            }
            return shape->controlPointWeldGroups.size() == count;
        };
        const auto assignGroup = [&nextWeldGroup](Shape *sourceShape,
                                                  int sourceIndex,
                                                  Shape *extrudedShape,
                                                  int extrudedIndex) {
            if (sourceShape == nullptr || extrudedShape == nullptr ||
                sourceIndex < 0 ||
                sourceIndex >= sourceShape->controlPointWeldGroups.size() ||
                extrudedIndex < 0 ||
                extrudedIndex >= extrudedShape->controlPointWeldGroups.size()) {
                return false;
            }
            quint64 group = sourceShape->controlPointWeldGroups[sourceIndex];
            if (group == 0) {
                if (nextWeldGroup == 0 ||
                    nextWeldGroup == std::numeric_limits<quint64>::max()) {
                    return false;
                }
                group = nextWeldGroup++;
                sourceShape->controlPointWeldGroups[sourceIndex] = group;
            }
            extrudedShape->controlPointWeldGroups[extrudedIndex] = group;
            return true;
        };

        for (int index = 0; index < sourcePoints_.size(); ++index) {
            SourcePoint &source = sourcePoints_[index];
            if (!source.objectId.isValid() ||
                source.solidFaceIndex >= 0 ||
                !source.surface.controlPoints.isEmpty() ||
                !lines[index].controlPointWeldGroups.isEmpty()) {
                continue;
            }
            const Shape *sourceShapeRead =
                context.document().shape(source.objectId);
            if (sourceShapeRead == nullptr ||
                controlPointCount(*sourceShapeRead) <= 0) {
                continue;
            }
            const int surfacePointCount =
                lines[index].nurbsSurface.controlPoints.size();
            if (!source.curve.controlPoints.isEmpty()) {
                if (surfacePointCount !=
                    source.sourceControlPointIndices.size() * 2) {
                    continue;
                }
                bool hasValidSourceMapping = true;
                for (const int sourceIndex :
                     source.sourceControlPointIndices) {
                    hasValidSourceMapping = hasValidSourceMapping &&
                        sourceIndex >= 0 &&
                        sourceIndex < controlPointCount(*sourceShapeRead);
                }
                if (!hasValidSourceMapping) {
                    continue;
                }
            } else if (source.sourceControlPointIndex < 0) {
                continue;
            } else if (source.sourceControlPointIndex >=
                       controlPointCount(*sourceShapeRead)) {
                continue;
            }
            if (!sourceShapeRead->controlPointWeldGroups.isEmpty() &&
                sourceShapeRead->controlPointWeldGroups.size() !=
                    controlPointCount(*sourceShapeRead)) {
                continue;
            }
            Shape *sourceShape = transaction.editGeometry(source.objectId);
            if (sourceShape == nullptr || !ensureGroups(sourceShape)) {
                transaction.rollback();
                status_.text = QStringLiteral("Extrude failed to weld source geometry");
                status_.canCommit = false;
                publish(context);
                return;
            }
            if (!source.curve.controlPoints.isEmpty()) {
                lines[index].controlPointWeldGroups.fill(0, surfacePointCount);
                bool mappedAll = true;
                for (int curvePoint = 0;
                     curvePoint < source.sourceControlPointIndices.size();
                     ++curvePoint) {
                    mappedAll = assignGroup(
                                    sourceShape,
                                    source.sourceControlPointIndices[curvePoint],
                                    &lines[index], curvePoint * 2) && mappedAll;
                }
                if (!mappedAll) {
                    transaction.rollback();
                    status_.text = QStringLiteral("Extrude failed to weld spline boundary");
                    status_.canCommit = false;
                    publish(context);
                    return;
                }
            } else if (source.sourceControlPointIndex >= 0) {
                if (lines[index].geometryType != GeometryType::Line ||
                    !validateNurbsCurve(lines[index].nurbs)) {
                    continue;
                }
                lines[index].controlPointWeldGroups.fill(
                    0, lines[index].nurbs.controlPoints.size());
                if (!assignGroup(sourceShape, source.sourceControlPointIndex,
                                 &lines[index], 0)) {
                    transaction.rollback();
                    status_.text = QStringLiteral("Extrude failed to weld source point");
                    status_.canCommit = false;
                    publish(context);
                    return;
                }
            }
        }
    }

    QVector<ObjectId> createdObjectIds;
    QVector<CompletedFaceExtrusion> faceExtrusions;
    for (int index = 0; index < sourcePoints_.size(); ++index) {
        const SourcePoint &source = sourcePoints_[index];
        const bool faceSource = !source.surface.controlPoints.isEmpty() ||
                                source.solidFaceIndex >= 0;
        if (faceSource && weldEnabled_) {
            // Only connected extrusion may replace the source geometry.
            const SceneObject *sceneObject =
                context.document().object(source.objectId);
            if (sceneObject == nullptr) {
                transaction.rollback();
                status_.text = QStringLiteral("Extrude failed to create geometry");
                status_.canCommit = false;
                publish(context);
                return;
            }
            const Point3D placement = sceneObject->placementTranslation;
            if ((placement.x != 0.0 || placement.y != 0.0 || placement.z != 0.0) &&
                !transaction.setObjectPlacementTranslation(
                    source.objectId, Point3D{})) {
                transaction.rollback();
                status_.text = QStringLiteral("Extrude failed to create geometry");
                status_.canCommit = false;
                publish(context);
                return;
            }
            if (!transaction.replaceGeometry(source.objectId, lines[index])) {
                transaction.rollback();
                status_.text = QStringLiteral("Extrude failed to create geometry");
                status_.canCommit = false;
                publish(context);
                return;
            }
            faceExtrusions.append({source.objectId,
                generatedFaceIndices.value(index,1)});
        } else {
            const ObjectId createdObjectId = transaction.addShape(
                lines[index], context.document().containerId(source.objectId));
            if (!createdObjectId.isValid()) {
                transaction.rollback();
                status_.text = QStringLiteral("Extrude failed to create geometry");
                status_.canCommit = false;
                publish(context);
                return;
            }
            createdObjectIds.append(createdObjectId);
            if (faceSource) faceExtrusions.append({createdObjectId, 1});
        }
    }
    if (!context.commitTransaction(transaction)) {
        status_.text = QStringLiteral("Extrude failed to create geometry");
        status_.canCommit = false;
        publish(context);
        return;
    }

    completedExtrusionObjectIds_ = std::move(createdObjectIds);
    completedFaceExtrusions_ = std::move(faceExtrusions);
    const Point3D committedDisplacement=subtract(cursorPoint_,sourcePoints_.first().worldPoint);
    for (const CompletedFaceExtrusion &face : completedFaceExtrusions_) {
        DebugLog::instance().write(
            QStringLiteral("extrude committed object=%1 face=%2 weld=%3 manifold=%4 displacement=(%5,%6,%7)")
                .arg(face.objectId.value()).arg(face.faceIndex)
                .arg(weldEnabled_).arg(manifoldEnabled_)
                .arg(committedDisplacement.x,0,'g',17)
                .arg(committedDisplacement.y,0,'g',17)
                .arg(committedDisplacement.z,0,'g',17));
    }
    sourcePoints_.clear();
    candidateShapes_.clear();
    candidateFaceIndices_.clear();
    displayShapes_.clear();
    displayFaceIndices_.clear();
    candidateError_.clear();
    hasLastInput_ = false;
    hasCursorPoint_ = false;
    snap_ = {};
    status_.state = ToolLifecycleState::Completed;
    status_.canCommit = false;
    status_.text = QStringLiteral("Extrude created %1 object%2")
                       .arg(lines.size())
                       .arg(lines.size() == 1 ? QString() : QStringLiteral("s"));
    publish(context);
    context.finishTool(ToolId::Select);
}

ToolPreview PointExtrudeTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = inputFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(inputFrame_);
    result.planeLocked = result.hasWorkPlaneFrame;
    result.snap = snap_;
    result.overridesSnap = true;
    result.worldCursorPoint = cursorPoint_;
    result.hasCursorPoint = hasCursorPoint_;
    result.cursorVisible = hasCursorPoint_;
    result.statusText = status_.text;
    if (!sourcePoints_.isEmpty() && hasCursorPoint_) {
        result.worldPoints.reserve(sourcePoints_.size() + 1);
        for (const SourcePoint &source : sourcePoints_) {
            result.worldPoints.append(source.worldPoint);
        }
        result.worldPoints.append(cursorPoint_);
        if (!displayShapes_.isEmpty()) {
            result.shapes = displayShapes_;
            result.extrusionPreviewFaceIndices = displayFaceIndices_;
            if (!result.shapes.isEmpty()) {
                for (const SourcePoint &source : sourcePoints_) {
                    if (weldEnabled_ && (!source.surface.controlPoints.isEmpty() ||
                         source.solidFaceIndex >= 0) &&
                        source.objectId.isValid()) {
                        result.hiddenObjectIds.append(source.objectId);
                    }
                }
            }
        }
    }
    if (status_.state == ToolLifecycleState::Completed) {
        result.completedExtrusionObjectIds = completedExtrusionObjectIds_;
        result.completedFaceExtrusions = completedFaceExtrusions_;
    }
    return result;
}

ToolStatus PointExtrudeTool::status() const
{
    return status_;
}

Point3D PointExtrudeTool::resolveTarget(const ToolInput &input,
                                        ToolContext &context)
{
    snap_ = context.snapEngine().findSpatialSnapPoint(
        context.document(),
        input.screenPosition,
        &sourcePoints_.first().worldPoint,
        context.viewportTransform(),
        input.viewportSize);
    const bool hasSnapPoint = snap_.isValid() && snap_.hasWorldPoint;
    if (!hasSnapPoint && constraintAxisKey_ == 0 && !normalConstraint_) {
        // Free extrusion is a spatial displacement operation. The original
        // point's plane must not limit dragging to its perspective horizon.
        // Keep the reference depth and follow the cursor in a camera-facing
        // plane; OSnap above still supplies the exact depth of scene targets.
        const auto &transform = context.viewportTransform();
        const WorkPlaneFrame cursorFrame = makeWorkPlaneFrameFromNormal(
            sourcePoints_.first().worldPoint, transform.viewDirection(),
            transform.viewUp());
        QPointF localPoint;
        if (transform.screenToWorkPlaneUnclipped(
                input.screenPosition, input.viewportSize, cursorFrame, &localPoint)) {
            return workPlaneFramePointToWorld(localPoint, cursorFrame);
        }
        return hasCursorPoint_ ? cursorPoint_ : sourcePoints_.first().worldPoint;
    }
    const Point3D target = hasSnapPoint
                               ? snap_.worldPoint
                               : workPlaneFramePointToWorld(
                                     input.rawWorldPosition,
                                     isValidWorkPlaneFrame(input.workPlaneFrame)
                                         ? input.workPlaneFrame
                                         : inputFrame_);
    if (constraintAxisKey_ == 0 && !normalConstraint_) {
        return target;
    }

    const Point3D reference = sourcePoints_.first().worldPoint;
    const Point3D direction = normalConstraint_ ? inputFrame_.normal
                                                : axisDirection(constraintAxisKey_);
    Point3D constrainedPoint;
    if (!hasSnapPoint &&
        context.viewportTransform().screenToWorldAxis(
            input.screenPosition, input.viewportSize, reference, direction,
            &constrainedPoint)) {
        return constrainedPoint;
    }

    if (!hasSnapPoint) {
        // A view straight along the axis has no unique ray/axis intersection.
        // Use screen drag distance instead of projecting onto the source
        // point's plane, which would give Z zero movement on an XY plane.
        const auto &transform = context.viewportTransform();
        QPointF referenceScreen;
        const qreal scale = transform.viewScalePixelsPerWorldUnit(input.viewportSize);
        if (scale > 1.0e-12 && transform.worldPointToScreenUnclipped(
                reference, input.viewportSize, &referenceScreen)) {
            const ViewportDirectionProjection projection =
                transform.worldDirectionToView(direction);
            QPointF screenDirection(projection.horizontal, -projection.vertical);
            const qreal magnitude = std::hypot(screenDirection.x(), screenDirection.y());
            screenDirection = magnitude > 1.0e-6
                                  ? screenDirection / magnitude
                                  : QPointF(0.0, -1.0);
            const qreal distance = QPointF::dotProduct(
                input.screenPosition - referenceScreen, screenDirection) / scale;
            return add(reference, multiply(direction, distance));
        }
    }

    const qreal distance = dot(subtract(target, reference), direction);
    constrainedPoint = add(reference, multiply(direction, distance));
    if (hasSnapPoint &&
        length(subtract(target, constrainedPoint)) > 1.0e-6) {
        // The snapped target is off the locked axis, so don't display a snap
        // marker at a point where the constrained extrusion won't end.
        snap_ = {};
    }
    return constrainedPoint;
}

bool PointExtrudeTool::makeLineShape(const SourcePoint &source,
                                     const Point3D &endPoint,
                                     Shape *shape) const
{
    if (shape == nullptr) {
        return false;
    }
    const Point3D direction = normalized(subtract(endPoint, source.worldPoint));
    if (length(direction) <= 1.0e-12) {
        return false;
    }

    const Point3D preferredNormal = source.workPlaneFrame.normal;
    Point3D linePlaneNormal = subtract(
        preferredNormal, multiply(direction, dot(preferredNormal, direction)));
    if (length(linePlaneNormal) <= 1.0e-8) {
        const Point3D helper = std::abs(direction.z) < 0.9
                                   ? Point3D{0.0, 0.0, 1.0}
                                   : Point3D{0.0, 1.0, 0.0};
        linePlaneNormal = subtract(helper,
                                   multiply(direction, dot(helper, direction)));
    }

    const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
        source.worldPoint, linePlaneNormal, direction);
    if (!isValidWorkPlaneFrame(frame)) {
        return false;
    }

    Shape line;
    line.geometryType = GeometryType::Line;
    line.workPlane = WorkPlane::XY;
    line.workPlaneOffset = source.worldPoint.z;
    line.workPlaneFrame = frame;
    line.points = {worldPointToWorkPlaneFrame(source.worldPoint, frame),
                   worldPointToWorkPlaneFrame(endPoint, frame)};
    line.nurbs = makeDegreeOneNurbs(line.points);
    if (!validateNurbsCurve(line.nurbs)) {
        return false;
    }
    *shape = line;
    return true;
}

QVector<Shape> PointExtrudeTool::makeLineShapes(const Point3D &endPoint,
                                              QVector<int> *faceIndices,
                                              QString *error) const
{
    if (error != nullptr) error->clear();
    QVector<Shape> lines;
    if (faceIndices != nullptr) {
        faceIndices->clear();
        for (const SourcePoint &source : sourcePoints_)
            faceIndices->append(weldEnabled_ && source.solidFaceIndex >= 0 ? source.solidFaceIndex : 1);
    }
    lines.reserve(sourcePoints_.size());
    if (sourcePoints_.isEmpty()) {
        return lines;
    }

    // The cursor endpoint defines a translation from the first selected
    // point. Apply that same vector to every selected point so their relative
    // spacing is preserved instead of making all lines converge at one spot.
    const Point3D displacement = subtract(endPoint,
                                         sourcePoints_.first().worldPoint);
    for (const SourcePoint &source : sourcePoints_) {
        Shape line;
        if (source.solidFaceIndex >= 0) {
            if (!weldEnabled_) {
                const auto faces = nurbsSolidFaces(source.solid);
                if (source.solidFaceIndex >= faces.size()) {
                    if (error != nullptr) *error = QStringLiteral("Selected face no longer exists");
                    return {};
                }
                line.geometryType = GeometryType::NurbsSolid;
                line.workPlaneFrame = source.workPlaneFrame;
                if (!makeNurbsExtrusionSolid(faces[source.solidFaceIndex],
                                             displacement, &line.nurbsSolid)) {
                    if (error != nullptr) *error = QStringLiteral("Independent face extrusion requires a nonzero displacement away from its plane");
                    return {};
                }
                lines.append(line);
                continue;
            }
            const SolidFaceExtrusionResult extrusion = planNurbsSolidFaceExtrusion(
                source.solid, source.solidFaceIndex, displacement, manifoldEnabled_);
            if (!extrusion.succeeded()) {
                if (error != nullptr) *error = extrusion.error;
                return {};
            }
            line.geometryType = GeometryType::NurbsSolid;
            line.workPlaneFrame = source.workPlaneFrame;
            line.nurbsSolid = extrusion.solid;
            if (faceIndices != nullptr) (*faceIndices)[lines.size()] = extrusion.faceIndex;
            lines.append(line);
        } else if (!source.surface.controlPoints.isEmpty()) {
            line.geometryType = GeometryType::NurbsSolid;
            line.workPlaneFrame = source.workPlaneFrame;
            if (!makeNurbsExtrusionSolid(source.surface, displacement,
                                         &line.nurbsSolid)) {
                if (error != nullptr) *error = QStringLiteral("Face extrusion requires a planar face and a nonzero displacement away from its plane");
                return {};
            }
            lines.append(line);
        } else if (!source.curve.controlPoints.isEmpty()) {
            line.geometryType = GeometryType::NurbsSurface;
            if (makeNurbsExtrusionSurface(source.curve, source.workPlaneFrame,
                                           displacement, &line.nurbsSurface)) {
                lines.append(line);
            }
        } else if (makeLineShape(source, add(source.worldPoint, displacement), &line)) {
            lines.append(line);
        }
    }
    return lines;
}

void PointExtrudeTool::updateStatus()
{
    status_.state = ToolLifecycleState::Active;
    candidateShapes_.clear();
    candidateFaceIndices_.clear();
    candidateError_.clear();
    if (!sourcePoints_.isEmpty() && hasCursorPoint_) {
        candidateShapes_ = makeLineShapes(cursorPoint_, &candidateFaceIndices_,
                                         &candidateError_);
        if (candidateShapes_.size() != sourcePoints_.size()) {
            candidateShapes_.clear();
            candidateFaceIndices_.clear();
            if (candidateError_.isEmpty()) {
                candidateError_ = QStringLiteral("Could not create geometry for every selected source");
            }
        }
    }
    status_.canCommit = !sourcePoints_.isEmpty() && hasCursorPoint_ &&
                        candidateShapes_.size() == sourcePoints_.size();
    if (status_.canCommit) {
        displayShapes_ = candidateShapes_;
        displayFaceIndices_ = candidateFaceIndices_;
    }
    if (!sourcePoints_.isEmpty() && hasCursorPoint_) {
        const Point3D displacement = subtract(cursorPoint_, sourcePoints_.first().worldPoint);
        const QString diagnostic = QStringLiteral("weld=%1 manifold=%2 result=%3")
            .arg(weldEnabled_).arg(manifoldEnabled_)
            .arg(status_.canCommit ? QStringLiteral("accepted") : candidateError_);
        if (length(displacement) > 1.0e-12 && diagnostic != lastDiagnostic_) {
            for (const SourcePoint &source : sourcePoints_) {
                if (source.solidFaceIndex < 0) continue;
                DebugLog::instance().write(
                    QStringLiteral("extrude candidate object=%1 face=%2 displacement=(%3,%4,%5) %6")
                        .arg(source.objectId.value()).arg(source.solidFaceIndex)
                        .arg(displacement.x,0,'g',17).arg(displacement.y,0,'g',17)
                        .arg(displacement.z,0,'g',17).arg(diagnostic));
            }
            lastDiagnostic_ = diagnostic;
        }
    }
    if (sourcePoints_.isEmpty()) {
        status_.text = QStringLiteral("Extrude: select points, curves, or planar faces first");
    } else if (status_.canCommit) {
        status_.text = QStringLiteral("Extrude: click endpoint; same offset for %1 source%2")
                           .arg(sourcePoints_.size())
                           .arg(sourcePoints_.size() == 1 ? QString() : QStringLiteral("s"));
    } else if (!candidateError_.isEmpty()) {
        status_.text = QStringLiteral("Extrude: %1").arg(candidateError_);
    } else if (std::any_of(
                   sourcePoints_.cbegin(), sourcePoints_.cend(),
                   [](const SourcePoint &source) {
                       return source.solidFaceIndex >= 0;
                   })) {
        status_.text = constraintAxisKey_ != 0
            ? QStringLiteral(
                  "Extrude: move this face along its normal")
            : QStringLiteral(
                  "Extrude: move along the selected face normal");
    } else {
        status_.text = QStringLiteral("Extrude: move endpoint to set the shared offset");
    }
    if (constraintAxisKey_ != 0) {
        status_.text += QStringLiteral(" • %1 axis")
                            .arg(QChar(constraintAxisKey_));
    } else if (normalConstraint_) {
        status_.text += QStringLiteral(" • face normal");
    }
    status_.text += weldEnabled_
        ? QStringLiteral(" • W Weld on")
        : QStringLiteral(" • W Weld off");
    status_.text += manifoldEnabled_
        ? QStringLiteral(" • M Manifold on")
        : QStringLiteral(" • M Manifold off");
}

bool PointExtrudeTool::weldEnabled() const
{
    return weldEnabled_;
}

void PointExtrudeTool::toggleWeld(ToolContext &context)
{
    displayShapes_.clear();
    displayFaceIndices_.clear();
    weldEnabled_ = !weldEnabled_;
    if (!weldEnabled_) manifoldEnabled_ = false;
    updateStatus();
    publish(context);
}

bool PointExtrudeTool::manifoldEnabled() const
{
    return manifoldEnabled_;
}

void PointExtrudeTool::toggleManifold(ToolContext &context)
{
    displayShapes_.clear();
    displayFaceIndices_.clear();
    manifoldEnabled_ = !manifoldEnabled_;
    if (manifoldEnabled_) weldEnabled_ = true;
    updateStatus();
    publish(context);
}

void PointExtrudeTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
