#pragma once

#include "core/document/shape.h"

namespace classiCAD {

// Reflect a shape across the infinite line defined by axisStart and axisEnd.
// The curve degree, order, weights, knots, and parameter domain are preserved;
// only Euclidean positions are transformed.
bool mirrorShapeAcrossLine(const Shape &source,
                           const QPointF &axisStart,
                           const QPointF &axisEnd,
                           Shape *mirrored);

// Translates a shape by a displacement expressed in an input workplane. A
// PolyCurve with component frames moves those frames in world space while
// retaining each component's local NURBS data.
bool translateShapeGeometry(Shape *shape,
                            const QPointF &delta,
                            const WorkPlaneFrame &inputFrame);

// Move either duplicated endpoint CV of a closed NURBS curve while keeping
// the seam closed. Circle and ellipse shapes remain seam-linked even after
// an earlier edit has already separated the two CVs.
bool setClosedNurbsSeamControlPoint(Shape *shape,
                                    int controlPointIndex,
                                    const QPointF &position);

// Applies the viewport's uniform or one-axis scale semantics to exact stored
// geometry. Surface depth relative to surfaceFrame is preserved.
bool scaleShapeGeometry(Shape *shape,
                        const QPointF &base,
                        const QPointF &axisDirection,
                        qreal factor,
                        bool oneDimensional,
                        const WorkPlaneFrame &surfaceFrame);

// Rotates planar geometry by rotating its workplane frame and rotates surface
// control vertices in world space. Stored curve parameters and UV trims stay
// unchanged.
bool rotateShapeGeometry(Shape *shape,
                         const Point3D &pivot,
                         const Point3D &axis,
                         qreal angle);

} // namespace classiCAD
