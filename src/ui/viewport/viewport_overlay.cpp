#include "viewport_overlay.h"

#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace classiCAD {

ViewportOverlay::ViewportOverlay(const ViewportRenderer &renderer,
                                 const ViewportTransform &transform)
    : renderer_(renderer)
    , transform_(transform)
{
}

void ViewportOverlay::drawSnapMarker(QPainter &painter,
                                     SnapType type,
                                     const QPointF &worldPoint,
                                     const QSize &viewportSize) const
{
    const QPointF snapScreen = transform_.worldToScreen(worldPoint, viewportSize);
    const QColor snapColor(QStringLiteral("#63b5e8"));
    painter.setPen(QPen(snapColor, 2.0));
    painter.setBrush(Qt::NoBrush);

    if (type == SnapType::Endpoint) {
        painter.drawEllipse(snapScreen, 7.0, 7.0);
    } else if (type == SnapType::Midpoint) {
        painter.drawRect(QRectF(snapScreen - QPointF(6.0, 6.0),
                                snapScreen + QPointF(6.0, 6.0)));
    } else if (type == SnapType::Intersection) {
        painter.drawLine(snapScreen - QPointF(7.0, 7.0),
                         snapScreen + QPointF(7.0, 7.0));
        painter.drawLine(snapScreen - QPointF(7.0, -7.0),
                         snapScreen + QPointF(7.0, -7.0));
    } else if (type == SnapType::Center) {
        painter.drawEllipse(snapScreen, 7.0, 7.0);
        painter.drawLine(snapScreen - QPointF(9.0, 0.0),
                         snapScreen + QPointF(9.0, 0.0));
        painter.drawLine(snapScreen - QPointF(0.0, 9.0),
                         snapScreen + QPointF(0.0, 9.0));
    } else if (type == SnapType::Perpendicular) {
        const QPointF corner = snapScreen + QPointF(-2.0, 3.0);
        painter.drawLine(snapScreen + QPointF(-8.0, 3.0), corner);
        painter.drawLine(corner, snapScreen + QPointF(-2.0, -5.0));
    } else if (type == SnapType::Tangent) {
        painter.drawEllipse(snapScreen, 6.0, 6.0);
        painter.drawLine(snapScreen + QPointF(-7.0, 4.0),
                         snapScreen + QPointF(7.0, -4.0));
    }
}

void ViewportOverlay::drawSelectionBox(QPainter &painter,
                                       const QPointF &startScreen,
                                       const QPointF &currentScreen) const
{
    const QRectF selectionBox(startScreen, currentScreen);
    painter.setPen(QPen(QColor(QStringLiteral("#63b5e8")), 1.0, Qt::DashLine));
    painter.setBrush(QColor(99, 181, 232, 35));
    painter.drawRect(selectionBox.normalized());
}

void ViewportOverlay::drawControlPoints(QPainter &painter,
                                        const Shape &shape,
                                        const QSize &viewportSize,
                                        ObjectId shapeObjectId,
                                        ObjectId selectedObjectId,
                                        bool draggingControlPoint,
                                        int activeControlPointIndex) const
{
    renderer_.drawControlPoints(painter,
                                shape,
                                viewportSize,
                                shapeObjectId,
                                selectedObjectId,
                                draggingControlPoint,
                                activeControlPointIndex);
}

void ViewportOverlay::drawSubdivisionPoints(QPainter &painter,
                                            const Shape &shape,
                                            const QVector<double> &parameters,
                                            const QSize &viewportSize,
                                            bool preview) const
{
    renderer_.drawSubdivisionPoints(painter,
                                    shape,
                                    parameters,
                                    viewportSize,
                                    preview);
}

void ViewportOverlay::drawLinePreview(QPainter &painter,
                                      const QVector<QPointF> &pendingPoints,
                                      const QPointF &cursorWorld,
                                      bool cursorValid,
                                      const SnapResult &currentSnap,
                                      const QSize &viewportSize) const
{
    const QColor lineColor(QStringLiteral("#e6b85c"));
    const QColor pointColor(QStringLiteral("#f0a45a"));

    painter.setPen(QPen(lineColor, 2.0));
    for (int index = 0; index + 1 < pendingPoints.size(); ++index) {
        painter.drawLine(transform_.worldToScreen(pendingPoints[index], viewportSize),
                         transform_.worldToScreen(pendingPoints[index + 1], viewportSize));
    }

    if (!pendingPoints.isEmpty() && cursorValid) {
        painter.setPen(QPen(lineColor, 2.0));
        painter.drawLine(transform_.worldToScreen(pendingPoints.back(), viewportSize),
                         transform_.worldToScreen(cursorWorld, viewportSize));
    }

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    for (const QPointF &point : pendingPoints) {
        painter.drawEllipse(transform_.worldToScreen(point, viewportSize), 5.0, 5.0);
    }

    if (cursorValid) {
        painter.setPen(QPen(pointColor, 2.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(transform_.worldToScreen(cursorWorld, viewportSize), 4.0, 4.0);
    }

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawArcPreview(QPainter &painter,
                                     const QVector<QPointF> &pendingPoints,
                                     ArcMode arcMode,
                                     const QPointF &cursorWorld,
                                     bool cursorValid,
                                     qreal arcSweep,
                                     const SnapResult &currentSnap,
                                     const QSize &viewportSize) const
{
    if (pendingPoints.isEmpty()) {
        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
        }
        return;
    }

    const QColor arcColor(QStringLiteral("#e6b85c"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    painter.setPen(QPen(arcColor, 2.0));
    painter.setBrush(Qt::NoBrush);

    if (arcMode == ArcMode::OnePoint) {
        if (pendingPoints.size() == 1) {
            if (cursorValid) {
                painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                                 transform_.worldToScreen(cursorWorld, viewportSize));
            }
        } else if (cursorValid) {
            painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")), 1.0, Qt::DashLine));
            painter.drawLine(transform_.worldToScreen(pendingPoints[0], viewportSize),
                             transform_.worldToScreen(pendingPoints[1], viewportSize));

            painter.setPen(QPen(arcColor, 2.0));
            if (std::abs(arcSweep) > 1e-12) {
                renderer_.drawCenterArcWithSweep(painter,
                                                 pendingPoints[0],
                                                 pendingPoints[1],
                                                 arcSweep,
                                                 viewportSize);
            }
        }
    } else {
        if (pendingPoints.size() == 1) {
            if (cursorValid) {
                painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                                 transform_.worldToScreen(cursorWorld, viewportSize));
            }
        } else if (cursorValid) {
            painter.setPen(QPen(QColor(QStringLiteral("#8aa7c7")), 1.0, Qt::DashLine));
            painter.drawLine(transform_.worldToScreen(pendingPoints[0], viewportSize),
                             transform_.worldToScreen(pendingPoints[1], viewportSize));
            painter.setPen(QPen(arcColor, 2.0));
            renderer_.drawCircularArc(painter,
                                      pendingPoints[0],
                                      pendingPoints[1],
                                      cursorWorld,
                                      viewportSize);
        }
    }

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    for (const QPointF &point : pendingPoints) {
        painter.drawEllipse(transform_.worldToScreen(point, viewportSize), 5.0, 5.0);
    }

    if (cursorValid) {
        painter.setPen(QPen(pointColor, 2.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(transform_.worldToScreen(cursorWorld, viewportSize), 4.0, 4.0);
    }

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawCirclePreview(QPainter &painter,
                                        const QVector<QPointF> &pendingPoints,
                                        const QPointF &cursorWorld,
                                        bool cursorValid,
                                        const SnapResult &currentSnap,
                                        const QSize &viewportSize) const
{
    if (pendingPoints.isEmpty()) {
        return;
    }

    const QColor circleColor(QStringLiteral("#e6b85c"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    const QPointF center = transform_.worldToScreen(pendingPoints.first(), viewportSize);

    painter.setPen(QPen(circleColor, 2.0));
    painter.setBrush(Qt::NoBrush);
    if (cursorValid) {
        const QPointF edge = transform_.worldToScreen(cursorWorld, viewportSize);
        const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
        painter.drawEllipse(center, radius, radius);
        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(pointColor);
        painter.drawEllipse(edge, 4.0, 4.0);
    }

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    painter.drawEllipse(center, 5.0, 5.0);

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawRectanglePreview(QPainter &painter,
                                           const QVector<QPointF> &pendingPoints,
                                           const QPointF &cursorWorld,
                                           bool cursorValid,
                                           const SnapResult &currentSnap,
                                           const QSize &viewportSize) const
{
    if (pendingPoints.isEmpty()) {
        return;
    }

    const QColor rectangleColor(QStringLiteral("#e6b85c"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    const QPointF firstWorld = pendingPoints.first();
    const QPointF firstScreen = transform_.worldToScreen(firstWorld, viewportSize);

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    painter.drawEllipse(firstScreen, 5.0, 5.0);

    if (!cursorValid) {
        return;
    }

    const QPointF secondWorld = cursorWorld;
    const QPointF secondScreen = transform_.worldToScreen(secondWorld, viewportSize);
    const QRectF rectangle = QRectF(firstScreen, secondScreen).normalized();
    painter.setPen(QPen(rectangleColor, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rectangle);

    painter.setPen(QPen(pointColor, 2.0));
    painter.setBrush(pointColor);
    painter.drawEllipse(secondScreen, 4.0, 4.0);

    painter.setPen(QColor(QStringLiteral("#d0d0d0")));
    painter.setFont(QFont(QStringLiteral("Sans"), 9));
    const qreal width = std::abs(secondWorld.x() - firstWorld.x());
    const qreal height = std::abs(secondWorld.y() - firstWorld.y());
    const QString dimensions = QStringLiteral("W %1  H %2")
                                   .arg(width, 0, 'f', 2)
                                   .arg(height, 0, 'f', 2);
    QPointF labelPosition = rectangle.topLeft() + QPointF(6.0, -8.0);
    if (labelPosition.y() < 14.0) {
        labelPosition.setY(rectangle.top() + 16.0);
    }
    painter.drawText(labelPosition, dimensions);

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawPointPreview(QPainter &painter,
                                       const QPointF &cursorWorld,
                                       bool cursorValid,
                                       const SnapResult &currentSnap,
                                       const QSize &viewportSize) const
{
    if (!cursorValid) {
        return;
    }

    const QPointF screenPoint = transform_.worldToScreen(cursorWorld, viewportSize);
    const QColor pointColor(QStringLiteral("#e6b85c"));
    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(pointColor);
    painter.drawEllipse(screenPoint, 4.5, 4.5);
    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawRotatePreview(QPainter &painter,
                                        const QPointF &cursorWorld,
                                        bool cursorValid,
                                        int rotateStep,
                                        const QPointF &rotateBaseWorld,
                                        const QPointF &rotateReferenceWorld,
                                        qreal rotatePreviewAngle,
                                        const SnapResult &currentSnap,
                                        const QSize &viewportSize) const
{
    if (!cursorValid) {
        return;
    }

    const QColor rotateColor(QStringLiteral("#e6b85c"));
    const QColor guideColor(QStringLiteral("#8aa7c7"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    const QPointF cursorScreen = transform_.worldToScreen(cursorWorld, viewportSize);

    painter.setBrush(Qt::NoBrush);
    if (rotateStep == 0) {
        painter.setPen(QPen(pointColor, 1.5));
        painter.drawEllipse(cursorScreen, 6.0, 6.0);
    } else {
        const QPointF baseScreen = transform_.worldToScreen(rotateBaseWorld, viewportSize);
        painter.setPen(QPen(pointColor, 1.5));
        painter.drawEllipse(baseScreen, 6.0, 6.0);
        if (rotateStep >= 1) {
            painter.setPen(QPen(guideColor, 1.0, Qt::DashLine));
            painter.drawLine(baseScreen, cursorScreen);
        }
        if (rotateStep >= 2) {
            const QPointF referenceScreen =
                transform_.worldToScreen(rotateReferenceWorld, viewportSize);
            painter.setPen(QPen(rotateColor, 1.0, Qt::DashLine));
            painter.drawLine(baseScreen, referenceScreen);
            painter.setPen(QColor(QStringLiteral("#d0d0d0")));
            painter.setFont(QFont(QStringLiteral("Sans"), 9));
            const QString angleText = QStringLiteral("%1°")
                                           .arg(rotatePreviewAngle *
                                                    180.0 /
                                                    3.14159265358979323846,
                                                0,
                                                'f',
                                                1);
            painter.drawText(cursorScreen + QPointF(12.0, -10.0), angleText);
        }
    }

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(pointColor);
    painter.drawEllipse(cursorScreen, 4.0, 4.0);
    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawSampledEraseIntervals(
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

void ViewportOverlay::drawEraseCandidatePreview(
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
        if (targetCurve.shapeIndex != shapeIndex) {
            continue;
        }
        drawSampledEraseIntervals(painter,
                                  targetCurve.sampled,
                                  targetCurve.previewIntervals);
    }
}

void ViewportOverlay::drawErasePreview(QPainter &painter,
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

void ViewportOverlay::drawToolStatus(QPainter &painter,
                                     const QSize &viewportSize,
                                     ToolId activeTool,
                                     ArcMode arcMode,
                                     bool subdivisionActive,
                                     int subdivisionSections,
                                     bool joinActive,
                                     int joinCount,
                                     bool lineCommandActive,
                                     int rotateStep) const
{
    painter.setPen(QColor(QStringLiteral("#a0a0a0")));
    painter.setFont(QFont(QStringLiteral("Sans"), 10));
    const QString activeToolLabel = activeTool == Tool::Arc
                                        ? arcModeName(arcMode)
                                        : toolName(activeTool);
    painter.drawText(18,
                     28,
                     QStringLiteral("2D VIEWPORT  •  %1").arg(activeToolLabel));

    if (subdivisionActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        painter.drawText(18,
                         viewportSize.height() - 42,
                         QStringLiteral("SUBDIVIDE  •  %1 sections  •  endpoints included")
                             .arg(subdivisionSections));
    }

    if (joinActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        painter.drawText(18,
                         viewportSize.height() - 42,
                         QStringLiteral("JOIN  •  %1 curves selected  •  Enter to join  •  Esc to cancel")
                             .arg(joinCount));
    }

    if (activeTool == Tool::Erase) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Drag over a curve segment  •  Release to erase to intersection/end  •  Esc/RMB exits"));
    } else if (activeTool == Tool::Trim) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click a selected curve segment to trim to intersection/end  •  Esc/RMB exits"));
    } else if (activeTool == Tool::Rotate) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        const QString rotateHint = rotateStep == 0
                                       ? QStringLiteral("Click rotation center  •  Esc/RMB cancels")
                                       : rotateStep == 1
                                             ? QStringLiteral("Click starting direction  •  Esc/RMB cancels")
                                             : QStringLiteral("Click ending direction to rotate  •  Esc/RMB cancels");
        painter.drawText(18, viewportSize.height() - 18, rotateHint);
    } else if (activeTool == Tool::Mirror) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click the first and second points of the mirror axis  •  Esc/RMB cancels"));
    } else if (activeTool == Tool::Line && lineCommandActive) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click to place connected points  •  Right-click to finish"));
    } else if (activeTool != Tool::Select) {
        QString hint;
        if (activeTool == Tool::Arc) {
            const QString points = arcMode == ArcMode::OnePoint
                                        ? QStringLiteral("center, start, endpoint")
                                        : QStringLiteral("start, end, through point");
            hint = QStringLiteral("Click to place %1  •  Esc clears current tool input")
                       .arg(points);
        } else {
            hint = QStringLiteral("Click to place %1 point%2  •  Esc clears current tool input")
                       .arg(toolName(activeTool).toLower())
                       .arg(requiredPoints(activeTool) == 1 ? QString()
                                                            : QStringLiteral("s"));
        }
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18, viewportSize.height() - 18, hint);
    } else if (!joinActive) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Shift-click: add/remove  •  Drag empty: box select  •  G: grab selected  •  X/Y: lock axis"));
    }
}

} // namespace classiCAD
