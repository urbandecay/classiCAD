#pragma once

#include "core/geometry/nurbs_curve.h"
#include "core/geometry/work_plane.h"

namespace classiCAD {

struct NurbsCurveFrameGroup {
    QVector<NurbsCurve2D> curves;
    QVector<WorkPlaneFrame> workPlaneFrames;
};

// Orders connected planar curve components and reverses individual curves as
// needed while preserving each curve's rational definition and domain.
bool orderConnectedNurbsCurves(const QVector<NurbsCurve2D> &input,
                              QVector<NurbsCurve2D> *ordered,
                              qreal tolerance);

// Finds endpoint-connected groups and returns each group's joined ordering
// when one exists. Branching components retain their original group order.
QVector<QVector<NurbsCurve2D>> connectedNurbsCurveGroups(
    const QVector<NurbsCurve2D> &curves,
    qreal orderingTolerance);
QVector<NurbsCurveFrameGroup> connectedNurbsCurveGroupsInWorld(
    const QVector<NurbsCurve2D> &curves,
    const QVector<WorkPlaneFrame> &workPlaneFrames,
    qreal orderingTolerance);

bool orderConnectedNurbsCurvesInWorld(
    const QVector<NurbsCurve2D> &input,
    const QVector<WorkPlaneFrame> &frames,
    QVector<NurbsCurve2D> *ordered,
    QVector<WorkPlaneFrame> *orderedFrames,
    qreal tolerance);

bool closeConnectedNurbsCurveGaps(QVector<NurbsCurve2D> *components,
                                  qreal tolerance);
bool connectedNurbsCurvesAreContinuous(
    const QVector<NurbsCurve2D> &components,
    qreal tolerance);

bool closeConnectedNurbsCurveGapsInWorld(
    QVector<NurbsCurve2D> *components,
    QVector<WorkPlaneFrame> *frames,
    qreal tolerance);
bool connectedNurbsCurvesAreContinuousInWorld(
    const QVector<NurbsCurve2D> &components,
    const QVector<WorkPlaneFrame> &frames,
    qreal tolerance);

// Splits degree-one polylines into exact knot-span lines, then merges
// collinear line components whose intervals overlap or meet within tolerance.
// Returns the number of component merges performed.
int fuseOverlappingNurbsLineComponents(QVector<NurbsCurve2D> *components,
                                       qreal tolerance);
int fuseOverlappingNurbsLineComponentsInWorld(
    QVector<NurbsCurve2D> *components,
    QVector<WorkPlaneFrame> *frames,
    qreal tolerance);

} // namespace classiCAD
