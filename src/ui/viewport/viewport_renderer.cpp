#include "viewport_renderer.h"

#include "core/geometry/arc_curve_factory.h"
#include "core/geometry/curve_evaluator.h"
#include "blender_grid_scale.h"
#include "blender_grid_frame.h"
#include "line_type_style.h"
#include "services/dimensions/dimension_font.h"
#include "services/dimensions/dimension_layout.h"

#include <QFont>
#include <QLineF>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>

namespace classiCAD {
namespace {

constexpr int kFadeBucketCount = 256;
constexpr int kGridLineHalfCount = 75;

using FadedLineBins = std::array<QVector<QLineF>, kFadeBucketCount>;

qreal smoothstep(qreal minimum, qreal maximum, qreal value)
{
    if (maximum <= minimum) {
        return value < minimum ? 0.0 : 1.0;
    }
    const qreal amount = std::clamp((value - minimum) / (maximum - minimum),
                                    0.0,
                                    1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

qreal perspectiveGridFade(const Point3D &worldPoint,
                          const Point3D &cameraPosition,
                          const Point3D &planeNormal,
                          qreal farClipDistance,
                          const BlenderGridAppearance &appearance)
{
    const qreal cameraX = cameraPosition.x - worldPoint.x;
    const qreal cameraY = cameraPosition.y - worldPoint.y;
    const qreal cameraZ = cameraPosition.z - worldPoint.z;
    const qreal distance = std::sqrt(cameraX * cameraX + cameraY * cameraY +
                                     cameraZ * cameraZ);
    if (!std::isfinite(distance) || distance <= 1.0e-12) {
        return 0.0;
    }

    const qreal facing = std::clamp(
        std::abs((cameraX * planeNormal.x + cameraY * planeNormal.y +
                  cameraZ * planeNormal.z) /
                 distance),
        0.0,
        1.0);
    const qreal grazingFade = 1.0 - std::pow(
        1.0 - facing, appearance.grazingFadeExponent);
    const qreal distanceFade =
        1.0 - smoothstep(farClipDistance * appearance.farFadeStart,
                         farClipDistance * appearance.farFadeEnd,
                         distance);
    return grazingFade * distanceFade;
}

template<typename AdditionalFade>
void appendPerspectiveLine(const ViewportTransform &transform,
                           const QSize &viewportSize,
                           WorkPlane plane,
                           qreal planeOffset,
                           const Point3D &cameraPosition,
                           const Point3D &planeNormal,
                           qreal farClipDistance,
                           const QPointF &start,
                           const QPointF &end,
                           qreal opacity,
                           int segmentCount,
                           FadedLineBins &lineBins,
                           const BlenderGridAppearance &appearance,
                           AdditionalFade additionalFade)
{
    if (opacity <= 0.0 || segmentCount < 1) {
        return;
    }

    const Point3D worldStart = workPlanePointToWorld(start, plane, planeOffset);
    const Point3D worldEnd = workPlanePointToWorld(end, plane, planeOffset);
    QPointF projectedStart;
    QPointF projectedEnd;
    const bool projectedWholeLine =
        transform.worldPointToScreen(worldStart, viewportSize, &projectedStart) &&
        transform.worldPointToScreen(worldEnd, viewportSize, &projectedEnd);
    if (projectedWholeLine) {
        constexpr qreal maximumSegmentLengthPixels = 3.0;
        constexpr int maximumSegmentCount = 128;
        const qreal requestedSegmentCount = std::ceil(
            QLineF(projectedStart, projectedEnd).length() /
            maximumSegmentLengthPixels);
        const int screenSegmentCount =
            requestedSegmentCount >= maximumSegmentCount
                ? maximumSegmentCount
                : std::max(1, static_cast<int>(requestedSegmentCount));
        segmentCount = std::clamp(std::max(segmentCount, screenSegmentCount),
                                  1,
                                  maximumSegmentCount);
    }

    qreal screenDistanceAlongLine = 0.0;
    for (int segment = 0; segment < segmentCount; ++segment) {
        const qreal firstT = static_cast<qreal>(segment) / segmentCount;
        const qreal secondT = static_cast<qreal>(segment + 1) / segmentCount;
        const QPointF firstPoint = start + (end - start) * firstT;
        const QPointF secondPoint = start + (end - start) * secondT;
        QPointF firstScreen;
        QPointF secondScreen;
        if (!transform.worldPointToScreen(workPlanePointToWorld(firstPoint,
                                                                plane,
                                                                planeOffset),
                                          viewportSize,
                                          &firstScreen) ||
            !transform.worldPointToScreen(workPlanePointToWorld(secondPoint,
                                                                plane,
                                                                planeOffset),
                                          viewportSize,
                                          &secondScreen)) {
            continue;
        }
        const qreal screenSegmentLength = QLineF(firstScreen, secondScreen).length();
        const qreal screenMidpointDistance =
            screenDistanceAlongLine + screenSegmentLength * 0.5;
        screenDistanceAlongLine += screenSegmentLength;

        const QPointF midpoint = start + (end - start) * ((firstT + secondT) * 0.5);
        const Point3D worldMidpoint = workPlanePointToWorld(midpoint,
                                                            plane,
                                                            planeOffset);
        const qreal alpha = std::clamp(
            opacity * additionalFade(midpoint) *
                perspectiveGridFade(worldMidpoint,
                                    cameraPosition,
                                    planeNormal,
                                    farClipDistance,
                                    appearance),
            0.0,
            1.0);
        if (alpha <= 0.0) {
            continue;
        }
        // Blender stipples fragments below 10% alpha into 4px dashes. Sample
        // that coverage at short screen-space segments to avoid a bright pileup
        // where perspective grid lines converge at the horizon.
        if (appearance.lowAlphaStipple &&
            alpha < appearance.stippleThreshold) {
            const qreal dashPhase = std::fmod(screenMidpointDistance /
                                                  appearance.stippleDashWidth,
                                              1.0);
            if (alpha / appearance.stippleThreshold < dashPhase) {
                continue;
            }
        }
        const int bucket = std::clamp(
            static_cast<int>(std::lround(alpha * (kFadeBucketCount - 1))),
            1,
            kFadeBucketCount - 1);
        lineBins[bucket].append(QLineF(firstScreen, secondScreen));
    }
}

void drawFadedLineBins(QPainter &painter,
                       const FadedLineBins &lineBins,
                       const QColor &color,
                       qreal baseOpacity,
                       bool additiveIterations = true)
{
    constexpr std::array<qreal, 4> additivePassOpacity = {1.0, 0.5, 0.25, 0.125};
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    const int passCount = additiveIterations
                              ? static_cast<int>(additivePassOpacity.size())
                              : 1;
    for (int pass = 0; pass < passCount; ++pass) {
        const qreal passOpacity = additivePassOpacity[static_cast<size_t>(pass)];
        for (int bucket = 0; bucket < kFadeBucketCount; ++bucket) {
            const QVector<QLineF> &lines = lineBins[bucket];
            if (lines.isEmpty()) {
                continue;
            }
            QColor fadedColor = color;
            fadedColor.setAlphaF(baseOpacity * passOpacity *
                                 static_cast<qreal>(bucket) /
                                     (kFadeBucketCount - 1));
            painter.setPen(QPen(fadedColor, 1.0));
            painter.drawLines(lines.constData(), lines.size());
        }
    }
    painter.restore();
}

template<typename RadialFade>
void appendOrthographicGridLine(const ViewportTransform &transform,
                                const QSize &viewportSize,
                                WorkPlane plane,
                                qreal planeOffset,
                                const QPointF &start,
                                const QPointF &end,
                                qreal opacity,
                                int segmentCount,
                                FadedLineBins &lineBins,
                                const BlenderGridAppearance &appearance,
                                RadialFade radialFade)
{
    if (opacity <= 0.0 || segmentCount < 1) {
        return;
    }
    QPointF projectedStart;
    QPointF projectedEnd;
    if (transform.worldPointToScreenUnclipped(
            workPlanePointToWorld(start, plane, planeOffset),
            viewportSize,
            &projectedStart) &&
        transform.worldPointToScreenUnclipped(
            workPlanePointToWorld(end, plane, planeOffset),
            viewportSize,
            &projectedEnd)) {
        const qreal desiredCount = std::ceil(QLineF(projectedStart, projectedEnd).length() /
                                             3.0);
        segmentCount = std::clamp(std::max(segmentCount,
                                           static_cast<int>(desiredCount)),
                                  1,
                                  128);
    }

    const Point3D normal = workPlaneNormal(plane);
    const Point3D direction = transform.viewDirection();
    const qreal directionLength = std::sqrt(direction.x * direction.x +
                                            direction.y * direction.y +
                                            direction.z * direction.z);
    const qreal normalLength = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                         normal.z * normal.z);
    const qreal facing = directionLength > 1.0e-12 && normalLength > 1.0e-12
                             ? std::clamp(std::abs((direction.x * normal.x +
                                                   direction.y * normal.y +
                                                   direction.z * normal.z) /
                                                  (directionLength * normalLength)),
                                          0.0,
                                          1.0)
                             : 1.0;
    const qreal grazingFade = 1.0 - std::pow(
        1.0 - facing, appearance.grazingFadeExponent);
    qreal screenDistanceAlongLine = 0.0;
    for (int segment = 0; segment < segmentCount; ++segment) {
        const qreal firstT = static_cast<qreal>(segment) / segmentCount;
        const qreal secondT = static_cast<qreal>(segment + 1) / segmentCount;
        const QPointF first = start + (end - start) * firstT;
        const QPointF second = start + (end - start) * secondT;
        QPointF firstScreen;
        QPointF secondScreen;
        if (!transform.worldPointToScreenUnclipped(
                workPlanePointToWorld(first, plane, planeOffset),
                viewportSize,
                &firstScreen) ||
            !transform.worldPointToScreenUnclipped(
                workPlanePointToWorld(second, plane, planeOffset),
                viewportSize,
                &secondScreen)) {
            continue;
        }
        const qreal screenLength = QLineF(firstScreen, secondScreen).length();
        const qreal distanceOnLine = screenDistanceAlongLine + screenLength * 0.5;
        screenDistanceAlongLine += screenLength;
        const QPointF midpoint = (first + second) * 0.5;
        const qreal alpha = std::clamp(opacity * radialFade(midpoint) * grazingFade,
                                       0.0,
                                       1.0);
        if (alpha <= 0.0) {
            continue;
        }
        if (appearance.lowAlphaStipple && alpha < appearance.stippleThreshold) {
            const qreal phase = std::fmod(distanceOnLine / appearance.stippleDashWidth,
                                          1.0);
            if (alpha / appearance.stippleThreshold < phase) {
                continue;
            }
        }
        const int bucket = std::clamp(
            static_cast<int>(std::lround(alpha * (kFadeBucketCount - 1))),
            1,
            kFadeBucketCount - 1);
        lineBins[bucket].append(QLineF(firstScreen, secondScreen));
    }
}

} // namespace

ViewportRenderer::ViewportRenderer(const ViewportTransform &transform,
                                   const CurveHitTester &curveHitTester)
    : transform_(transform)
    , curveHitTester_(curveHitTester)
{
}

void ViewportRenderer::setSmoothCurveDisplay(bool enabled)
{
    smoothCurveDisplay_ = enabled;
}

void ViewportRenderer::setGridAppearance(const BlenderGridAppearance &appearance)
{
    if (isValidBlenderGridAppearance(appearance)) {
        gridAppearance_ = appearance;
    }
}

void ViewportRenderer::setGridBaseStep(qreal baseGridStep)
{
    if (std::isfinite(baseGridStep) && baseGridStep > 0.0) {
        gridBaseStep_ = baseGridStep;
    }
}

void ViewportRenderer::setArchitecturalDimensionFont(bool enabled)
{
    architecturalDimensionFont_ = enabled;
}

QPointF ViewportRenderer::worldToScreen(const QPointF &world,
                                        const QSize &viewportSize) const
{
    return transform_.worldToScreen(world, viewportSize);
}

QPointF ViewportRenderer::screenToWorld(const QPointF &screen,
                                        const QSize &viewportSize) const
{
    return transform_.screenToWorld(screen, viewportSize);
}

bool ViewportRenderer::isValidNurbsCurve(const Shape::NurbsCurve2D &curve) const
{
    return validateNurbsCurve(curve);
}

bool ViewportRenderer::evaluateNurbsPoint(const Shape::NurbsCurve2D &curve,
                                          qreal parameter,
                                          QPointF *point) const
{
    return classiCAD::evaluateNurbsPoint(curve, parameter, point);
}

void ViewportRenderer::drawGrid(QPainter &painter,
                                const QSize &viewportSize) const
{
    const BlenderGridFrame gridFrame = resolveBlenderGridFrame(transform_,
                                                               viewportSize);
    const WorkPlane plane = gridFrame.plane;
    const qreal planeOffset = gridFrame.planeOffset;
    if (transform_.isPerspectiveEnabled()) {
        const qreal farClipDistance = transform_.cameraPreferences().clipEnd;
        const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
        const Point3D planeNormal = workPlaneNormal(plane);
        const QPointF gridCenter = gridFrame.cameraRelativeOffset;
        const qreal gridFocusDistance = gridFrame.focusDistance;

        const BlenderGridLevelSelection gridLevel = selectBlenderGridLevel(
            gridFocusDistance, false, gridBaseStep_);
        const qreal levelBlend = gridLevel.levelFraction;
        const std::array<qreal, 3> levelOpacities = {
            1.0 - levelBlend,
            1.0,
            1.0,
        };

        std::array<FadedLineBins, 3> lineBins;

        for (int level = 2; level >= 0; --level) {
            const qreal step = blenderGridStepAtLevel(gridLevel, level);
            if (!std::isfinite(step) || step <= 1.0e-9) {
                continue;
            }
            const qreal originU = std::round(gridCenter.x() / step) * step;
            const qreal originV = std::round(gridCenter.y() / step) * step;
            const qreal radius = kGridLineHalfCount * step;
            const qreal layerOpacity = levelOpacities[static_cast<size_t>(level)];
            const auto radialFade = [originU, originV, radius](const QPointF &point) {
                const qreal radialDistance =
                    std::hypot(point.x() - originU, point.y() - originV);
                return 1.0 - std::clamp(radialDistance / radius, 0.0, 1.0);
            };
            const qreal emphasis = level == 0
                                       ? 0.0
                                       : (level == 1 ? 1.0 - levelBlend : 1.0);
            const QColor levelColor = QColor::fromRgbF(
                gridAppearance_.gridColor.redF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.redF() * emphasis,
                gridAppearance_.gridColor.greenF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.greenF() * emphasis,
                gridAppearance_.gridColor.blueF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.blueF() * emphasis,
                gridAppearance_.gridColor.alphaF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.alphaF() * emphasis);
            const qreal lineOpacity = layerOpacity * levelColor.alphaF() *
                                      gridAppearance_.opacity;

            for (int lineIndex = -kGridLineHalfCount;
                 lineIndex <= kGridLineHalfCount;
                 ++lineIndex) {
                const qreal fixedU = originU + lineIndex * step;
                const qreal fixedV = originV + lineIndex * step;
                const qreal varyingExtent = std::sqrt(std::max<qreal>(
                    0.0,
                    radius * radius - lineIndex * lineIndex * step * step));

                appendPerspectiveLine(
                    transform_, viewportSize, plane, planeOffset,
                    cameraPosition, planeNormal, farClipDistance,
                    QPointF(fixedU, originV - varyingExtent),
                    QPointF(fixedU, originV + varyingExtent),
                    lineOpacity, 24,
                    lineBins[static_cast<size_t>(level)],
                    gridAppearance_, radialFade);
                appendPerspectiveLine(
                    transform_, viewportSize, plane, planeOffset,
                    cameraPosition, planeNormal, farClipDistance,
                    QPointF(originU - varyingExtent, fixedV),
                    QPointF(originU + varyingExtent, fixedV),
                    lineOpacity, 24,
                    lineBins[static_cast<size_t>(level)],
                    gridAppearance_, radialFade);
            }
        }

        painter.save();
        painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
        for (int level = 2; level >= 0; --level) {
            const qreal emphasis = level == 0
                                       ? 0.0
                                       : (level == 1 ? 1.0 - levelBlend : 1.0);
            const QColor levelColor = QColor::fromRgbF(
                gridAppearance_.gridColor.redF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.redF() * emphasis,
                gridAppearance_.gridColor.greenF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.greenF() * emphasis,
                gridAppearance_.gridColor.blueF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.blueF() * emphasis,
                gridAppearance_.gridColor.alphaF() * (1.0 - emphasis) +
                    gridAppearance_.emphasisColor.alphaF() * emphasis);
            const qreal levelAlpha = level == 0
                                         ? 1.0 - levelBlend
                                         : 1.0;
            const qreal colorAlpha = levelColor.alphaF() * gridAppearance_.opacity;
            QColor opaqueLevelColor = levelColor;
            opaqueLevelColor.setAlpha(255);
            drawFadedLineBins(painter,
                              lineBins[static_cast<size_t>(level)],
                              opaqueLevelColor,
                              levelAlpha * colorAlpha);
        }
        painter.restore();
        return;
    }

    const BlenderGridLevelSelection gridLevel = selectBlenderGridLevel(
        gridFrame.focusDistance,
        gridFrame.fixedAxisOrthographic,
        gridBaseStep_);
    const qreal levelBlend = gridLevel.levelFraction;
    painter.save();
    painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
    for (int level = 2; level >= 0; --level) {
        const qreal step = blenderGridStepAtLevel(gridLevel, level);
        if (!std::isfinite(step) || step <= 1.0e-12) {
            continue;
        }
        const qreal radius = kGridLineHalfCount * step;
        const QPointF offset = gridFrame.cameraRelativeOffset;
        const qreal firstU = std::round(offset.x() / step) * step - radius;
        const qreal firstV = std::round(offset.y() / step) * step - radius;
        const qreal levelAlpha = level == 0 ? 1.0 - levelBlend : 1.0;
        const qreal emphasis = level == 0
                                   ? 0.0
                                   : (level == 1 ? 1.0 - levelBlend : 1.0);
        const QColor levelColor = QColor::fromRgbF(
            gridAppearance_.gridColor.redF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.redF() * emphasis,
            gridAppearance_.gridColor.greenF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.greenF() * emphasis,
            gridAppearance_.gridColor.blueF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.blueF() * emphasis,
            gridAppearance_.gridColor.alphaF() * (1.0 - emphasis) +
                gridAppearance_.emphasisColor.alphaF() * emphasis);
        const qreal pixelFade = blenderOrthographicGridPixelFade(
            step * transform_.viewScalePixelsPerWorldUnit(viewportSize));
        const qreal lineOpacity = levelAlpha * levelColor.alphaF() *
                                  gridAppearance_.opacity * pixelFade;
        const auto radialFade = [offset, radius, this](const QPointF &point) {
            const qreal normalizedU = (point.x() - offset.x()) / radius;
            const qreal normalizedV = (point.y() - offset.y()) / radius;
            const qreal fade = std::clamp(1.0 - normalizedU * normalizedU -
                                              normalizedV * normalizedV,
                                          0.0,
                                          1.0);
            return std::pow(fade,
                            gridAppearance_.orthographicEdgeFadeExponent);
        };
        FadedLineBins lineBins;
        for (int lineIndex = -kGridLineHalfCount;
             lineIndex <= kGridLineHalfCount;
             ++lineIndex) {
            const qreal fixedV = firstV + (lineIndex + kGridLineHalfCount) * step;
            if (std::abs(fixedV) > 2.0e-7) {
                appendOrthographicGridLine(
                    transform_, viewportSize, plane, planeOffset,
                    QPointF(offset.x() - radius, fixedV),
                    QPointF(offset.x() + radius, fixedV),
                    lineOpacity, 24, lineBins, gridAppearance_, radialFade);
            }
            const qreal fixedU = firstU + (lineIndex + kGridLineHalfCount) * step;
            if (std::abs(fixedU) > 2.0e-7) {
                appendOrthographicGridLine(
                    transform_, viewportSize, plane, planeOffset,
                    QPointF(fixedU, offset.y() - radius),
                    QPointF(fixedU, offset.y() + radius),
                    lineOpacity, 24, lineBins, gridAppearance_, radialFade);
            }
        }
        QColor opaqueLevelColor = levelColor;
        opaqueLevelColor.setAlpha(255);
        drawFadedLineBins(painter, lineBins, opaqueLevelColor, 1.0, false);
    }
    painter.restore();
}

void ViewportRenderer::drawOrigin(QPainter &painter,
                                  const QSize &viewportSize) const
{
    const BlenderGridFrame gridFrame = resolveBlenderGridFrame(transform_,
                                                               viewportSize);
    const WorkPlane plane = gridFrame.plane;
    const qreal planeOffset = gridFrame.planeOffset;
    QPointF corners[4];
    const QPointF screenCorners[] = {{0.0, 0.0},
                                     {static_cast<qreal>(viewportSize.width()), 0.0},
                                     {static_cast<qreal>(viewportSize.width()),
                                      static_cast<qreal>(viewportSize.height())},
                                     {0.0, static_cast<qreal>(viewportSize.height())}};
    int cornerCount = 0;
    for (int index = 0; index < 4; ++index) {
        const bool resolved = transform_.isPerspectiveEnabled()
                                  ? transform_.screenToWorkPlane(
                                        screenCorners[index], viewportSize,
                                        plane, planeOffset,
                                        &corners[cornerCount])
                                  : transform_.screenToWorkPlaneUnclipped(
                                        screenCorners[index], viewportSize,
                                        plane, planeOffset,
                                        &corners[cornerCount]);
        if (!resolved) {
            continue;
        }
        ++cornerCount;
    }
    if (cornerCount < 2) {
        return;
    }
    qreal minimumU = corners[0].x();
    qreal maximumU = corners[0].x();
    qreal minimumV = corners[0].y();
    qreal maximumV = corners[0].y();
    for (int index = 0; index < cornerCount; ++index) {
        minimumU = std::min(minimumU, corners[index].x());
        maximumU = std::max(maximumU, corners[index].x());
        minimumV = std::min(minimumV, corners[index].y());
        maximumV = std::max(maximumV, corners[index].y());
    }

    if (transform_.isPerspectiveEnabled()) {
        const qreal farClipDistance = transform_.cameraPreferences().clipEnd;
        minimumU = std::max(minimumU, -farClipDistance);
        maximumU = std::min(maximumU, farClipDistance);
        minimumV = std::max(minimumV, -farClipDistance);
        maximumV = std::min(maximumV, farClipDistance);
        if (minimumU > maximumU || minimumV > maximumV) {
            return;
        }

        const Point3D cameraPosition = transform_.cameraPosition(viewportSize);
        const Point3D planeNormal = workPlaneNormal(plane);
        FadedLineBins uAxisBins;
        FadedLineBins vAxisBins;
        const auto noAdditionalFade = [](const QPointF &) { return 1.0; };
        const int globalUAxis = plane == WorkPlane::YZ ? 1 : 0;
        const int globalVAxis = plane == WorkPlane::XY ? 1 : 2;
        const auto axisColorForIndex = [this](int axis) {
            if (axis == 0) {
                return gridAppearance_.axisXColor;
            }
            if (axis == 1) {
                return gridAppearance_.axisYColor;
            }
            return gridAppearance_.axisZColor;
        };
        if (gridFrame.visibleAxes[static_cast<size_t>(globalUAxis)]) {
            const QColor color = axisColorForIndex(globalUAxis);
            appendPerspectiveLine(
                transform_, viewportSize, plane, planeOffset, cameraPosition,
                planeNormal, farClipDistance,
                QPointF(minimumU, 0.0), QPointF(maximumU, 0.0),
                color.alphaF() * gridAppearance_.opacity, 128, uAxisBins,
                gridAppearance_, noAdditionalFade);
        }
        if (gridFrame.visibleAxes[static_cast<size_t>(globalVAxis)]) {
            const QColor color = axisColorForIndex(globalVAxis);
            appendPerspectiveLine(
                transform_, viewportSize, plane, planeOffset, cameraPosition,
                planeNormal, farClipDistance,
                QPointF(0.0, minimumV), QPointF(0.0, maximumV),
                color.alphaF() * gridAppearance_.opacity, 128, vAxisBins,
                gridAppearance_, noAdditionalFade);
        }

        painter.save();
        painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
        const std::array<QColor, 3> axisColors{
            gridAppearance_.axisXColor,
            gridAppearance_.axisYColor,
            gridAppearance_.axisZColor,
        };
        QColor uColor = axisColors[static_cast<size_t>(globalUAxis)];
        QColor vColor = axisColors[static_cast<size_t>(globalVAxis)];
        const qreal uOpacity = uColor.alphaF() * gridAppearance_.opacity;
        const qreal vOpacity = vColor.alphaF() * gridAppearance_.opacity;
        uColor.setAlpha(255);
        vColor.setAlpha(255);
        drawFadedLineBins(painter,
                          uAxisBins,
                          uColor,
                          uOpacity);
        drawFadedLineBins(painter,
                          vAxisBins,
                          vColor,
                          vOpacity);
        painter.restore();
        return;
    }

    painter.save();
    painter.setClipRect(QRect(QPoint(0, 0), viewportSize));
    const int globalUAxis = plane == WorkPlane::YZ ? 1 : 0;
    const int globalVAxis = plane == WorkPlane::XY ? 1 : 2;
    const std::array<QColor, 3> axisColors{
        gridAppearance_.axisXColor,
        gridAppearance_.axisYColor,
        gridAppearance_.axisZColor,
    };
    const auto projectGridPoint = [this, &viewportSize, plane, planeOffset](
                                      const QPointF &point) {
        QPointF screenPoint;
        if (!transform_.worldPointToScreenUnclipped(
                workPlanePointToWorld(point, plane, planeOffset),
                viewportSize,
                &screenPoint)) {
            return QPointF();
        }
        return screenPoint;
    };
    if (gridFrame.visibleAxes[static_cast<size_t>(globalUAxis)]) {
        QColor color = axisColors[static_cast<size_t>(globalUAxis)];
        color.setAlphaF(color.alphaF() * gridAppearance_.opacity);
        painter.setPen(QPen(color, 1.25));
        painter.drawLine(projectGridPoint(QPointF(minimumU, 0.0)),
                         projectGridPoint(QPointF(maximumU, 0.0)));
    }
    if (gridFrame.visibleAxes[static_cast<size_t>(globalVAxis)]) {
        QColor color = axisColors[static_cast<size_t>(globalVAxis)];
        color.setAlphaF(color.alphaF() * gridAppearance_.opacity);
        painter.setPen(QPen(color, 1.25));
        painter.drawLine(projectGridPoint(QPointF(0.0, minimumV)),
                         projectGridPoint(QPointF(0.0, maximumV)));
    }
    painter.restore();
}

QVector<QPointF> ViewportRenderer::rectangleVertices(const Shape &shape) const
{
    if (shape.geometryType == GeometryType::Rectangle &&
        isValidNurbsCurve(shape.nurbs) && shape.nurbs.controlPoints.size() == 5) {
        return shape.nurbs.controlPoints.mid(0, 4);
    }
    if (shape.geometryType != GeometryType::Rectangle || shape.points.size() < 2) {
        return {};
    }

    if (shape.points.size() >= 4) {
        return {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    return {first,
            QPointF(second.x(), first.y()),
            second,
            QPointF(first.x(), second.y())};
}

void ViewportRenderer::drawShape(QPainter &painter,
                                 const Shape &shape,
                                 const QSize &viewportSize,
                                 bool preview,
                                 bool selected,
                                 bool drawPreviewPoints,
                                 const QColor &layerColor,
                                 const QString &layerLineType,
                                 qreal layerLineWeightMm) const
{
    if (shape.points.isEmpty() && !isValidNurbsCurve(shape.nurbs) &&
        shape.components.isEmpty()) {
        return;
    }

    const QColor curveColor = selected
                                  ? QColor(QStringLiteral("#5da9e9"))
                                  : layerColor.isValid()
                                        ? layerColor
                                        : preview
                                              ? QColor(QStringLiteral("#e6b85c"))
                                              : QColor(QStringLiteral("#d28b45"));
    const QColor controlColor = QColor(QStringLiteral("#8aa7c7"));
    const qreal storedWidth = layerLineWeightMm > 0.0
                                  ? std::clamp(layerLineWeightMm * 6.0, 1.0, 10.0)
                                  : 2.0;
    const qreal curveWidth = selected ? 3.5 : (preview ? 1.5 : storedWidth);
    const QPen layerPen = selected || preview
                              ? QPen(curveColor, curveWidth)
                              : layerLineTypePen(curveColor, curveWidth, layerLineType);

    painter.setPen(layerPen);
    painter.setBrush(Qt::NoBrush);

    if (isDimensionGeometryType(shape.geometryType)) {
        const DimensionScreenLayout layout =
            buildDimensionScreenLayout(
                shape,
                transform_,
                viewportSize,
                architecturalDimensionFont_ ? DimensionFontStyle::Architectural
                                            : DimensionFontStyle::Standard);
        if (layout.valid) {
            painter.save();
            const qreal dimensionLineWidth =
                selected ? 2.0 : (preview ? 1.5 : 1.25);
            const QPen dimensionPen = selected || preview
                                          ? QPen(curveColor,
                                                 dimensionLineWidth,
                                                 Qt::SolidLine,
                                                 Qt::RoundCap,
                                                 Qt::RoundJoin)
                                          : layerLineTypePen(curveColor,
                                                             dimensionLineWidth,
                                                             layerLineType,
                                                             Qt::RoundCap);
            painter.setPen(dimensionPen);
            for (const QLineF &line : layout.lines) {
                painter.drawLine(line);
            }
            painter.setBrush(curveColor);
            for (const QPolygonF &arrowHead : layout.arrowHeads) {
                painter.drawPolygon(arrowHead);
            }

            painter.setFont(dimensionAnnotationFont(
                architecturalDimensionFont_ ? DimensionFontStyle::Architectural
                                            : DimensionFontStyle::Standard));
            painter.fillRect(layout.labelBounds, QColor(QStringLiteral("#282828")));
            painter.setPen(curveColor);
            painter.drawText(layout.labelBounds, Qt::AlignCenter, layout.label);
            painter.restore();
        }
        return;
    }

    if (shape.geometryType == GeometryType::Picture) {
        const QVector<QPointF> corners = pictureFrameCorners(shape);
        if (shape.pictureImage.isNull() || corners.size() != 4) {
            return;
        }

        QPolygonF sourceCorners;
        sourceCorners << QPointF(0.0, 0.0)
                      << QPointF(shape.pictureImage.width(), 0.0)
                      << QPointF(shape.pictureImage.width(), shape.pictureImage.height())
                      << QPointF(0.0, shape.pictureImage.height());
        QPolygonF screenCorners;
        for (const QPointF &corner : corners) {
            screenCorners.append(worldToScreen(corner, viewportSize));
        }

        QTransform imageTransform;
        if (QTransform::quadToQuad(sourceCorners, screenCorners, imageTransform)) {
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            if (preview) {
                painter.setOpacity(0.78);
            }
            painter.setTransform(imageTransform, true);
            painter.drawImage(QPointF(0.0, 0.0), shape.pictureImage);
            painter.restore();
        }

        if (selected || preview) {
            QPolygonF outline = screenCorners;
            outline.append(screenCorners.first());
            painter.save();
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(curveColor,
                                selected ? 1.5 : 1.25,
                                preview ? Qt::DashLine : Qt::SolidLine));
            painter.drawPolyline(outline);
            painter.restore();
        }
        return;
    }

    if (shape.geometryType == GeometryType::PolyCurve && !shape.components.isEmpty()) {
        for (int index = 0; index < shape.components.size(); ++index) {
            const Shape::NurbsCurve2D &component = shape.components[index];
            if (isValidNurbsCurve(component)) {
                drawNurbsCurve(painter,
                               component,
                               shapeComponentWorkPlaneFrame(shape, index),
                               viewportSize);
            }
        }
    } else if (shape.geometryType == GeometryType::Point && !shape.points.isEmpty()) {
        const qreal pointWidth = selected ? 2.0 : curveWidth;
        painter.setPen(selected || preview
                           ? QPen(curveColor, pointWidth)
                           : layerLineTypePen(curveColor, pointWidth, layerLineType));
        painter.setBrush(curveColor);
        painter.drawEllipse(worldToScreen(shape.points.first(), viewportSize),
                            selected ? 5.0 : 4.5,
                            selected ? 5.0 : 4.5);
    } else if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else {
            for (int index = 0; index + 1 < shape.points.size(); ++index) {
                painter.drawLine(worldToScreen(shape.points[index], viewportSize),
                                 worldToScreen(shape.points[index + 1], viewportSize));
            }
        }
    } else if (shape.geometryType == GeometryType::Rectangle && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
            return;
        }
        const QVector<QPointF> vertices = rectangleVertices(shape);
        if (vertices.size() >= 4) {
            QPainterPath rectanglePath;
            rectanglePath.moveTo(worldToScreen(vertices.first(), viewportSize));
            for (int index = 1; index < vertices.size(); ++index) {
                rectanglePath.lineTo(worldToScreen(vertices[index], viewportSize));
            }
            rectanglePath.closeSubpath();
            painter.drawPath(rectanglePath);
        }
    } else if (shape.geometryType == GeometryType::Polygon) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        }
    } else if (shape.geometryType == GeometryType::Circle && shape.points.size() >= 2) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else {
            const QPointF center = worldToScreen(shape.points[0], viewportSize);
            const QPointF edge = worldToScreen(shape.points[1], viewportSize);
            const qreal radius =
                std::hypot(edge.x() - center.x(), edge.y() - center.y());
            painter.drawEllipse(center, radius, radius);
        }
    } else if (shape.geometryType == GeometryType::Ellipse) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        }
    } else if (shape.geometryType == GeometryType::Arc && shape.points.size() >= 3) {
        if (isValidNurbsCurve(shape.nurbs)) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else if (shape.arcMode == ArcMode::OnePoint) {
            if (std::abs(shape.arcSweep) > 1e-9) {
                drawCenterArcWithSweep(painter,
                                       shape.points[0],
                                       shape.points[1],
                                       shape.arcSweep,
                                       viewportSize);
            } else {
                drawCenterArc(painter,
                              shape.points[0],
                              shape.points[1],
                              shape.points[2],
                              viewportSize);
            }
        } else {
            drawCircularArc(painter,
                            shape.points[0],
                            shape.points[1],
                            shape.points[2],
                            viewportSize);
        }
    } else if (shape.geometryType == GeometryType::Bezier ||
               shape.geometryType == GeometryType::Nurbs) {
        const QVector<QPointF> controlPoints = shape.nurbs.controlPoints.isEmpty()
                                                   ? shape.points
                                                   : shape.nurbs.controlPoints;
        const bool hasStoredCurve = isValidNurbsCurve(shape.nurbs);
        if (controlPoints.size() >= 2) {
            painter.setPen(QPen(controlColor, 1, Qt::DashLine));
            for (int index = 0; index + 1 < controlPoints.size(); ++index) {
                painter.drawLine(worldToScreen(controlPoints[index], viewportSize),
                                 worldToScreen(controlPoints[index + 1], viewportSize));
            }
        }

        painter.setPen(layerPen);
        if (hasStoredCurve) {
            drawNurbsCurve(painter, shape.nurbs, viewportSize);
        } else if (shape.geometryType == GeometryType::Bezier && shape.points.size() >= 4) {
            QPainterPath curve;
            curve.moveTo(worldToScreen(shape.points[0], viewportSize));
            curve.cubicTo(worldToScreen(shape.points[1], viewportSize),
                          worldToScreen(shape.points[2], viewportSize),
                          worldToScreen(shape.points[3], viewportSize));
            painter.drawPath(curve);
        } else if (shape.geometryType == GeometryType::Nurbs) {
            for (int index = 0; index + 1 < controlPoints.size(); ++index) {
                painter.drawLine(worldToScreen(controlPoints[index], viewportSize),
                                 worldToScreen(controlPoints[index + 1], viewportSize));
            }
        }
    } else {
        painter.setPen(QPen(controlColor, 1, Qt::DashLine));
        for (int index = 0; index + 1 < shape.points.size(); ++index) {
            painter.drawLine(worldToScreen(shape.points[index], viewportSize),
                             worldToScreen(shape.points[index + 1], viewportSize));
        }
    }

    if (preview && drawPreviewPoints && !isDimensionGeometryType(shape.geometryType)) {
        painter.setBrush(controlColor);
        painter.setPen(Qt::NoPen);
        for (const QPointF &point : shape.points) {
            painter.drawEllipse(worldToScreen(point, viewportSize), 4.0, 4.0);
        }
    }
}

bool ViewportRenderer::subdivisionCurve(const Shape &shape,
                                        Shape::NurbsCurve2D *curve) const
{
    if (curve == nullptr) {
        return false;
    }

    if (isValidNurbsCurve(shape.nurbs)) {
        *curve = shape.nurbs;
        return true;
    }

    if (shape.geometryType == GeometryType::Line && shape.points.size() >= 2) {
        *curve = makeDegreeOneNurbs(shape.points);
        return isValidNurbsCurve(*curve);
    }

    return false;
}

void ViewportRenderer::drawControlPoints(QPainter &painter,
                                         const Shape &shape,
                                         const QSize &viewportSize,
                                         ObjectId shapeObjectId,
                                         ObjectId selectedObjectId,
                                         bool draggingControlPoint,
                                         int activeControlPointIndex,
                                         bool drawMarkers) const
{
    const QColor handleColor(QStringLiteral("#77b7e6"));
    const QColor handleFill(QStringLiteral("#263b4b"));
    painter.setPen(QPen(handleColor, 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);

    int globalControlPointIndex = 0;
    const auto drawControlPointChain = [&](const QVector<QPointF> &controlPoints,
                                           const WorkPlaneFrame &frame) {
        if (controlPoints.isEmpty()) {
            return;
        }

        for (int index = 0; index + 1 < controlPoints.size(); ++index) {
            painter.drawLine(transform_.workPlaneToScreen(controlPoints[index],
                                                          viewportSize,
                                                          frame),
                             transform_.workPlaneToScreen(controlPoints[index + 1],
                                                          viewportSize,
                                                          frame));
        }

        for (int index = 0; index < controlPoints.size(); ++index) {
            if (!drawMarkers) {
                continue;
            }
            const bool active = draggingControlPoint &&
                                shapeObjectId == selectedObjectId &&
                                activeControlPointIndex == globalControlPointIndex + index;
            painter.setPen(QPen(active ? QColor(QStringLiteral("#f0a45a")) : handleColor,
                                1.5));
            painter.setBrush(active ? QColor(QStringLiteral("#f0a45a")) : handleFill);
            const QPointF screenPoint = transform_.workPlaneToScreen(
                controlPoints[index], viewportSize, frame);
            painter.drawRect(QRectF(screenPoint - QPointF(4.0, 4.0),
                                    screenPoint + QPointF(4.0, 4.0)));
        }
        globalControlPointIndex += controlPoints.size();
    };

    if (shape.geometryType == GeometryType::PolyCurve) {
        for (int index = 0; index < shape.components.size(); ++index) {
            drawControlPointChain(shape.components[index].controlPoints,
                                  shapeComponentWorkPlaneFrame(shape, index));
        }
        return;
    }

    drawControlPointChain(curveHitTester_.controlPointsForShape(shape),
                          shapeWorkPlaneFrame(shape));
}

void ViewportRenderer::drawSubdivisionPoints(QPainter &painter,
                                             const Shape &shape,
                                             const QVector<double> &parameters,
                                             const QSize &viewportSize,
                                             bool preview) const
{
    if (parameters.isEmpty()) {
        return;
    }

    Shape::NurbsCurve2D curve;
    if (!subdivisionCurve(shape, &curve)) {
        return;
    }

    const QColor pointColor = preview ? QColor(QStringLiteral("#f0a45a"))
                                      : QColor(QStringLiteral("#e6b85c"));
    painter.setPen(QPen(pointColor, preview ? 2.0 : 1.5));
    painter.setBrush(pointColor);

    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    if (fullKnots.size() > curve.controlPoints.size()) {
        const double firstParameter = fullKnots[curve.degree];
        const double lastParameter = fullKnots[curve.controlPoints.size()];
        for (const double parameter : {firstParameter, lastParameter}) {
            QPointF point;
            if (evaluateNurbsPoint(curve, parameter, &point)) {
                painter.drawEllipse(worldToScreen(point, viewportSize),
                                    preview ? 5.0 : 4.0,
                                    preview ? 5.0 : 4.0);
            }
        }
    }

    for (const double parameter : parameters) {
        QPointF point;
        if (!evaluateNurbsPoint(curve, parameter, &point)) {
            continue;
        }
        painter.drawEllipse(worldToScreen(point, viewportSize),
                            preview ? 5.0 : 4.0,
                            preview ? 5.0 : 4.0);
    }
}

void ViewportRenderer::drawNurbsCurve(QPainter &painter,
                                      const Shape::NurbsCurve2D &curve,
                                      const QSize &viewportSize) const
{
    drawNurbsCurve(painter, curve, transform_.workPlaneFrame(), viewportSize);
}

void ViewportRenderer::drawNurbsCurve(QPainter &painter,
                                      const Shape::NurbsCurve2D &curve,
                                      const WorkPlaneFrame &frame,
                                      const QSize &viewportSize) const
{
    if (!isValidNurbsCurve(curve) || !isValidWorkPlaneFrame(frame)) {
        return;
    }

    const auto project = [&](const QPointF &point) {
        return transform_.workPlaneToScreen(point, viewportSize, frame);
    };

    const int controlPointCount = curve.controlPoints.size();
    const QVector<double> fullKnots = expandedNurbsKnotVector(curve);
    const qreal firstParameter = fullKnots[curve.degree];
    const qreal lastParameter = fullKnots[controlPointCount];
    int nonZeroSpans = 0;
    for (int index = curve.degree; index < controlPointCount; ++index) {
        if (fullKnots[index + 1] > fullKnots[index]) {
            ++nonZeroSpans;
        }
    }
    QPainterPath path;
    if (!smoothCurveDisplay_) {
        const int sampleCount = std::max(32, nonZeroSpans * 24);
        bool hasStart = false;
        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            QPointF point;
            if (!evaluateNurbsPoint(curve, parameter, &point)) {
                continue;
            }
            const QPointF screenPoint = project(point);
            if (!hasStart) {
                path.moveTo(screenPoint);
                hasStart = true;
            } else {
                path.lineTo(screenPoint);
            }
        }

        if (hasStart) {
            painter.drawPath(path);
        }
        return;
    }

    constexpr qreal flatnessTolerancePixels = 0.3;
    constexpr int maximumSubdivisionDepth = 14;
    constexpr int maximumAdaptivePointCount = 16384;
    QVector<QPointF> screenPoints;
    screenPoints.reserve(std::min(maximumAdaptivePointCount, nonZeroSpans * 8));
    bool exceededAdaptivePointLimit = false;

    const auto screenPointAt = [&](qreal parameter, QPointF *screenPoint) {
        if (screenPoint == nullptr) {
            return false;
        }
        QPointF worldPoint;
        if (!evaluateNurbsPoint(curve, parameter, &worldPoint)) {
            return false;
        }
        *screenPoint = project(worldPoint);
        return true;
    };
    const auto distanceToSegment = [](const QPointF &point,
                                     const QPointF &segmentStart,
                                     const QPointF &segmentEnd) {
        const QPointF direction = segmentEnd - segmentStart;
        const qreal lengthSquared = QPointF::dotProduct(direction, direction);
        if (lengthSquared <= 1.0e-16) {
            return std::hypot(point.x() - segmentStart.x(),
                              point.y() - segmentStart.y());
        }
        const qreal projection = std::clamp(
            QPointF::dotProduct(point - segmentStart, direction) / lengthSquared,
            0.0,
            1.0);
        const QPointF nearest = segmentStart + direction * projection;
        return std::hypot(point.x() - nearest.x(), point.y() - nearest.y());
    };

    const auto appendAdaptiveSegment = [&](const auto &self,
                                           qreal parameterStart,
                                           const QPointF &screenStart,
                                           qreal parameterEnd,
                                           const QPointF &screenEnd,
                                           int depth) -> void {
        if (exceededAdaptivePointLimit) {
            return;
        }

        const qreal parameterRange = parameterEnd - parameterStart;
        const qreal quarterParameter = parameterStart + parameterRange * 0.25;
        const qreal middleParameter = parameterStart + parameterRange * 0.5;
        const qreal threeQuarterParameter = parameterStart + parameterRange * 0.75;
        QPointF quarterPoint;
        QPointF middlePoint;
        QPointF threeQuarterPoint;
        if (!screenPointAt(quarterParameter, &quarterPoint) ||
            !screenPointAt(middleParameter, &middlePoint) ||
            !screenPointAt(threeQuarterParameter, &threeQuarterPoint)) {
            screenPoints.append(screenEnd);
            if (screenPoints.size() > maximumAdaptivePointCount) {
                exceededAdaptivePointLimit = true;
            }
            return;
        }

        const qreal maximumDeviation = std::max({
            distanceToSegment(quarterPoint, screenStart, screenEnd),
            distanceToSegment(middlePoint, screenStart, screenEnd),
            distanceToSegment(threeQuarterPoint, screenStart, screenEnd)});
        if (maximumDeviation <= flatnessTolerancePixels ||
            depth >= maximumSubdivisionDepth) {
            screenPoints.append(screenEnd);
            if (screenPoints.size() > maximumAdaptivePointCount) {
                exceededAdaptivePointLimit = true;
            }
            return;
        }

        self(self,
             parameterStart,
             screenStart,
             middleParameter,
             middlePoint,
             depth + 1);
        self(self,
             middleParameter,
             middlePoint,
             parameterEnd,
             screenEnd,
             depth + 1);
    };

    bool hasStart = false;
    for (int spanIndex = curve.degree;
         spanIndex < controlPointCount && !exceededAdaptivePointLimit;
         ++spanIndex) {
        const qreal spanStart = fullKnots[spanIndex];
        const qreal spanEnd = fullKnots[spanIndex + 1];
        if (spanEnd <= spanStart) {
            continue;
        }

        QPointF spanStartPoint;
        QPointF spanEndPoint;
        if (!screenPointAt(spanStart, &spanStartPoint) ||
            !screenPointAt(spanEnd, &spanEndPoint)) {
            continue;
        }
        if (!hasStart) {
            screenPoints.append(spanStartPoint);
            hasStart = true;
        }
        appendAdaptiveSegment(appendAdaptiveSegment,
                              spanStart,
                              spanStartPoint,
                              spanEnd,
                              spanEndPoint,
                              0);
    }

    if (exceededAdaptivePointLimit) {
        // Preserve the entire curve if a pathological shape exceeds the
        // adaptive detail budget. This fallback is bounded and still becomes
        // denser at higher zoom levels.
        screenPoints.clear();
        const int sampleCount = std::max(
            32,
            std::min(
                maximumAdaptivePointCount,
                static_cast<int>(std::ceil(
                    nonZeroSpans * 24.0 *
                    std::sqrt(std::max(
                        1.0,
                        transform_.viewScalePixelsPerWorldUnit(viewportSize)))))));
        for (int sample = 0; sample <= sampleCount; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / sampleCount;
            const qreal parameter = firstParameter +
                                    (lastParameter - firstParameter) * fraction;
            QPointF screenPoint;
            if (screenPointAt(parameter, &screenPoint)) {
                screenPoints.append(screenPoint);
            }
        }
    }

    for (int index = 0; index < screenPoints.size(); ++index) {
        if (index == 0) {
            path.moveTo(screenPoints[index]);
        } else {
            path.lineTo(screenPoints[index]);
        }
    }

    if (!screenPoints.isEmpty()) {
        painter.drawPath(path);
    }
}

void ViewportRenderer::drawCircularArc(QPainter &painter,
                                       const QPointF &start,
                                       const QPointF &end,
                                       const QPointF &through,
                                       const QSize &viewportSize) const
{
    CircularArc2D arc;
    if (!makeCircularArcThroughPoint(start, end, through, &arc)) {
        painter.drawLine(worldToScreen(start, viewportSize),
                         worldToScreen(end, viewportSize));
        return;
    }
    drawNurbsCurve(painter, arc.curve, viewportSize);
}

void ViewportRenderer::drawCenterArcWithSweep(QPainter &painter,
                                              const QPointF &centerWorld,
                                              const QPointF &startWorld,
                                              qreal sweepAngle,
                                              const QSize &viewportSize) const
{
    const QPointF startVector = startWorld - centerWorld;
    const qreal radius = std::hypot(startVector.x(), startVector.y());
    if (radius <= 1e-9 || std::abs(sweepAngle) <= 1e-9) {
        return;
    }

    CircularArc2D arc;
    if (makeCircularArcFromCenterSweep(
            centerWorld,
            radius,
            std::atan2(startVector.y(), startVector.x()),
            sweepAngle,
            &arc)) {
        drawNurbsCurve(painter, arc.curve, viewportSize);
    }
}

void ViewportRenderer::drawCenterArc(QPainter &painter,
                                     const QPointF &centerWorld,
                                     const QPointF &startWorld,
                                     const QPointF &endWorld,
                                     const QSize &viewportSize) const
{
    const QPointF startVector = startWorld - centerWorld;
    const QPointF endVector = endWorld - centerWorld;
    const qreal radius = std::hypot(startVector.x(), startVector.y());
    if (radius <= 1e-9) {
        painter.drawLine(worldToScreen(startWorld, viewportSize),
                         worldToScreen(endWorld, viewportSize));
        return;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal twoPi = 2.0 * pi;
    const qreal startAngle = std::atan2(startVector.y(), startVector.x());
    const qreal endAngle = std::atan2(endVector.y(), endVector.x());
    qreal sweepAngle = endAngle - startAngle;
    if (sweepAngle > pi) {
        sweepAngle -= twoPi;
    } else if (sweepAngle < -pi) {
        sweepAngle += twoPi;
    }

    if (std::abs(sweepAngle) <= 1e-9) {
        painter.drawLine(worldToScreen(startWorld, viewportSize),
                         worldToScreen(endWorld, viewportSize));
        return;
    }
    drawCenterArcWithSweep(painter,
                           centerWorld,
                           startWorld,
                           sweepAngle,
                           viewportSize);
}

} // namespace classiCAD
