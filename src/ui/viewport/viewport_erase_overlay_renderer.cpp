#include "viewport_erase_overlay_renderer.h"

#include <QFont>
#include <QPainterPath>

#include <algorithm>

namespace classiCAD {

void ViewportEraseOverlayRenderer::drawSampledIntervals(
    QPainter &painter,
    const SampledNurbsCurve2D &sampled,
    const QVector<ParameterInterval> &intervals) const
{
    for (const ParameterInterval &interval : intervals) {
        QPainterPath path;
        bool hasStart = false;
        for (int sample = 1; sample < sampled.screenPoints.size(); ++sample) {
            const qreal segmentStart = sampled.parameters[sample - 1];
            const qreal segmentEnd = sampled.parameters[sample];
            const qreal overlapStart = std::max(interval.start, segmentStart);
            const qreal overlapEnd = std::min(interval.end, segmentEnd);
            if (overlapEnd <= overlapStart || segmentEnd <= segmentStart) {
                continue;
            }

            const qreal startFraction =
                (overlapStart - segmentStart) / (segmentEnd - segmentStart);
            const qreal endFraction =
                (overlapEnd - segmentStart) / (segmentEnd - segmentStart);
            const QPointF startPoint =
                sampled.screenPoints[sample - 1] +
                (sampled.screenPoints[sample] - sampled.screenPoints[sample - 1]) *
                    startFraction;
            const QPointF endPoint =
                sampled.screenPoints[sample - 1] +
                (sampled.screenPoints[sample] - sampled.screenPoints[sample - 1]) *
                    endFraction;
            if (!hasStart) {
                path.moveTo(startPoint);
                hasStart = true;
            } else {
                path.lineTo(startPoint);
            }
            path.lineTo(endPoint);
        }

        if (hasStart) {
            painter.drawPath(path);
        }
    }
}

void ViewportEraseOverlayRenderer::drawCandidatePreview(
    QPainter &painter,
    const QVector<EraseCurveSampleCache> &targetCurves,
    int shapeIndex,
    bool eraseStrokeHasPoints) const
{
    if (!eraseStrokeHasPoints) {
        return;
    }

    painter.setPen(QPen(QColor(QStringLiteral("#d28b45")), 3.5));
    painter.setBrush(Qt::NoBrush);
    for (const EraseCurveSampleCache &targetCurve : targetCurves) {
        if (targetCurve.shapeIndex == shapeIndex) {
            drawSampledIntervals(painter,
                                 targetCurve.sampled,
                                 targetCurve.previewIntervals);
        }
    }
}

void ViewportEraseOverlayRenderer::drawCursorPreview(
    QPainter &painter,
    ToolId activeTool,
    const QPointF &eraseCursorScreen,
    bool cursorValid,
    bool eraseCursorPressed,
    int candidateCount) const
{
    if (!cursorValid || !eraseCursorPressed) {
        return;
    }

    constexpr qreal eraserRadiusPixels = 10.0;
    const QColor eraserColor(QStringLiteral("#f0a45a"));
    painter.setPen(QPen(eraserColor, 1.5, Qt::DashLine));
    painter.setBrush(QColor(240, 164, 90, 28));
    painter.drawEllipse(eraseCursorScreen, eraserRadiusPixels, eraserRadiusPixels);

    if (isEraseLikeTool(activeTool) && candidateCount > 0) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        painter.drawText(eraseCursorScreen + QPointF(14.0, -10.0),
                         QStringLiteral("%1 %2")
                             .arg(toolName(activeTool))
                             .arg(candidateCount));
    }
}

} // namespace classiCAD
