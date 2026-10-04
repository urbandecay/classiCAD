#include "nurbs_surface_factory.h"

#include "curve_evaluator.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

bool makeNurbsExtrusionSurface(const NurbsCurve2D &curve,
                               const WorkPlaneFrame &curveFrame,
                               const Point3D &displacement,
                               NurbsSurface3D *surface)
{
    if (surface == nullptr || !validateNurbsCurve(curve) ||
        !isValidWorkPlaneFrame(curveFrame) ||
        !std::isfinite(displacement.x) || !std::isfinite(displacement.y) ||
        !std::isfinite(displacement.z) ||
        std::hypot(displacement.x,
                   std::hypot(displacement.y, displacement.z)) <= 1.0e-12) {
        return false;
    }

    NurbsSurface3D result;
    result.degreeU = curve.degree;
    result.orderU = curve.order;
    result.degreeV = 1;
    result.orderV = 2;
    result.controlVertexCountU = curve.controlPoints.size();
    result.controlVertexCountV = 2;
    result.rational = curve.rational;
    result.knotsU = curve.knots;
    result.knotsV = {0.0, 1.0};
    result.controlPoints.reserve(result.controlVertexCountU * 2);
    result.weights.reserve(result.controlVertexCountU * 2);

    for (int index = 0; index < curve.controlPoints.size(); ++index) {
        const Point3D base = workPlaneFramePointToWorld(curve.controlPoints[index],
                                                        curveFrame);
        const double weight = curve.weights[index];
        result.controlPoints.append(base);
        result.controlPoints.append({base.x + displacement.x,
                                     base.y + displacement.y,
                                     base.z + displacement.z});
        result.weights.append(weight);
        result.weights.append(weight);
    }

    if (!validateNurbsSurface(result)) {
        return false;
    }
    *surface = std::move(result);
    return true;
}

bool makeNurbsPlanarFillSurface(const NurbsCurve2D &curve,
                                const WorkPlaneFrame &curveFrame,
                                NurbsSurface3D *surface)
{
    if (surface == nullptr || !validateNurbsCurve(curve) ||
        !isValidWorkPlaneFrame(curveFrame)) {
        return false;
    }

    qreal curveStart = 0.0;
    qreal curveEnd = 0.0;
    QPointF firstPoint;
    QPointF lastPoint;
    if (!nurbsParameterDomain(curve, &curveStart, &curveEnd) ||
        !evaluateNurbsPoint(curve, curveStart, &firstPoint) ||
        !evaluateNurbsPoint(curve, curveEnd, &lastPoint)) {
        return false;
    }

    qreal minimumX = curve.controlPoints.first().x();
    qreal maximumX = minimumX;
    qreal minimumY = curve.controlPoints.first().y();
    qreal maximumY = minimumY;
    for (const QPointF &point : curve.controlPoints) {
        minimumX = std::min(minimumX, point.x());
        maximumX = std::max(maximumX, point.x());
        minimumY = std::min(minimumY, point.y());
        maximumY = std::max(maximumY, point.y());
    }
    const qreal scale = std::max<qreal>(
        {1.0, maximumX - minimumX, maximumY - minimumY});
    const qreal tolerance = scale * 1.0e-8;
    if (std::hypot(firstPoint.x() - lastPoint.x(),
                   firstPoint.y() - lastPoint.y()) > tolerance ||
        maximumX - minimumX <= tolerance || maximumY - minimumY <= tolerance) {
        return false;
    }

    NurbsSurface3D result;
    result.degreeU = 1;
    result.orderU = 2;
    result.degreeV = 1;
    result.orderV = 2;
    result.controlVertexCountU = 2;
    result.controlVertexCountV = 2;
    result.rational = false;
    result.knotsU = {0.0, 1.0};
    result.knotsV = {0.0, 1.0};
    result.controlPoints = {
        workPlaneFramePointToWorld({minimumX, minimumY}, curveFrame),
        workPlaneFramePointToWorld({minimumX, maximumY}, curveFrame),
        workPlaneFramePointToWorld({maximumX, minimumY}, curveFrame),
        workPlaneFramePointToWorld({maximumX, maximumY}, curveFrame)};
    result.weights = {1.0, 1.0, 1.0, 1.0};

    NurbsSurfaceTrimLoop outerLoop;
    outerLoop.curve = curve;
    for (QPointF &point : outerLoop.curve.controlPoints) {
        point.setX((point.x() - minimumX) / (maximumX - minimumX));
        point.setY((point.y() - minimumY) / (maximumY - minimumY));
    }
    result.trimLoops.append(std::move(outerLoop));

    if (!validateNurbsSurface(result)) {
        return false;
    }
    *surface = std::move(result);
    return true;
}

} // namespace classiCAD
