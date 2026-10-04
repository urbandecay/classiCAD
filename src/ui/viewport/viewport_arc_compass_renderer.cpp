#include "viewport_arc_compass_renderer.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

Point3D scalePoint(const Point3D &point, qreal scale)
{
    return {point.x * scale, point.y * scale, point.z * scale};
}

QColor arcCompassColor(const Point3D &normal)
{
    constexpr qreal axisTolerance = 0.999999;
    if (std::abs(normal.x) > axisTolerance) {
        return QColor::fromRgbF(0.85, 0.0, 0.0, 1.0);
    }
    if (std::abs(normal.y) > axisTolerance) {
        return QColor::fromRgbF(0.0, 0.60, 0.0, 1.0);
    }
    if (std::abs(normal.z) > axisTolerance) {
        return QColor::fromRgbF(0.149, 0.376, 1.0, 1.0);
    }
    // The add-on uses black for oblique workplanes, which disappears against
    // classiCAD's dark viewport. Keep the same neutral compass treatment with
    // a light stroke so the overlay remains visible on that background.
    return QColor(225, 225, 225, 255);
}

} // namespace

void ViewportArcCompassRenderer::draw(QPainter &painter,
                    const ViewportTransform &transform,
                    const WorkPlaneFrame &frame,
                    const QPointF &localCenter,
                    const QSize &viewportSize,
                    qreal rotation,
                    qreal angleIncrementDegrees)
{
    constexpr qreal pi = 3.14159265358979323846;
    constexpr int ringSamples = 72;
    constexpr qreal compassRadiusPixels = 62.5;
    if (!isValidWorkPlaneFrame(frame)) {
        return;
    }

    const Point3D centerWorld = workPlaneFramePointToWorld(localCenter, frame);
    QPointF centerScreen;
    const Point3D rightUnnormalized = {
        transform.viewUp().y * transform.viewDirection().z -
            transform.viewUp().z * transform.viewDirection().y,
        transform.viewUp().z * transform.viewDirection().x -
            transform.viewUp().x * transform.viewDirection().z,
        transform.viewUp().x * transform.viewDirection().y -
            transform.viewUp().y * transform.viewDirection().x};
    const qreal rightLength = std::sqrt(
        rightUnnormalized.x * rightUnnormalized.x +
        rightUnnormalized.y * rightUnnormalized.y +
        rightUnnormalized.z * rightUnnormalized.z);
    if (rightLength <= 1.0e-12 ||
        !transform.worldPointToScreenUnclipped(centerWorld,
                                               viewportSize,
                                               &centerScreen)) {
        return;
    }
    const Point3D cameraRight = scalePoint(rightUnnormalized, 1.0 / rightLength);
    QPointF cameraRightScreen;
    const Point3D cameraRightProbe{centerWorld.x + cameraRight.x,
                                   centerWorld.y + cameraRight.y,
                                   centerWorld.z + cameraRight.z};
    if (!transform.worldPointToScreenUnclipped(cameraRightProbe,
                                               viewportSize,
                                               &cameraRightScreen)) {
        return;
    }
    const qreal pixelsPerWorldUnit = std::hypot(
        cameraRightScreen.x() - centerScreen.x(),
        cameraRightScreen.y() - centerScreen.y());
    if (!std::isfinite(pixelsPerWorldUnit) || pixelsPerWorldUnit <= 1.0e-9) {
        return;
    }
    // Match the add-on: size the world-space compass from camera-right at its
    // center depth, then project each workplane point through the actual view.
    // This keeps the rotated marks on the same projected rays as the arc guide
    // in perspective views as well as orthographic views.
    const qreal compassRadiusWorld = compassRadiusPixels / pixelsPerWorldUnit;
    constexpr qreal outerRadius = 1.0;
    const qreal innerRadius = outerRadius * (80.0 / 120.0);
    const qreal tickLength = outerRadius * (10.0 / 120.0);
    const qreal crossLength = outerRadius * (10.0 / 120.0);
    const qreal cosine = std::cos(rotation);
    const qreal sine = std::sin(rotation);
    const auto rotate = [cosine, sine](qreal x, qreal y) {
        return QPointF(x * cosine - y * sine,
                       x * sine + y * cosine);
    };
    const auto projectCompassPoint = [&](qreal x,
                                         qreal y,
                                         bool rotatePoint,
                                         QPointF *screen) {
        const QPointF rotated = rotatePoint ? rotate(x, y) : QPointF(x, y);
        if (screen == nullptr) {
            return false;
        }
        const Point3D worldPoint{
            centerWorld.x + compassRadiusWorld *
                                (frame.xAxis.x * rotated.x() + frame.yAxis.x * rotated.y()),
            centerWorld.y + compassRadiusWorld *
                                (frame.xAxis.y * rotated.x() + frame.yAxis.y * rotated.y()),
            centerWorld.z + compassRadiusWorld *
                                (frame.xAxis.z * rotated.x() + frame.yAxis.z * rotated.y())};
        return transform.worldPointToScreenUnclipped(worldPoint,
                                                     viewportSize,
                                                     screen);
    };
    const auto drawCompassLine = [&](qreal firstX,
                                     qreal firstY,
                                     qreal secondX,
                                     qreal secondY,
                                     bool rotateLine) {
        QPointF first;
        QPointF second;
        if (projectCompassPoint(firstX, firstY, rotateLine, &first) &&
            projectCompassPoint(secondX, secondY, rotateLine, &second)) {
            painter.drawLine(first, second);
        }
    };

    const QColor color = arcCompassColor(frame.normal);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 1.0));

    for (int sample = 0; sample < ringSamples; ++sample) {
        const qreal firstAngle = 2.0 * pi * sample / ringSamples;
        const qreal secondAngle = 2.0 * pi * (sample + 1) / ringSamples;
        drawCompassLine(outerRadius * std::cos(firstAngle),
                        outerRadius * std::sin(firstAngle),
                        outerRadius * std::cos(secondAngle),
                        outerRadius * std::sin(secondAngle),
                        true);
    }

    const int tickCount = std::max(
        1,
        qRound(360.0 / std::max<qreal>(angleIncrementDegrees, 1.0e-9)));
    for (int tick = 0; tick < tickCount; ++tick) {
        const qreal angle = 2.0 * pi * tick / tickCount;
        drawCompassLine(outerRadius * std::cos(angle),
                        outerRadius * std::sin(angle),
                        (outerRadius - tickLength) * std::cos(angle),
                        (outerRadius - tickLength) * std::sin(angle),
                        true);
    }

    const auto drawProtractorArc = [&](qreal startAngle, qreal endAngle) {
        constexpr int arcSamples = 48;
        QPointF previous;
        QPointF arcStart;
        const bool startVisible = projectCompassPoint(
            innerRadius * std::cos(startAngle),
            innerRadius * std::sin(startAngle),
            true,
            &previous);
        if (!startVisible) {
            return;
        }
        arcStart = previous;
        bool previousVisible = true;
        for (int sample = 1; sample <= arcSamples; ++sample) {
            const qreal fraction = static_cast<qreal>(sample) / arcSamples;
            const qreal angle = startAngle + (endAngle - startAngle) * fraction;
            QPointF next;
            const bool nextVisible = projectCompassPoint(
                innerRadius * std::cos(angle),
                innerRadius * std::sin(angle),
                true,
                &next);
            if (previousVisible && nextVisible) {
                painter.drawLine(previous, next);
            }
            previous = next;
            previousVisible = nextVisible;
        }
        QPointF arcEnd;
        if (projectCompassPoint(innerRadius * std::cos(endAngle),
                                innerRadius * std::sin(endAngle),
                                true,
                                &arcEnd)) {
            painter.drawLine(arcStart, arcEnd);
        }
    };
    drawProtractorArc(200.0 * pi / 180.0, 340.0 * pi / 180.0);
    drawProtractorArc(20.0 * pi / 180.0, 160.0 * pi / 180.0);

    drawCompassLine(-crossLength, 0.0, crossLength, 0.0, false);
    drawCompassLine(0.0, -crossLength, 0.0, crossLength, false);
    painter.restore();
}

} // namespace classiCAD
