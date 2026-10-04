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

#include <cmath>

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

void PointExtrudeTool::begin(ToolContext &context)
{
    sourcePoints_.clear();
    inputFrame_ = {};
    cursorPoint_ = {};
    lastInput_ = {};
    constraintAxisKey_ = 0;
    normalConstraint_ = false;
    hasLastInput_ = false;
    hasCursorPoint_ = false;
    snap_ = {};

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
            SourcePoint source;
            source.objectId = selectedObjectId;
            source.worldPoint = shapePointToWorld(*selectedShape,
                                                  selectedShape->points.first());
            source.workPlaneFrame = shapeWorkPlaneFrame(*selectedShape);
            source.workPlaneFrame.origin = source.worldPoint;
            if (isValidWorkPlaneFrame(source.workPlaneFrame)) {
                sourcePoints_.append(source);
            }
        } else if (selectedShape->geometryType == GeometryType::NurbsSurface) {
            SourcePoint source;
            if (!nurbsSolidBaseFrame(selectedShape->nurbsSurface,
                                     &source.workPlaneFrame)) continue;
            source.objectId = selectedObjectId;
            source.surface = selectedShape->nurbsSurface;
            source.worldPoint = source.workPlaneFrame.origin;
            sourcePoints_.append(source);
        } else {
            const auto curves = context.curveSampler().curvesForShape(*selectedShape);
            for (int index = 0; index < curves.size(); ++index) {
                if (!validateNurbsCurve(curves[index])) continue;
                SourcePoint source;
                source.objectId = selectedObjectId;
                source.curve = curves[index];
                source.workPlaneFrame = selectedShape->geometryType == GeometryType::PolyCurve
                    ? shapeComponentWorkPlaneFrame(*selectedShape, index)
                    : shapeWorkPlaneFrame(*selectedShape);
                if (!isValidWorkPlaneFrame(source.workPlaneFrame)) continue;
                source.worldPoint = workPlaneFramePointToWorld(
                    source.curve.controlPoints.first(), source.workPlaneFrame);
                sourcePoints_.append(source);
            }
        }
    }

    if (!sourcePoints_.isEmpty()) {
        normalConstraint_ = !sourcePoints_.first().surface.controlPoints.isEmpty();
        inputFrame_ = sourcePoints_.first().workPlaneFrame;
        inputFrame_.origin = sourcePoints_.first().worldPoint;
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

    const QVector<Shape> lines = makeLineShapes(cursorPoint_);
    if (lines.isEmpty() ||
        !context.commitShapes(ToolId::PointExtrude, lines)) {
        status_.text = QStringLiteral("Extrude failed to create geometry");
        status_.canCommit = false;
        publish(context);
        return;
    }

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
        if (status_.canCommit) {
            result.shapes = makeLineShapes(cursorPoint_);
        }
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

QVector<Shape> PointExtrudeTool::makeLineShapes(const Point3D &endPoint) const
{
    QVector<Shape> lines;
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
        if (!source.surface.controlPoints.isEmpty()) {
            line.geometryType = GeometryType::NurbsSolid;
            line.workPlaneFrame = source.workPlaneFrame;
            if (!makeNurbsExtrusionSolid(source.surface, displacement,
                                         &line.nurbsSolid)) return {};
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
    status_.canCommit = !sourcePoints_.isEmpty() && hasCursorPoint_ &&
                        !makeLineShapes(cursorPoint_).isEmpty();
    if (sourcePoints_.isEmpty()) {
        status_.text = QStringLiteral("Extrude: select points, curves, or planar faces first");
    } else if (status_.canCommit) {
        status_.text = QStringLiteral("Extrude: click endpoint; same offset for %1 source%2")
                           .arg(sourcePoints_.size())
                           .arg(sourcePoints_.size() == 1 ? QString() : QStringLiteral("s"));
    } else {
        status_.text = QStringLiteral("Extrude: move endpoint to set the shared offset");
    }
    if (constraintAxisKey_ != 0) {
        status_.text += QStringLiteral(" • %1 axis")
                            .arg(QChar(constraintAxisKey_));
    } else if (normalConstraint_) {
        status_.text += QStringLiteral(" • face normal");
    }
}

void PointExtrudeTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
