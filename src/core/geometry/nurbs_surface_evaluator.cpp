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

bool PreparedNurbsSurfaceBasisGrid::prepare(
    const NurbsSurface3D &surface,
    const QVector<QPointF> &parameters)
{
    valid_ = false;
    degreeU_ = surface.degreeU;
    degreeV_ = surface.degreeV;
    controlVertexCountU_ = surface.controlVertexCountU;
    controlVertexCountV_ = surface.controlVertexCountV;
    rational_ = surface.rational;
    knotsU_ = surface.knotsU;
    knotsV_ = surface.knotsV;
    weights_ = surface.weights;
    uParameters_.clear();
    vParameters_.clear();
    uRows_.clear();
    vRows_.clear();
    uBasisValues_.clear();
    vBasisValues_.clear();
    sampleIndices_.clear();

    if (!validateNurbsSurface(surface) || parameters.isEmpty()) {
        return false;
    }

    const QVector<double> fullKnotsU =
        expandedNurbsSurfaceKnotVector(surface.knotsU);
    const QVector<double> fullKnotsV =
        expandedNurbsSurfaceKnotVector(surface.knotsV);
    const qreal uStart = fullKnotsU[surface.degreeU];
    const qreal uEnd = fullKnotsU[surface.controlVertexCountU];
    const qreal vStart = fullKnotsV[surface.degreeV];
    const qreal vEnd = fullKnotsV[surface.controlVertexCountV];

    uParameters_.reserve(parameters.size());
    vParameters_.reserve(parameters.size());
    for (const QPointF &parameter : parameters) {
        if (!std::isfinite(parameter.x()) || !std::isfinite(parameter.y())) {
            return false;
        }
        uParameters_.append(std::clamp(parameter.x(), uStart, uEnd));
        vParameters_.append(std::clamp(parameter.y(), vStart, vEnd));
    }
    std::sort(uParameters_.begin(), uParameters_.end());
    uParameters_.erase(std::unique(uParameters_.begin(), uParameters_.end()),
                       uParameters_.end());
    std::sort(vParameters_.begin(), vParameters_.end());
    vParameters_.erase(std::unique(vParameters_.begin(), vParameters_.end()),
                       vParameters_.end());

    const auto prepareAxis = [](const QVector<double> &knots,
                                const QVector<qreal> &parameters,
                                int degree,
                                int controlVertexCount,
                                qreal domainStart,
                                qreal domainEnd,
                                QVector<AxisBasisRow> *rows,
                                QVector<qreal> *values) {
        rows->reserve(parameters.size());
        values->reserve(parameters.size() * (degree + 1));
        const int lastSpan = controlVertexCount - 1;
        for (qreal parameter : parameters) {
            if (parameter >= domainEnd) {
                parameter = std::nextafter(domainEnd, domainStart);
            }

            int low = degree;
            int high = lastSpan + 1;
            int span = (low + high) / 2;
            while (parameter < knots[span] || parameter >= knots[span + 1]) {
                if (parameter < knots[span]) {
                    high = span;
                } else {
                    low = span;
                }
                const int nextSpan = (low + high) / 2;
                if (nextSpan == span) {
                    break;
                }
                span = nextSpan;
            }

            QVector<qreal> basis(degree + 1, 0.0);
            QVector<qreal> left(degree + 1, 0.0);
            QVector<qreal> right(degree + 1, 0.0);
            basis[0] = 1.0;
            for (int order = 1; order <= degree; ++order) {
                left[order] = parameter - knots[span + 1 - order];
                right[order] = knots[span + order] - parameter;
                qreal saved = 0.0;
                for (int basisIndex = 0; basisIndex < order; ++basisIndex) {
                    const qreal denominator =
                        right[basisIndex + 1] + left[order - basisIndex];
                    const qreal temporary = std::abs(denominator) > 1.0e-12
                        ? basis[basisIndex] / denominator
                        : 0.0;
                    basis[basisIndex] = saved +
                        right[basisIndex + 1] * temporary;
                    saved = left[order - basisIndex] * temporary;
                }
                basis[order] = saved;
            }

            AxisBasisRow row;
            row.firstControlVertex = span - degree;
            row.valuesOffset = values->size();
            row.valueCount = degree + 1;
            *values += basis;
            rows->append(row);
        }
    };
    prepareAxis(fullKnotsU, uParameters_, surface.degreeU,
                surface.controlVertexCountU, uStart, uEnd,
                &uRows_, &uBasisValues_);
    prepareAxis(fullKnotsV, vParameters_, surface.degreeV,
                surface.controlVertexCountV, vStart, vEnd,
                &vRows_, &vBasisValues_);

    sampleIndices_.reserve(parameters.size());
    for (const QPointF &parameter : parameters) {
        const qreal u = std::clamp(parameter.x(), uStart, uEnd);
        const qreal v = std::clamp(parameter.y(), vStart, vEnd);
        const auto uPosition = std::lower_bound(uParameters_.cbegin(),
                                                uParameters_.cend(), u);
        const auto vPosition = std::lower_bound(vParameters_.cbegin(),
                                                vParameters_.cend(), v);
        if (uPosition == uParameters_.cend() || *uPosition != u ||
            vPosition == vParameters_.cend() || *vPosition != v) {
            sampleIndices_.clear();
            return false;
        }
        sampleIndices_.append({static_cast<int>(uPosition - uParameters_.cbegin()),
                               static_cast<int>(vPosition - vParameters_.cbegin())});
    }
    valid_ = true;
    return true;
}

bool PreparedNurbsSurfaceBasisGrid::isValid() const
{
    return valid_;
}

int PreparedNurbsSurfaceBasisGrid::sampleCount() const
{
    return sampleIndices_.size();
}

bool PreparedNurbsSurfaceBasisGrid::evaluate(const NurbsSurface3D &surface,
                                             int sampleIndex,
                                             Point3D *point) const
{
    if (!valid_ || point == nullptr || sampleIndex < 0 ||
        sampleIndex >= sampleIndices_.size() ||
        surface.degreeU != degreeU_ || surface.degreeV != degreeV_ ||
        surface.controlVertexCountU != controlVertexCountU_ ||
        surface.controlVertexCountV != controlVertexCountV_ ||
        surface.rational != rational_ || surface.knotsU != knotsU_ ||
        surface.knotsV != knotsV_ || surface.weights != weights_ ||
        surface.controlPoints.size() !=
            controlVertexCountU_ * controlVertexCountV_) {
        return false;
    }

    const SampleBasisIndices sample = sampleIndices_[sampleIndex];
    if (sample.u < 0 || sample.u >= uRows_.size() || sample.v < 0 ||
        sample.v >= vRows_.size()) {
        return false;
    }
    const AxisBasisRow &uRow = uRows_[sample.u];
    const AxisBasisRow &vRow = vRows_[sample.v];
    Point3D numerator{};
    qreal denominator = 0.0;
    for (int uOffset = 0; uOffset < uRow.valueCount; ++uOffset) {
        const int uIndex = uRow.firstControlVertex + uOffset;
        const qreal uBasis = uBasisValues_[uRow.valuesOffset + uOffset];
        for (int vOffset = 0; vOffset < vRow.valueCount; ++vOffset) {
            const int vIndex = vRow.firstControlVertex + vOffset;
            const int controlIndex = uIndex * controlVertexCountV_ + vIndex;
            qreal basis = uBasis * vBasisValues_[vRow.valuesOffset + vOffset];
            if (rational_) {
                basis *= surface.weights[controlIndex];
            }
            const Point3D &controlPoint = surface.controlPoints[controlIndex];
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
