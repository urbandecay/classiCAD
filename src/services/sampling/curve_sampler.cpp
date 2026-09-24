#include "curve_sampler.h"

#include "core/geometry/curve_evaluator.h"

#include <algorithm>

namespace classiCAD {

bool CurveSampler::sampleNurbsCurve(const Shape::NurbsCurve2D &curve,
                                    const ViewportTransform &transform,
                                    const QSize &viewportSize,
                                    SampledNurbsCurve2D *sampled) const
{
    if (sampled == nullptr || !validateNurbsCurve(curve)) {
        return false;
    }

    sampled->parameters.clear();
    sampled->screenPoints.clear();
    sampled->segmentBounds.clear();
    sampled->bounds = QRectF();

    qreal domainStart = 0.0;
    qreal domainEnd = 0.0;
    if (!nurbsParameterDomain(curve, &domainStart, &domainEnd)) {
        return false;
    }

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < curve.controlPoints.size(); ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }

    // Keep erase-preview density consistent across all callers: linear spans
    // need more samples while higher-degree spans get a lower cap per span.
    const int samplesPerSpan = curve.degree <= 1 ? 64 : 32;
    constexpr int maximumSampleCount = 2048;
    const int samplesForEachSpan =
        std::max(1,
                 std::min(samplesPerSpan,
                          maximumSampleCount / std::max(1, nonZeroSpans)));
    sampled->parameters.reserve(nonZeroSpans * samplesForEachSpan + 1);
    sampled->screenPoints.reserve(nonZeroSpans * samplesForEachSpan + 1);

    for (int spanIndex = curve.degree;
         spanIndex < curve.controlPoints.size();
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        for (int sample = 0; sample <= samplesForEachSpan; ++sample) {
            if (spanIndex > curve.degree && sample == 0) {
                continue;
            }

            const qreal fraction = static_cast<qreal>(sample) /
                                   samplesForEachSpan;
            const qreal parameter = spanStart +
                                    (spanEnd - spanStart) * fraction;
            QPointF worldPoint;
            if (!evaluateNurbsPoint(curve, parameter, &worldPoint)) {
                sampled->parameters.clear();
                sampled->screenPoints.clear();
                sampled->segmentBounds.clear();
                sampled->bounds = QRectF();
                return false;
            }
            sampled->parameters.append(parameter);
            sampled->screenPoints.append(
                transform.worldToScreen(worldPoint, viewportSize));
        }
    }

    if (sampled->screenPoints.size() < 2) {
        return false;
    }

    sampled->segmentBounds.reserve(sampled->screenPoints.size() - 1);
    sampled->bounds = QRectF(sampled->screenPoints.first(),
                             sampled->screenPoints.first());
    for (int sample = 1; sample < sampled->screenPoints.size(); ++sample) {
        const QPointF &first = sampled->screenPoints[sample - 1];
        const QPointF &second = sampled->screenPoints[sample];
        sampled->segmentBounds.append(QRectF(first, second).normalized());
        sampled->bounds = sampled->bounds.united(QRectF(second, second));
    }

    return sampled->parameters.size() >= 2;
}

QVector<Shape::NurbsCurve2D> CurveSampler::curvesForShape(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::PolyCurve) {
        return shape.components;
    }
    if (validateNurbsCurve(shape.nurbs)) {
        return {shape.nurbs};
    }
    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        return {makeDegreeOneNurbs(shape.points)};
    }
    if ((shape.geometryType == GeometryType::Bezier ||
         shape.geometryType == GeometryType::Nurbs) && shape.points.size() >= 2) {
        return {makeBezierNurbs(shape.points)};
    }
    if (shape.geometryType == GeometryType::Rectangle && shape.points.size() >= 2) {
        QVector<QPointF> vertices;
        if (shape.points.size() >= 4) {
            vertices = {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
        } else {
            const QPointF first = shape.points[0];
            const QPointF second = shape.points[1];
            vertices = {first,
                        QPointF(second.x(), first.y()),
                        second,
                        QPointF(first.x(), second.y())};
        }

        QVector<Shape::NurbsCurve2D> curves;
        curves.reserve(vertices.size());
        for (int index = 0; index < vertices.size(); ++index) {
            curves.append(makeDegreeOneNurbs(
                {vertices[index], vertices[(index + 1) % vertices.size()]}));
        }
        return curves;
    }
    return {};
}

QVector<EraseCurveSampleCache> CurveSampler::sampleDocument(
    const Document &document,
    const ViewportTransform &transform,
    const QSize &viewportSize) const
{
    QVector<EraseCurveSampleCache> caches;
    for (int shapeIndex = 0; shapeIndex < document.size(); ++shapeIndex) {
        if (!document.isObjectVisible(document.objectIdAt(shapeIndex))) {
            continue;
        }
        const QVector<Shape::NurbsCurve2D> curves = curvesForShape(document[shapeIndex]);
        for (int componentIndex = 0; componentIndex < curves.size(); ++componentIndex) {
            if (!validateNurbsCurve(curves[componentIndex])) {
                continue;
            }

            EraseCurveSampleCache cache;
            cache.shapeIndex = shapeIndex;
            cache.componentIndex = componentIndex;
            cache.curve = curves[componentIndex];
            if (sampleNurbsCurve(cache.curve,
                                 transform,
                                 viewportSize,
                                 &cache.sampled)) {
                caches.append(cache);
            }
        }
    }
    return caches;
}

} // namespace classiCAD
