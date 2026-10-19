#pragma once

#include "core/document/shape.h"

namespace classiCAD {

// World-space CV positions in the persistent weld-group index order.
QVector<Point3D> extrusionWeldControlPoints(const Shape &shape,
                                           Point3D placement = {});

// Attach the staged extrusion to coincident existing CVs. Degree-one curve
// spans and degree-one surface patch boundaries are split exactly at
// matching CVs, including partial overlaps. Objects remain separate. Placement
// translations are read in world space and are never baked into geometry.
// Solid cage vertices use the same unique ordering as component selection.
// Rectangular solid edges split into exact 2x2 boundary patches. Curved spans
// and face interiors are not split by this operation.
bool weldExtrusionDestination(QVector<Shape> *shapes,
                              const QVector<int> &extrudedShapeIndices,
                              const QVector<Point3D> &placements = {},
                              qreal tolerance = 1.0e-6,
                              QString *failure = nullptr);

// Combine adjacent standalone planar sweeps by removing identical opposing
// walls. The result arrays use shape indices; owners identifies the first
// object in each connected region and capIndices maps each original top cap.
// Geometry must already share a world frame (no placement translations).
bool mergeAdjacentExtrusionSolids(QVector<Shape> *shapes,
                                  const QVector<int> &eligibleIndices,
                                  QVector<int> *owners,
                                  QVector<int> *capIndices,
                                  QString *failure = nullptr);

} // namespace classiCAD
