#include "viewport_tool_preview_renderer.h"

#include "core/geometry/circle_construction.h"
#include "core/geometry/curve_construction.h"
#include "viewport_arc_compass_renderer.h"
#include "viewport_hud_renderer.h"

#include <QFont>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

bool projectFramePoint(const ViewportTransform &transform,
                       const WorkPlaneFrame &frame,
                       const QPointF &localPoint,
                       const QSize &viewportSize,
                       QPointF *screenPoint)
{
    return transform.worldPointToScreen(
        workPlaneFramePointToWorld(localPoint, frame), viewportSize, screenPoint);
}

QColor arcTwoPointGuideColor(const QPointF &localVector,
                             const WorkPlaneFrame &frame)
{
    const Point3D worldVector{
        localVector.x() * frame.xAxis.x + localVector.y() * frame.yAxis.x,
        localVector.x() * frame.xAxis.y + localVector.y() * frame.yAxis.y,
        localVector.x() * frame.xAxis.z + localVector.y() * frame.yAxis.z};
    const qreal length = std::sqrt(worldVector.x * worldVector.x +
                                   worldVector.y * worldVector.y +
                                   worldVector.z * worldVector.z);
    if (length <= 1.0e-9) {
        return QColor(72, 72, 72);
    }

    constexpr qreal axisTolerance = 0.9999;
    if (std::abs(worldVector.x) / length > axisTolerance) {
        return QColor(255, 26, 26);
    }
    if (std::abs(worldVector.y) / length > axisTolerance) {
        return QColor(26, 179, 26);
    }
    if (std::abs(worldVector.z) / length > axisTolerance) {
        return QColor(51, 128, 255);
    }
    return QColor(92, 92, 92);
}

} // namespace

ViewportToolPreviewRenderer::ViewportToolPreviewRenderer(
    const ViewportRenderer &renderer,
    const ViewportTransform &transform,
    const ViewportSnapMarkerRenderer &snapMarkerRenderer)
    : renderer_(renderer)
    , transform_(transform)
    , snapMarkerRenderer_(snapMarkerRenderer)
{
}

void ViewportToolPreviewRenderer::drawSnapMarker(
    QPainter &painter,
    SnapType type,
    const QPointF &worldPoint,
    const QSize &viewportSize) const
{
    snapMarkerRenderer_.draw(painter, type, worldPoint, viewportSize);
}

void ViewportToolPreviewRenderer::drawLinePreview(QPainter &painter,
                                      const QVector<QPointF> &pendingPoints,
                                      const QPointF &cursorWorld,
                                      bool cursorValid,
                                      const SnapResult &currentSnap,
                                      const QSize &viewportSize,
                                      bool drawCurve,
                                      const WorkPlaneFrame &workPlaneFrame) const
{
    const QColor lineColor(QStringLiteral("#e6b85c"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    const auto toScreen = [this, &workPlaneFrame, &viewportSize](const QPointF &point) {
        return isValidWorkPlaneFrame(workPlaneFrame)
                   ? transform_.workPlaneToScreen(point,
                                                  viewportSize,
                                                  workPlaneFrame)
                   : transform_.worldToScreen(point, viewportSize);
    };

    if (drawCurve) {
        painter.setPen(QPen(lineColor, 2.0));
        for (int index = 0; index + 1 < pendingPoints.size(); ++index) {
            painter.drawLine(toScreen(pendingPoints[index]),
                             toScreen(pendingPoints[index + 1]));
        }

        if (!pendingPoints.isEmpty() && cursorValid) {
            painter.drawLine(toScreen(pendingPoints.back()),
                             toScreen(cursorWorld));
        }
    }

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    for (const QPointF &point : pendingPoints) {
        painter.drawEllipse(toScreen(point), 5.0, 5.0);
    }

    if (cursorValid) {
        painter.setPen(QPen(pointColor, 2.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(toScreen(cursorWorld), 4.0, 4.0);
    }

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportToolPreviewRenderer::drawWorldLinePreview(QPainter &painter,
                                          const QVector<Point3D> &points,
                                          const Point3D &cursor,
                                          bool cursorValid,
                                          const QSize &viewportSize,
                                          bool drawCurve,
                                          const WorkPlaneFrame &workPlaneFrame) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor baseColor(QStringLiteral("#000000"));
    QColor activeColor = baseColor;
    QPointF previousScreen;
    QPointF cursorScreen;
    bool previousValid = false;
    const bool validCursor = cursorValid &&
        transform_.worldPointToScreen(cursor, viewportSize, &cursorScreen);
    if (validCursor && !points.isEmpty()) {
        Point3D direction{cursor.x - points.back().x,
                          cursor.y - points.back().y,
                          cursor.z - points.back().z};
        const qreal magnitude = std::sqrt(direction.x * direction.x +
                                          direction.y * direction.y +
                                          direction.z * direction.z);
        if (magnitude > 1.0e-12) {
            direction.x /= magnitude;
            direction.y /= magnitude;
            direction.z /= magnitude;
            if (std::abs(direction.x) > 0.9999) {
                activeColor = QColor::fromRgbF(1.0, 0.1, 0.1, 1.0);
            } else if (std::abs(direction.y) > 0.9999) {
                activeColor = QColor::fromRgbF(0.1, 0.7, 0.1, 1.0);
            } else if (std::abs(direction.z) > 0.9999) {
                activeColor = QColor::fromRgbF(0.2, 0.5, 1.0, 1.0);
            }
        }
    }

    if (points.size() >= 2) {
        transform_.worldPointToScreen(points.front(), viewportSize, &previousScreen);
        previousValid = true;
        for (int index = 1; index < points.size(); ++index) {
            QPointF nextScreen;
            const bool nextValid = transform_.worldPointToScreen(
                points[index], viewportSize, &nextScreen);
            if (drawCurve && previousValid && nextValid) {
                painter.setPen(QPen(baseColor, 1.0));
                painter.drawLine(previousScreen, nextScreen);
            }
            previousScreen = nextScreen;
            previousValid = nextValid;
        }
    } else if (!points.isEmpty()) {
        previousValid = transform_.worldPointToScreen(
            points.back(), viewportSize, &previousScreen);
    }
    if (!points.isEmpty() && previousValid && validCursor &&
        (drawCurve || activeColor != baseColor)) {
        painter.setPen(QPen(activeColor, 1.0));
        painter.drawLine(previousScreen, cursorScreen);
    }

    Point3D axisX = isValidWorkPlaneFrame(workPlaneFrame)
        ? workPlaneFrame.xAxis : Point3D{1.0, 0.0, 0.0};
    Point3D axisY = isValidWorkPlaneFrame(workPlaneFrame)
        ? workPlaneFrame.yAxis : Point3D{0.0, 1.0, 0.0};
    const auto drawCross = [&](const Point3D &worldPoint, qreal halfSize) {
        QPointF center;
        if (!transform_.worldPointToScreen(worldPoint, viewportSize, &center)) return;
        const auto projectedAxis = [&](const Point3D &axis, const QPointF &fallback) {
            const Point3D offsetPoint{worldPoint.x + axis.x,
                                      worldPoint.y + axis.y,
                                      worldPoint.z + axis.z};
            QPointF endpoint;
            if (!transform_.worldPointToScreen(offsetPoint, viewportSize, &endpoint)) {
                return fallback;
            }
            QPointF vector = endpoint - center;
            const qreal screenLength = std::hypot(vector.x(), vector.y());
            if (screenLength <= 1.0e-4) return fallback;
            return vector / screenLength;
        };
        QPointF directionX = projectedAxis(axisX, QPointF(1.0, 0.0));
        QPointF directionY = projectedAxis(axisY, QPointF(0.0, 1.0));
        if (std::abs(QPointF::dotProduct(directionX, directionY)) > 0.98) {
            directionY = QPointF(-directionX.y(), directionX.x());
        }
        painter.setPen(QPen(baseColor, 1.5));
        painter.drawLine(center - directionX * halfSize,
                         center + directionX * halfSize);
        painter.drawLine(center - directionY * halfSize,
                         center + directionY * halfSize);
    };
    for (const Point3D &point : points) drawCross(point, 2.5);
    if (validCursor) drawCross(cursor, 2.5);
    painter.restore();
}

void ViewportToolPreviewRenderer::drawArcPreview(QPainter &painter,
                                     const QVector<QPointF> &pendingPoints,
                                     ArcMode arcMode,
                                     const QPointF &cursorWorld,
                                     bool cursorValid,
                                     qreal arcSweep,
                                     const SnapResult &currentSnap,
                                     const QSize &viewportSize,
                                     const WorkPlaneFrame &workPlaneFrame,
                                     qreal compassRotation,
                                     const QColor &curveColor,
                                     bool drawCurve) const
{
    const QColor resolvedCurveColor = curveColor.isValid()
                                          ? curveColor
                                          : QColor(QStringLiteral("#e6b85c"));
    if (arcMode == ArcMode::OnePoint) {
        const WorkPlaneFrame frame = isValidWorkPlaneFrame(workPlaneFrame)
                                         ? workPlaneFrame
                                         : transform_.workPlaneFrame();
        if (isValidWorkPlaneFrame(frame)) {
            const QPointF compassCenter = pendingPoints.isEmpty()
                                              ? cursorWorld
                                              : pendingPoints.first();
            if (pendingPoints.isEmpty() ? cursorValid : true) {
                ViewportArcCompassRenderer::draw(painter,
                                                 transform_,
                                                 frame,
                                                 compassCenter,
                                                 viewportSize,
                                                 compassRotation);
            }
        }

        if (pendingPoints.isEmpty()) {
            if (currentSnap.isValid()) {
                drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
            }
            return;
        }

        if (!isValidWorkPlaneFrame(frame)) {
            return;
        }
        const QColor startColor(204, 204, 51);
        const QColor endColor(51, 204, 51);
        const auto screenPoint = [&](const QPointF &localPoint, QPointF *screen) {
            return projectFramePoint(transform_, frame, localPoint, viewportSize, screen);
        };
        QPointF centerScreen;
        if (!screenPoint(pendingPoints.first(), &centerScreen)) {
            if (currentSnap.isValid()) {
                drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
            }
            return;
        }

        if (pendingPoints.size() == 1) {
            QPointF cursorScreen;
            if (cursorValid && screenPoint(cursorWorld, &cursorScreen)) {
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(QPen(startColor, 1.0));
                painter.drawLine(centerScreen, cursorScreen);
                painter.restore();
            }
        } else {
            const QPointF center = pendingPoints[0];
            const QPointF radiusVector = pendingPoints[1] - center;
            const qreal radius = std::hypot(radiusVector.x(), radiusVector.y());
            if (radius > 1.0e-9) {
                const qreal startAngle = std::atan2(radiusVector.y(),
                                                    radiusVector.x());
                constexpr qreal twoPi = 6.28318530717958647692;
                const qreal displayedSweep = std::clamp(arcSweep,
                                                        -twoPi + 1.0e-6,
                                                        twoPi - 1.0e-6);
                QPointF startScreen;
                const bool startVisible = screenPoint(pendingPoints[1], &startScreen);

                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setBrush(Qt::NoBrush);
                if (startVisible) {
                    painter.setPen(QPen(startColor, 1.0));
                    painter.drawLine(centerScreen, startScreen);
                }

                if (drawCurve && std::abs(displayedSweep) > 1.0e-12) {
                    constexpr int previewSamplesPerRevolution = 96;
                    const int sampleCount = std::max(
                        8,
                        static_cast<int>(std::ceil(
                            std::abs(displayedSweep) * previewSamplesPerRevolution /
                            twoPi)));
                    QPolygonF preview;
                    preview.reserve(sampleCount + 1);
                    for (int index = 0; index <= sampleCount; ++index) {
                        const qreal angle = startAngle + displayedSweep *
                            (static_cast<qreal>(index) / sampleCount);
                        QPointF projected;
                        if (screenPoint(center + QPointF(radius * std::cos(angle),
                                                         radius * std::sin(angle)),
                                        &projected)) {
                            preview.append(projected);
                        }
                    }
                    painter.setPen(QPen(resolvedCurveColor, 1.0));
                    painter.drawPolyline(preview);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(resolvedCurveColor);
                    for (const QPointF &point : preview) {
                        painter.drawEllipse(point, 2.0, 2.0);
                    }
                }

                const qreal endAngle = startAngle + displayedSweep;
                QPointF endScreen;
                if (screenPoint(center + QPointF(radius * std::cos(endAngle),
                                                 radius * std::sin(endAngle)),
                                &endScreen)) {
                    painter.setPen(QPen(endColor, 1.0));
                    painter.drawLine(centerScreen, endScreen);
                }
                painter.restore();
            }
        }

        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
        }
        return;
    }

    if (arcMode == ArcMode::TwoPoint || arcMode == ArcMode::ThreePoint) {
        const WorkPlaneFrame frame = isValidWorkPlaneFrame(workPlaneFrame)
                                         ? workPlaneFrame
                                         : transform_.workPlaneFrame();
        if (!isValidWorkPlaneFrame(frame)) {
            return;
        }

        const QColor pointColor(QStringLiteral("#f0a45a"));
        const auto drawPoint = [&](const QPointF &localPoint) {
            QPointF screenPoint;
            if (!projectFramePoint(transform_, frame, localPoint,
                                   viewportSize, &screenPoint)) {
                return;
            }
            painter.setPen(QPen(pointColor, 1.25));
            painter.setBrush(QColor(QStringLiteral("#282828")));
            painter.drawEllipse(screenPoint, 4.0, 4.0);
            painter.drawLine(screenPoint + QPointF(-5.0, 0.0),
                             screenPoint + QPointF(5.0, 0.0));
            painter.drawLine(screenPoint + QPointF(0.0, -5.0),
                             screenPoint + QPointF(0.0, 5.0));
        };
        const auto drawGuide = [&](const QPointF &start,
                                   const QPointF &end,
                                   const QColor &color) {
            QPointF startScreen;
            QPointF endScreen;
            if (projectFramePoint(transform_, frame, start, viewportSize,
                                  &startScreen) &&
                projectFramePoint(transform_, frame, end, viewportSize,
                                  &endScreen)) {
                painter.setPen(QPen(color, 1.0));
                painter.drawLine(startScreen, endScreen);
            }
        };

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (pendingPoints.isEmpty()) {
            if (cursorValid) {
                drawPoint(cursorWorld);
            }
        } else if (pendingPoints.size() == 1) {
            drawPoint(pendingPoints.first());
            if (cursorValid) {
                const QPointF chord = cursorWorld - pendingPoints.first();
                drawGuide(pendingPoints.first(), cursorWorld,
                          arcTwoPointGuideColor(chord, frame));
                drawPoint(cursorWorld);
            }
        } else {
            const QPointF first = pendingPoints[0];
            const QPointF second = pendingPoints[1];
            const QPointF chord = second - first;
            const qreal chordLength = std::hypot(chord.x(), chord.y());
            drawPoint(first);
            drawPoint(second);
            drawGuide(first, second, arcTwoPointGuideColor(chord, frame));

            if (cursorValid && chordLength > 1.0e-9) {
                if (arcMode == ArcMode::TwoPoint) {
                    const QPointF midpoint = (first + second) * 0.5;
                    const QPointF heightVector = cursorWorld - midpoint;
                    drawGuide(midpoint, cursorWorld,
                              arcTwoPointGuideColor(heightVector, frame));
                }
                if (drawCurve) {
                    painter.setPen(QPen(resolvedCurveColor, 1.5));
                    renderer_.drawCircularArc(painter, first, second,
                                              cursorWorld, viewportSize);
                }
                drawPoint(cursorWorld);
            }
        }
        painter.restore();

        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point,
                           viewportSize);
        }
        return;
    }

    if (pendingPoints.isEmpty()) {
        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
        }
        return;
    }

    const QColor arcColor = resolvedCurveColor;
    const QColor pointColor(QStringLiteral("#f0a45a"));

    painter.setPen(QPen(arcColor, 2.0));
    painter.setBrush(Qt::NoBrush);

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
        if (drawCurve) {
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

void ViewportToolPreviewRenderer::drawCirclePreview(QPainter &painter,
                                        ToolId tool,
                                        const QVector<QPointF> &pendingPoints,
                                        const QPointF &cursorWorld,
                                        bool cursorValid,
                                        const SnapResult &currentSnap,
                                        const QSize &viewportSize,
                                        const WorkPlaneFrame &workPlaneFrame,
                                        const QColor &curveColor,
                                        bool drawCurve) const
{
    const QColor fallbackCurveColor(QStringLiteral("#d28b45"));
    const QColor defaultGuideColor(128, 128, 128, 180);
    const QColor pointColor(QStringLiteral("#101010"));
    const QColor pointOutlineColor(QStringLiteral("#b0b0b0"));
    const auto guideColorFor = [&](const QPointF &delta) {
        if (!isValidWorkPlaneFrame(workPlaneFrame) ||
            std::hypot(delta.x(), delta.y()) <= 1.0e-9) {
            return defaultGuideColor;
        }
        const qreal worldX = workPlaneFrame.xAxis.x * delta.x() +
                             workPlaneFrame.yAxis.x * delta.y();
        const qreal worldY = workPlaneFrame.xAxis.y * delta.x() +
                             workPlaneFrame.yAxis.y * delta.y();
        const qreal worldZ = workPlaneFrame.xAxis.z * delta.x() +
                             workPlaneFrame.yAxis.z * delta.y();
        const qreal magnitude = std::sqrt(worldX * worldX + worldY * worldY +
                                          worldZ * worldZ);
        if (magnitude <= 1.0e-9) {
            return defaultGuideColor;
        }
        const qreal x = std::abs(worldX / magnitude);
        const qreal y = std::abs(worldY / magnitude);
        const qreal z = std::abs(worldZ / magnitude);
        if (x > 0.9999) return QColor(255, 26, 26);
        if (y > 0.9999) return QColor(26, 179, 26);
        if (z > 0.9999) return QColor(51, 128, 255);
        return defaultGuideColor;
    };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (pendingPoints.isEmpty()) {
        if (cursorValid) {
            const QPointF screen = transform_.worldToScreen(cursorWorld, viewportSize);
            painter.setPen(QPen(pointOutlineColor, 1.0));
            painter.setBrush(pointColor);
            painter.drawEllipse(screen, 3.0, 3.0);
        }
        painter.restore();
        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
        }
        return;
    }

    QVector<QPointF> candidatePoints = pendingPoints;
    if (cursorValid) {
        candidatePoints.append(cursorWorld);
    }

    QVector<QPointF> definition;
    bool definitionValid = false;
    if (candidatePoints.size() >= requiredPoints(tool)) {
        if (tool == ToolId::CircleDiameter) {
            definitionValid = makeCircleDefinitionFromDiameter(candidatePoints[0],
                                                               candidatePoints[1],
                                                               &definition);
        } else if (tool == ToolId::CircleThreePoint) {
            definitionValid = makeCircleDefinitionFromThreePoints(candidatePoints[0],
                                                                  candidatePoints[1],
                                                                  candidatePoints[2],
                                                                  &definition);
        } else {
            definition = {candidatePoints[0], candidatePoints[1]};
            definitionValid = true;
        }
    }

    if (drawCurve && definitionValid) {
        const Shape::NurbsCurve2D curve = makeCircleNurbs(definition);
        if (validateNurbsCurve(curve)) {
            painter.setPen(QPen(curveColor.isValid() ? curveColor
                                                    : fallbackCurveColor,
                                1.6));
            painter.setBrush(Qt::NoBrush);
            renderer_.drawNurbsCurve(painter, curve, viewportSize);
        }
    }

    painter.setPen(QPen(defaultGuideColor, 1.0));
    if (cursorValid) {
        const QPointF previous = tool == ToolId::CircleThreePoint && pendingPoints.size() >= 2
                                     ? pendingPoints.back()
                                     : pendingPoints.first();
        const QColor guideColor = guideColorFor(cursorWorld - previous);
        painter.setPen(QPen(guideColor, 1.0));
        painter.drawLine(transform_.worldToScreen(previous, viewportSize),
                         transform_.worldToScreen(cursorWorld, viewportSize));
        if (tool == ToolId::CircleThreePoint && pendingPoints.size() >= 2) {
            painter.setPen(QPen(guideColorFor(pendingPoints[1] - pendingPoints[0]), 1.0));
            painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                             transform_.worldToScreen(pendingPoints[1], viewportSize));
        }
    }

    painter.setPen(QPen(pointOutlineColor, 1.0));
    painter.setBrush(pointColor);
    for (const QPointF &point : pendingPoints) {
        painter.drawEllipse(transform_.worldToScreen(point, viewportSize), 2.5, 2.5);
    }
    if (cursorValid) {
        const QPointF cursorScreen = transform_.worldToScreen(cursorWorld, viewportSize);
        painter.setPen(QPen(pointOutlineColor, 1.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(cursorScreen, 2.5, 2.5);
    }
    if (tool == ToolId::Circle && definitionValid && !definition.isEmpty()) {
        const QPointF centerScreen = transform_.worldToScreen(definition.first(), viewportSize);
        painter.setPen(QPen(pointOutlineColor, 1.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(centerScreen, 2.5, 2.5);
    }
    painter.restore();

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportToolPreviewRenderer::drawEllipsePreview(QPainter &painter,
                                         const QVector<QPointF> &pendingPoints,
                                         const QPointF &cursorWorld,
                                         bool cursorValid,
                                         const QVector<ToolPreviewGuide> &guides,
                                         const SnapResult &currentSnap,
                                         const QSize &viewportSize) const
{
    const QColor pointColor(QStringLiteral("#f0a45a"));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const ToolPreviewGuide &guide : guides) {
        const QColor color = guide.color.isValid()
                                 ? guide.color
                                 : QColor(QStringLiteral("#8aa7c7"));
        painter.setPen(QPen(color,
                            1.0,
                            guide.dashed ? Qt::DashLine : Qt::SolidLine));
        painter.drawLine(transform_.worldToScreen(guide.line.p1(), viewportSize),
                         transform_.worldToScreen(guide.line.p2(), viewportSize));
    }

    painter.setPen(QPen(QColor(QStringLiteral("#202020")), 1.0));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    for (const QPointF &point : pendingPoints) {
        painter.drawEllipse(transform_.worldToScreen(point, viewportSize), 2.5, 2.5);
    }
    if (cursorValid) {
        painter.setPen(QPen(QColor(QStringLiteral("#202020")), 1.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(transform_.worldToScreen(cursorWorld, viewportSize), 3.0, 3.0);
    }
    painter.restore();

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportToolPreviewRenderer::drawRectanglePreview(QPainter &painter,
                                           ToolId tool,
                                           const QVector<QPointF> &pendingPoints,
                                           const QPointF &cursorWorld,
                                           bool cursorValid,
                                           const SnapResult &currentSnap,
                                           const QSize &viewportSize,
                                           bool drawCurve) const
{
    if (pendingPoints.isEmpty()) {
        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
        }
        return;
    }

    const QColor rectangleColor(QStringLiteral("#e6b85c"));
    const QColor guideColor(QStringLiteral("#8aa7c7"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    const RectangleMode mode = rectangleModeForTool(tool);
    QVector<QPointF> candidatePoints = pendingPoints;
    if (cursorValid) {
        candidatePoints.append(cursorWorld);
    }

    QVector<QPointF> rectanglePoints;
    if (candidatePoints.size() >= requiredPoints(tool)) {
        rectanglePoints = makeRectanglePoints(mode, candidatePoints);
    }

    painter.save();
    painter.setPen(QPen(guideColor, 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    if (rectanglePoints.size() != 4 && cursorValid) {
        painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
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

    if (rectanglePoints.size() == 4) {
        QVector<QPointF> screenPoints;
        screenPoints.reserve(rectanglePoints.size());
        for (const QPointF &point : rectanglePoints) {
            screenPoints.append(transform_.worldToScreen(point, viewportSize));
        }

        if (drawCurve) {
            painter.setPen(QPen(rectangleColor, 2.0));
            painter.setBrush(Qt::NoBrush);
            for (int index = 0; index < screenPoints.size(); ++index) {
                painter.drawLine(screenPoints[index],
                                 screenPoints[(index + 1) % screenPoints.size()]);
            }
        }

        const qreal width = std::hypot(rectanglePoints[1].x() - rectanglePoints[0].x(),
                                       rectanglePoints[1].y() - rectanglePoints[0].y());
        const qreal height = std::hypot(rectanglePoints[2].x() - rectanglePoints[1].x(),
                                        rectanglePoints[2].y() - rectanglePoints[1].y());
        QRectF bounds(screenPoints.first(), screenPoints.first());
        for (const QPointF &point : screenPoints) {
            bounds = bounds.united(QRectF(point, point));
        }
        painter.setPen(QColor(QStringLiteral("#d0d0d0")));
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        const QString dimensions = QStringLiteral("W %1  H %2")
                                       .arg(width, 0, 'f', 2)
                                       .arg(height, 0, 'f', 2);
        QPointF labelPosition = bounds.topLeft() + QPointF(6.0, -8.0);
        if (labelPosition.y() < 14.0) {
            labelPosition.setY(bounds.top() + 16.0);
        }
        painter.drawText(labelPosition, dimensions);
    }

    painter.restore();

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportToolPreviewRenderer::drawPolygonPreview(QPainter &painter,
                                         ToolId tool,
                                         int sideCount,
                                         const QVector<QPointF> &pendingPoints,
                                         const QPointF &cursorWorld,
                                         bool cursorValid,
                                         const SnapResult &currentSnap,
                                         const QSize &viewportSize,
                                         bool drawCurve) const
{
    if (pendingPoints.isEmpty()) {
        if (currentSnap.isValid()) {
            drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
        }
        return;
    }

    const QColor polygonColor(QStringLiteral("#e6b85c"));
    const QColor guideColor = cursorValid
                                  ? arcTwoPointGuideColor(
                                        cursorWorld - pendingPoints.first(),
                                        transform_.workPlaneFrame())
                                  : QColor(92, 92, 92);
    const QColor pointColor(QStringLiteral("#f0a45a"));
    QVector<QPointF> candidatePoints = pendingPoints;
    if (cursorValid) {
        candidatePoints.append(cursorWorld);
    }

    const QVector<QPointF> vertices = candidatePoints.size() >= 2
                                          ? makeRegularPolygonPoints(
                                                polygonModeForTool(tool),
                                                candidatePoints,
                                                sideCount)
                                          : QVector<QPointF>{};

    painter.save();
    painter.setBrush(Qt::NoBrush);
    if (cursorValid) {
        painter.setPen(QPen(guideColor,
                            1.0,
                            vertices.size() < 3 ? Qt::DashLine : Qt::SolidLine));
        painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                         transform_.worldToScreen(cursorWorld, viewportSize));
    }
    if (drawCurve && vertices.size() >= 3) {
        painter.setPen(QPen(polygonColor, 2.0));
        for (int index = 0; index < vertices.size(); ++index) {
            painter.drawLine(transform_.worldToScreen(vertices[index], viewportSize),
                             transform_.worldToScreen(
                                 vertices[(index + 1) % vertices.size()], viewportSize));
        }
    }

    painter.setPen(QPen(pointColor, 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    for (const QPointF &point : pendingPoints) {
        painter.drawEllipse(transform_.worldToScreen(point, viewportSize), 5.0, 5.0);
    }
    if (cursorValid) {
        const QPointF cursorScreen = transform_.worldToScreen(cursorWorld, viewportSize);
        painter.setPen(QPen(pointColor, 2.0));
        painter.setBrush(pointColor);
        painter.drawEllipse(cursorScreen, 4.0, 4.0);

        painter.setPen(QColor(QStringLiteral("#d0d0d0")));
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        painter.drawText(cursorScreen + QPointF(10.0, -10.0),
                         QStringLiteral("%1 sides").arg(sideCount));
    }
    painter.restore();

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportToolPreviewRenderer::drawPointPreview(QPainter &painter,
                                       const QPointF &cursorWorld,
                                       bool cursorValid,
                                       const SnapResult &currentSnap,
                                       const QSize &viewportSize,
                                       bool drawPoint) const
{
    if (!cursorValid) {
        return;
    }

    const QPointF screenPoint = transform_.worldToScreen(cursorWorld, viewportSize);
    const QColor pointColor(QStringLiteral("#e6b85c"));
    if (drawPoint) {
        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(pointColor);
        painter.drawEllipse(screenPoint, 4.5, 4.5);
    }
    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportToolPreviewRenderer::drawRotatePreview(QPainter &painter,
                                        const QPointF &cursorWorld,
                                        bool cursorValid,
                                        int rotateStep,
                                        const WorkPlaneFrame &rotateFrame,
                                        const QPointF &rotateBaseWorld,
                                        const QPointF &rotateReferenceWorld,
                                        qreal rotatePreviewAngle,
                                        const QString &rotateAngleInput,
                                        bool rotateAngleInputActive,
                                        bool rotateAngleSnapEnabled,
                                        qreal rotateAngleSnapIncrementDegrees,
                                        bool rotateAngleInputInRadians,
                                        const SnapResult &currentSnap,
                                        const QSize &viewportSize,
                                        bool drawGeometry) const
{
    if (!cursorValid) {
        return;
    }

    const QColor rotateColor(QStringLiteral("#e6b85c"));
    const QColor guideColor(QStringLiteral("#8aa7c7"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    const WorkPlaneFrame frame = isValidWorkPlaneFrame(rotateFrame)
                                     ? rotateFrame
                                     : transform_.workPlaneFrame();
    QPointF cursorScreen;
    if (!transform_.worldPointToScreen(
            workPlaneFramePointToWorld(cursorWorld, frame),
            viewportSize,
            &cursorScreen)) {
        return;
    }

    QPointF compassCenter = cursorWorld;
    qreal compassRotation = 0.0;
    if (rotateStep >= 1) {
        compassCenter = rotateBaseWorld;
        if (rotateStep == 1) {
            const QPointF reference = cursorWorld - rotateBaseWorld;
            compassRotation = std::atan2(reference.y(), reference.x());
        } else {
            const QPointF reference = rotateReferenceWorld - rotateBaseWorld;
            compassRotation = std::atan2(reference.y(), reference.x());
        }
    }
    ViewportArcCompassRenderer::draw(painter,
                                     transform_,
                                     frame,
                                     compassCenter,
                                     viewportSize,
                                     compassRotation,
                                     rotateAngleSnapIncrementDegrees);

    if (drawGeometry) {
        painter.setBrush(Qt::NoBrush);
        if (rotateStep == 0) {
            painter.setPen(QPen(pointColor, 1.5));
            painter.drawEllipse(cursorScreen, 6.0, 6.0);
        } else {
            const QPointF baseScreen = transform_.workPlaneToScreen(
                rotateBaseWorld, viewportSize, frame);
            painter.setPen(QPen(pointColor, 1.5));
            painter.drawEllipse(baseScreen, 6.0, 6.0);
            if (rotateStep >= 1) {
                painter.setPen(QPen(guideColor, 1.2, Qt::DashLine));
                painter.drawLine(baseScreen, cursorScreen);
            }
            if (rotateStep >= 2) {
                const QPointF referenceScreen =
                    transform_.workPlaneToScreen(rotateReferenceWorld,
                                                 viewportSize,
                                                 frame);
                painter.setPen(QPen(guideColor, 1.0, Qt::DashLine));
                painter.drawLine(baseScreen, referenceScreen);
                painter.setPen(QPen(rotateColor, 1.6));
                painter.drawLine(baseScreen, cursorScreen);
            }
        }
        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(pointColor);
        painter.drawEllipse(cursorScreen, 4.0, 4.0);
    }
    if (rotateStep >= 2) {
        painter.setPen(QColor(QStringLiteral("#d0d0d0")));
        painter.setFont(QFont(QStringLiteral("Sans"), 9));
        const QString angleUnit = rotateAngleInputInRadians
                                      ? QStringLiteral(" rad")
                                      : QStringLiteral("°");
        const QString angleText = rotateAngleInputActive &&
                                          !rotateAngleInput.isEmpty()
                                      ? QStringLiteral("∠ %1°")
                                            .arg(rotateAngleInput)
                                      : QStringLiteral("∠ %1%2%3")
                                            .arg(rotateAngleInputInRadians
                                                     ? rotatePreviewAngle
                                                     : rotatePreviewAngle *
                                                           180.0 /
                                                           3.14159265358979323846,
                                                 0,
                                                 'f',
                                                 rotateAngleInputInRadians ? 3 : 1)
                                            .arg(angleUnit)
                                            .arg(rotateAngleSnapEnabled
                                                     ? QStringLiteral("  •  %1")
                                                           .arg(ViewportHudRenderer::rotateSnapIncrementLabel(
                                                               rotateAngleSnapIncrementDegrees,
                                                               rotateAngleInputInRadians))
                                                     : QString());
        painter.drawText(cursorScreen + QPointF(12.0, -10.0), angleText);
    }
    if (currentSnap.isValid() &&
        !(rotateAngleInputActive && !rotateAngleInput.isEmpty())) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}


} // namespace classiCAD
