#include "nurbs_surface_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

qreal basisValue(const QVector<double> &knots,
                 int index,
                 int degree,
                 qreal parameter)
{
    if (degree == 0) {
        return knots[index] <= parameter && parameter < knots[index + 1]
                   ? 1.0
                   : 0.0;
    }

    qreal value = 0.0;
    const qreal leftDenominator = knots[index + degree] - knots[index];
    if (std::abs(leftDenominator) > 1.0e-12) {
        value += (parameter - knots[index]) / leftDenominator *
                 basisValue(knots, index, degree - 1, parameter);
    }

    const qreal rightDenominator = knots[index + degree + 1] -
                                   knots[index + 1];
    if (std::abs(rightDenominator) > 1.0e-12) {
        value += (knots[index + degree + 1] - parameter) /
                 rightDenominator *
                 basisValue(knots, index + 1, degree - 1, parameter);
    }
    return value;
}

} // namespace

bool PreparedNurbsSurfaceEvaluator::prepare(const NurbsSurface3D &surface)
{
    surface_ = surface;
    fullKnotsU_.clear();
    fullKnotsV_.clear();
    uStart_ = 0.0;
    uEnd_ = 0.0;
    vStart_ = 0.0;
    vEnd_ = 0.0;
    valid_ = false;
    if (!validateNurbsSurface(surface_)) {
        return false;
    }

    fullKnotsU_ = expandedNurbsSurfaceKnotVector(surface_.knotsU);
    fullKnotsV_ = expandedNurbsSurfaceKnotVector(surface_.knotsV);
    uStart_ = fullKnotsU_[surface_.degreeU];
    uEnd_ = fullKnotsU_[surface_.controlVertexCountU];
    vStart_ = fullKnotsV_[surface_.degreeV];
    vEnd_ = fullKnotsV_[surface_.controlVertexCountV];
    valid_ = uEnd_ > uStart_ && vEnd_ > vStart_;
    return valid_;
}

bool PreparedNurbsSurfaceEvaluator::isValid() const
{
    return valid_;
}

bool PreparedNurbsSurfaceEvaluator::parameterDomains(qreal *uStart,
                                                      qreal *uEnd,
                                                      qreal *vStart,
                                                      qreal *vEnd) const
{
    if (!valid_ || (uStart == nullptr && uEnd == nullptr &&
                    vStart == nullptr && vEnd == nullptr)) {
        return false;
    }
    if (uStart != nullptr) {
        *uStart = uStart_;
    }
    if (uEnd != nullptr) {
        *uEnd = uEnd_;
    }
    if (vStart != nullptr) {
        *vStart = vStart_;
    }
    if (vEnd != nullptr) {
        *vEnd = vEnd_;
    }
    return true;
}

bool PreparedNurbsSurfaceEvaluator::evaluate(qreal u,
                                             qreal v,
                                             Point3D *point) const
{
    if (!valid_ || point == nullptr || !std::isfinite(u) || !std::isfinite(v)) {
        return false;
    }

    u = std::clamp(u, uStart_, uEnd_);
    v = std::clamp(v, vStart_, vEnd_);
    if (u >= uEnd_) {
        u = std::nextafter(uEnd_, uStart_);
    }
    if (v >= vEnd_) {
        v = std::nextafter(vEnd_, vStart_);
    }

    QVector<qreal> uBasis(surface_.controlVertexCountU);
    QVector<qreal> vBasis(surface_.controlVertexCountV);
    for (int uIndex = 0; uIndex < surface_.controlVertexCountU; ++uIndex) {
        uBasis[uIndex] = basisValue(fullKnotsU_,
                                    uIndex,
                                    surface_.degreeU,
                                    u);
    }
    for (int vIndex = 0; vIndex < surface_.controlVertexCountV; ++vIndex) {
        vBasis[vIndex] = basisValue(fullKnotsV_,
                                    vIndex,
                                    surface_.degreeV,
                                    v);
    }

    Point3D numerator{};
    qreal denominator = 0.0;
    for (int uIndex = 0; uIndex < surface_.controlVertexCountU; ++uIndex) {
        if (uBasis[uIndex] == 0.0) {
            continue;
        }
        for (int vIndex = 0; vIndex < surface_.controlVertexCountV; ++vIndex) {
            const int controlIndex = uIndex * surface_.controlVertexCountV + vIndex;
            const qreal basis = uBasis[uIndex] * vBasis[vIndex] *
                (surface_.rational ? surface_.weights[controlIndex] : 1.0);
            const Point3D &controlPoint = surface_.controlPoints[controlIndex];
            numerator.x += controlPoint.x * basis;
            numerator.y += controlPoint.y * basis;
            numerator.z += controlPoint.z * basis;
            denominator += basis;
        }
    }
    if (std::abs(denominator) <= 1.0e-12) {
        return false;
    }
    point->x = numerator.x / denominator;
    point->y = numerator.y / denominator;
    point->z = numerator.z / denominator;
    return true;
}

} // namespace classiCAD
