#include "trim_erase_query.h"

#include "core/geometry/curve_erase_intervals.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/shape_mapping.h"
#include "services/sampling/curve_sampler.h"
#include "services/viewport/viewport_transform.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

const EraseCurveSampleCache *findCachedTarget(
    const QVector<EraseCurveSampleCache> *cachedTargets,
    int sourceShapeIndex,
    int componentIndex)
{
    if (cachedTargets == nullptr) {
        return nullptr;
    }
    for (const EraseCurveSampleCache &cachedTarget : *cachedTargets) {
        if (cachedTarget.shapeIndex == sourceShapeIndex &&
            cachedTarget.componentIndex == componentIndex) {
            return &cachedTarget;
        }
    }
    return nullptr;
}

EraseIntersectionParameterResult intersectionsForComponent(
    int componentIndex,
    const NurbsCurve2D &curve,
    const WorkPlaneFrame &workPlaneFrame,
    const EraseCurveSampleCache *cachedTarget,
    const EraseIntersectionResolver &intersectionResolver)
{
    if (cachedTarget != nullptr) {
        return {cachedTarget->intersectionParameters,
                cachedTarget->intersectionObjectIds,
                0,
                0};
    }
    return intersectionResolver
               ? intersectionResolver(componentIndex, curve, workPlaneFrame)
               : EraseIntersectionParameterResult{};
}

} // namespace

bool calculateTrimEraseReplacement(
    const Shape &sourceShape,
    int sourceShapeIndex,
    const QVector<QPointF> &screenStroke,
    const QRectF *trimBox,
    bool wholeObjectOnIntersectionFreeErase,
    int onlyComponentIndex,
    const QVector<EraseCurveSampleCache> *cachedTargets,
    qreal endpointTolerance,
    const CurveSampler &curveSampler,
    const ViewportTransform &viewportTransform,
    const QSize &viewportSize,
    const EraseIntersectionResolver &intersectionResolver,
    TrimEraseShapeQueryResult *result)
{
    if (result == nullptr) {
        return false;
    }
    *result = {};
    const QVector<ShapeNurbsCurveComponent> sourceComponents =
        nurbsCurveComponentsForShape(sourceShape);
    if (sourceComponents.isEmpty()) {
        return false;
    }
    const bool allowIntersectionFreeWholeObjectErase =
        wholeObjectOnIntersectionFreeErase && trimBox == nullptr &&
        std::all_of(sourceComponents.cbegin(), sourceComponents.cend(),
                    [](const ShapeNurbsCurveComponent &component) {
                        return component.curve.dimension == 2;
                    });

    QVector<TrimEraseComponentQueryResult> componentResults;
    QVector<QVector<ParameterInterval>> strokeHitIntervals;
    componentResults.reserve(sourceComponents.size());
    strokeHitIntervals.resize(sourceComponents.size());
    for (const ShapeNurbsCurveComponent &sourceComponent : sourceComponents) {
        TrimEraseComponentQueryResult componentResult;
        componentResult.componentIndex = sourceComponent.componentIndex;
        componentResult.workPlaneFrame = sourceComponent.workPlaneFrame;
        const EraseCurveSampleCache *cachedTarget = findCachedTarget(
            cachedTargets, sourceShapeIndex, sourceComponent.componentIndex);
        componentResult.usedCachedIntersections = cachedTarget != nullptr;
        componentResults.append(std::move(componentResult));
    }

    if (allowIntersectionFreeWholeObjectErase) {
        bool strokeHitsObject = false;
        bool objectHasIntersection = false;
        for (int index = 0; index < sourceComponents.size(); ++index) {
            const ShapeNurbsCurveComponent &sourceComponent =
                sourceComponents[index];
            TrimEraseComponentQueryResult &componentResult = componentResults[index];
            const EraseCurveSampleCache *cachedTarget = findCachedTarget(
                cachedTargets, sourceShapeIndex, sourceComponent.componentIndex);
            const EraseIntersectionParameterResult intersections =
                intersectionsForComponent(sourceComponent.componentIndex,
                                          sourceComponent.curve,
                                          sourceComponent.workPlaneFrame,
                                          cachedTarget,
                                          intersectionResolver);
            componentResult.intersectionParameters = intersections.parameters;
            componentResult.intersectingObjectIds =
                intersections.intersectingObjectIds;
            qreal domainStart = 0.0;
            qreal domainEnd = 0.0;
            const qreal domainLength =
                nurbsParameterDomain(sourceComponent.curve,
                                     &domainStart,
                                     &domainEnd)
                    ? domainEnd - domainStart
                    : 0.0;
            const qreal boundaryTolerance =
                std::max<qreal>(1.0e-12, domainLength * 1.0e-9);
            const bool hasInteriorIntersection = std::any_of(
                intersections.parameters.begin(),
                intersections.parameters.end(),
                [&](qreal parameter) {
                    return parameter > domainStart + boundaryTolerance &&
                           parameter < domainEnd - boundaryTolerance;
                });
            strokeHitIntervals[index] = nurbsEraseIntervalsForStroke(
                sourceComponent.curve,
                sourceComponent.workPlaneFrame,
                screenStroke,
                viewportTransform,
                viewportSize);
            strokeHitsObject = strokeHitsObject ||
                               !strokeHitIntervals[index].isEmpty();
            objectHasIntersection = objectHasIntersection ||
                                    hasInteriorIntersection ||
                                    !intersections.intersectingObjectIds.isEmpty();
        }
        if (strokeHitsObject && !objectHasIntersection) {
            result->changed = true;
            result->erasedWholeObject = true;
            result->components = std::move(componentResults);
            return true;
        }
    }

    QVector<NurbsCurve2D> remainingCurves;
    QVector<WorkPlaneFrame> remainingFrames;
    for (int index = 0; index < sourceComponents.size(); ++index) {
        const ShapeNurbsCurveComponent &sourceComponent = sourceComponents[index];
        TrimEraseComponentQueryResult &componentResult = componentResults[index];
        const EraseCurveSampleCache *cachedTarget = findCachedTarget(
            cachedTargets, sourceShapeIndex, sourceComponent.componentIndex);
        if (onlyComponentIndex >= 0 &&
            sourceComponent.componentIndex != onlyComponentIndex) {
            remainingCurves.append(sourceComponent.curve);
            remainingFrames.append(sourceComponent.workPlaneFrame);
            continue;
        }

        EraseIntersectionParameterResult intersections;
        if (cachedTarget != nullptr) {
            intersections = intersectionsForComponent(
                sourceComponent.componentIndex,
                sourceComponent.curve,
                sourceComponent.workPlaneFrame,
                cachedTarget,
                intersectionResolver);
        } else if (allowIntersectionFreeWholeObjectErase) {
            intersections.parameters = componentResult.intersectionParameters;
            intersections.intersectingObjectIds =
                componentResult.intersectingObjectIds;
        }

        QVector<ParameterInterval> hitIntervals;
        if (trimBox != nullptr) {
            SampledNurbsCurve2D fallbackSamples;
            const SampledNurbsCurve2D *samples =
                cachedTarget != nullptr ? &cachedTarget->sampled : &fallbackSamples;
            if (cachedTarget == nullptr &&
                !curveSampler.sampleNurbsCurve(sourceComponent.curve,
                                               sourceComponent.workPlaneFrame,
                                               viewportTransform,
                                               viewportSize,
                                               &fallbackSamples)) {
                remainingCurves.append(sourceComponent.curve);
                remainingFrames.append(sourceComponent.workPlaneFrame);
                continue;
            }
            hitIntervals = nurbsCurveIntervalsInsideScreenBox(
                sourceComponent.curve,
                *samples,
                sourceComponent.workPlaneFrame,
                *trimBox,
                viewportTransform,
                viewportSize);
        } else if (allowIntersectionFreeWholeObjectErase) {
            hitIntervals = strokeHitIntervals[index];
        } else {
            hitIntervals = nurbsEraseIntervalsForStroke(
                sourceComponent.curve,
                sourceComponent.workPlaneFrame,
                screenStroke,
                viewportTransform,
                viewportSize);
        }

        if (cachedTarget == nullptr &&
            !allowIntersectionFreeWholeObjectErase) {
            intersections = intersectionsForComponent(
                sourceComponent.componentIndex,
                sourceComponent.curve,
                sourceComponent.workPlaneFrame,
                nullptr,
                intersectionResolver);
        }
        componentResult.intersectionParameters = intersections.parameters;
        componentResult.intersectingObjectIds = intersections.intersectingObjectIds;
        componentResult.removedIntervals = boundCurveEraseIntervals(
            sourceComponent.curve, hitIntervals, intersections.parameters);
        if (componentResult.removedIntervals.isEmpty()) {
            remainingCurves.append(sourceComponent.curve);
            remainingFrames.append(sourceComponent.workPlaneFrame);
            continue;
        }

        QVector<NurbsCurve2D> keptCurves;
        if (!keepNurbsCurveOutsideIntervals(sourceComponent.curve,
                                            componentResult.removedIntervals,
                                            &keptCurves)) {
            return false;
        }
        result->changed = true;
        for (const NurbsCurve2D &keptCurve : keptCurves) {
            remainingCurves.append(keptCurve);
            remainingFrames.append(sourceComponent.workPlaneFrame);
        }
    }

    result->components = std::move(componentResults);
    if (!result->changed) {
        return true;
    }
    return rebuildShapeFromCurveEraseFragments(sourceShape,
                                               remainingCurves,
                                               remainingFrames,
                                               endpointTolerance,
                                               &result->replacementShapes);
}

} // namespace classiCAD
