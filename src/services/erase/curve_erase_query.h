#pragma once

#include "core/geometry/curve_geometry_data.h"
#include "core/geometry/nurbs_curve.h"
#include "core/geometry/work_plane.h"
#include "core/document/object_id.h"
#include "services/sampling/curve_sample_data.h"

#include <QRectF>
#include <QSize>

namespace classiCAD {

class ViewportTransform;
class Document;

struct EraseCurveIntersectionCandidate {
    ObjectId objectId = ObjectId::invalid();
    int componentIndex = -1;
    NurbsCurve2D curve;
    WorkPlaneFrame workPlaneFrame;
};

struct ErasePointIntersectionCandidate {
    ObjectId objectId = ObjectId::invalid();
    Point3D worldPosition;
};

struct EraseIntersectionCandidates {
    QVector<EraseCurveIntersectionCandidate> curves;
    QVector<ErasePointIntersectionCandidate> points;
};

struct EraseIntersectionParameterResult {
    QVector<qreal> parameters;
    QVector<ObjectId> intersectingObjectIds;
    int nurbsSeedSolves = 0;
    int pointChecks = 0;
};

EraseIntersectionCandidates makeEraseIntersectionCandidates(
    const Document &document,
    const QVector<EraseCurveSampleCache> *sceneCache = nullptr);

EraseIntersectionParameterResult findEraseIntersectionParameters(
    const NurbsCurve2D &sourceCurve,
    const WorkPlaneFrame &sourceWorkPlaneFrame,
    ObjectId sourceObjectId,
    int sourceComponentIndex,
    const QVector<EraseCurveIntersectionCandidate> &otherCurves,
    const QVector<ErasePointIntersectionCandidate> &otherPoints,
    qreal endpointProximityTolerance = 0.0);

qreal distanceToNurbsCurveOnScreen(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const QPointF &screenPosition,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize);

QVector<ParameterInterval> nurbsEraseIntervalsForStrokeSegment(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const QPointF &strokeStart,
    const QPointF &strokeEnd,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize);

QVector<ParameterInterval> nurbsEraseIntervalsForStroke(
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const QVector<QPointF> &stroke,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize);

QVector<ParameterInterval> nurbsCurveIntervalsInsideScreenBox(
    const NurbsCurve2D &curve,
    const SampledNurbsCurve2D &sampled,
    const WorkPlaneFrame &workPlaneFrame,
    const QRectF &box,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize);

} // namespace classiCAD
