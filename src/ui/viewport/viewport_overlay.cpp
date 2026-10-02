#include "viewport_overlay.h"

#include "core/geometry/circle_construction.h"

#include <QFont>
#include <QImage>
#include <QLineF>
#include <QPolygonF>
#include <QPainterPath>
#include <QRegion>

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr qreal blenderAxisRadius = 32.0;
constexpr qreal blenderAxisMarkerRadius = 8.0;
constexpr qreal blenderMiniButtonSize = 28.0;
constexpr qreal blenderGizmoSupersampling = 2.0;

Point3D scalePoint(const Point3D &point, qreal scale)
{
    return {point.x * scale, point.y * scale, point.z * scale};
}

bool projectFramePoint(const ViewportTransform &transform,
                       const WorkPlaneFrame &frame,
                       const QPointF &localPoint,
                       const QSize &viewportSize,
                       QPointF *screenPoint)
{
    return transform.worldPointToScreen(
        workPlaneFramePointToWorld(localPoint, frame), viewportSize, screenPoint);
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

void drawArcCompass(QPainter &painter,
                    const ViewportTransform &transform,
                    const WorkPlaneFrame &frame,
                    const QPointF &localCenter,
                    const QSize &viewportSize,
                    qreal rotation)
{
    constexpr qreal pi = 3.14159265358979323846;
    constexpr int ringSamples = 72;
    constexpr int angleIncrementDegrees = 15;
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

    constexpr int tickCount = 360 / angleIncrementDegrees;
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

struct BlenderAxisMarker {
    Point3D direction;
    QPointF screenPosition;
    qreal depth = 0.0;
    int axis = 0;
    bool positive = true;
};

QPointF blenderAxisCenter(const QSize &viewportSize)
{
    return QPointF(viewportSize.width() - 50.0, 50.0);
}

QColor blenderAxisColor(int axis)
{
    switch (axis) {
    case 0:
        return QColor(QStringLiteral("#e75b61"));
    case 1:
        return QColor(QStringLiteral("#78bd55"));
    default:
        return QColor(QStringLiteral("#4b91df"));
    }
}

QString blenderAxisName(int axis)
{
    switch (axis) {
    case 0:
        return QStringLiteral("X");
    case 1:
        return QStringLiteral("Y");
    default:
        return QStringLiteral("Z");
    }
}

qreal blenderDirectionDot(const Point3D &first, const Point3D &second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

QRectF blenderMiniButtonRect(BlenderNavigationAction action,
                             const QSize &viewportSize)
{
    int slot = -1;
    switch (action) {
    case BlenderNavigationAction::Zoom:
        slot = 0;
        break;
    case BlenderNavigationAction::Pan:
        slot = 1;
        break;
    case BlenderNavigationAction::Camera:
        slot = 2;
        break;
    case BlenderNavigationAction::Projection:
        slot = 3;
        break;
    default:
        return {};
    }
    const qreal top = blenderAxisCenter(viewportSize).y() +
                      blenderAxisRadius + 20.0 + slot * blenderMiniButtonSize;
    return QRectF(viewportSize.width() - 36.0,
                  top,
                  blenderMiniButtonSize,
                  blenderMiniButtonSize);
}

QImage loadBlenderNavigationIcon(const char *resourcePath)
{
    QImage image(QString::fromLatin1(resourcePath));
    image.setDevicePixelRatio(2.0);
    return image;
}

const QImage &blenderNavigationIcon(BlenderNavigationAction action,
                                   bool perspectiveEnabled)
{
    static const QImage zoom = loadBlenderNavigationIcon(
        ":/blender-navigation/zoom.png");
    static const QImage pan = loadBlenderNavigationIcon(
        ":/blender-navigation/pan.png");
    static const QImage camera = loadBlenderNavigationIcon(
        ":/blender-navigation/camera.png");
    static const QImage perspective = loadBlenderNavigationIcon(
        ":/blender-navigation/perspective.png");
    static const QImage iso = loadBlenderNavigationIcon(
        ":/blender-navigation/iso.png");

    switch (action) {
    case BlenderNavigationAction::Zoom:
        return zoom;
    case BlenderNavigationAction::Pan:
        return pan;
    case BlenderNavigationAction::Camera:
        return camera;
    case BlenderNavigationAction::Projection:
        return perspectiveEnabled ? perspective : iso;
    default: {
        static const QImage noIcon;
        return noIcon;
    }
    }
}

QVector<BlenderAxisMarker> blenderAxisMarkers(const ViewportTransform &transform,
                                             const QSize &viewportSize)
{
    const QPointF center = blenderAxisCenter(viewportSize);
    QVector<BlenderAxisMarker> markers;
    markers.reserve(6);
    for (int axis = 0; axis < 3; ++axis) {
        for (const qreal sign : {1.0, -1.0}) {
            Point3D direction;
            if (axis == 0) {
                direction.x = sign;
            } else if (axis == 1) {
                direction.y = sign;
            } else {
                direction.z = sign;
            }
            const ViewportDirectionProjection projected =
                transform.worldDirectionToView(direction);
            markers.append({direction,
                            center + QPointF(projected.horizontal,
                                             -projected.vertical) * blenderAxisRadius,
                            projected.towardCamera,
                            axis,
                            sign > 0.0});
        }
    }
    std::sort(markers.begin(), markers.end(), [](const BlenderAxisMarker &first,
                                                 const BlenderAxisMarker &second) {
        return first.depth < second.depth;
    });
    return markers;
}

void drawBlenderNavigationButton(QPainter &painter,
                                 BlenderNavigationAction action,
                                 const QSize &viewportSize,
                                 const QPointF &hoverPosition,
                                 bool perspectiveEnabled)
{
    const QRectF button = blenderMiniButtonRect(action, viewportSize);
    const bool hovered = button.contains(hoverPosition);
    if (hovered) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(95, 95, 95, 110));
        painter.drawRoundedRect(button.adjusted(3.0, 3.0, -3.0, -3.0), 3.0, 3.0);
    }

    const QPointF center = button.center();
    const QImage &icon = blenderNavigationIcon(action, perspectiveEnabled);
    if (icon.isNull()) {
        return;
    }

    const qreal iconDpr = icon.devicePixelRatio();
    const QSizeF sourceLogicalSize(icon.width() / iconDpr,
                                  icon.height() / iconDpr);
    constexpr qreal maximumIconExtent = 17.0;
    const qreal scale = maximumIconExtent /
        std::max(sourceLogicalSize.width(), sourceLogicalSize.height());
    const QSizeF iconSize(sourceLogicalSize.width() * scale,
                          sourceLogicalSize.height() * scale);
    const QRectF iconRect(center.x() - iconSize.width() / 2.0,
                          center.y() - iconSize.height() / 2.0,
                          iconSize.width(),
                          iconSize.height());
    painter.save();
    painter.setOpacity(hovered ? 1.0 : 0.65);
    painter.drawImage(iconRect, icon);
    painter.restore();
}

} // namespace

ViewportOverlay::ViewportOverlay(const ViewportRenderer &renderer,
                                 const ViewportTransform &transform)
    : renderer_(renderer)
    , transform_(transform)
{
}

void ViewportOverlay::setSnapLabelsVisible(bool visible)
{
    snapLabelsVisible_ = visible;
}

void ViewportOverlay::drawSnapMarker(QPainter &painter,
                                     SnapType type,
                                     const QPointF &worldPoint,
                                     const QSize &viewportSize) const
{
    painter.save();
    const QPointF snapScreen = transform_.worldToScreen(worldPoint, viewportSize);
    const QColor snapColor(QStringLiteral("#63b5e8"));
    painter.setPen(QPen(snapColor, 2.0));
    painter.setBrush(Qt::NoBrush);

    if (type == SnapType::Endpoint) {
        painter.drawEllipse(snapScreen, 7.0, 7.0);
    } else if (type == SnapType::Midpoint) {
        painter.drawRect(QRectF(snapScreen - QPointF(6.0, 6.0),
                                snapScreen + QPointF(6.0, 6.0)));
    } else if (type == SnapType::ControlPoint) {
        painter.drawRect(QRectF(snapScreen - QPointF(7.0, 7.0),
                                snapScreen + QPointF(7.0, 7.0)));
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
    } else if (type == SnapType::Near) {
        const QPointF diamond[] = {snapScreen + QPointF(0.0, -7.0),
                                   snapScreen + QPointF(7.0, 0.0),
                                   snapScreen + QPointF(0.0, 7.0),
                                   snapScreen + QPointF(-7.0, 0.0)};
        painter.drawPolygon(diamond, 4);
    }

    if (snapLabelsVisible_ && type != SnapType::None) {
        painter.setPen(snapColor);
        painter.setFont(QFont(QStringLiteral("Sans"), 9, QFont::Bold));
        painter.drawText(snapScreen + QPointF(10.0, -10.0), snapTypeName(type));
    }
    painter.restore();
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
                                        int activeControlPointIndex,
                                        bool drawMarkers) const
{
    renderer_.drawControlPoints(painter,
                                shape,
                                viewportSize,
                                shapeObjectId,
                                selectedObjectId,
                                draggingControlPoint,
                                activeControlPointIndex,
                                drawMarkers);
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

void ViewportOverlay::drawWorldLinePreview(QPainter &painter,
                                          const QVector<Point3D> &points,
                                          const Point3D &cursor,
                                          bool cursorValid,
                                          const QSize &viewportSize,
                                          bool drawCurve) const
{
    painter.save();
    QPointF previous;
    bool previousValid = false;
    painter.setPen(QPen(QColor(QStringLiteral("#e6b85c")), 2.0));
    for (const Point3D &point : points) {
        QPointF screen;
        const bool valid = transform_.worldPointToScreen(point, viewportSize, &screen);
        if (drawCurve && previousValid && valid) painter.drawLine(previous, screen);
        previous = screen;
        previousValid = valid;
    }
    QPointF cursorScreen;
    const bool validCursor = cursorValid &&
        transform_.worldPointToScreen(cursor, viewportSize, &cursorScreen);
    if (drawCurve && previousValid && validCursor) painter.drawLine(previous, cursorScreen);
    painter.setPen(QPen(QColor(QStringLiteral("#f0a45a")), 1.5));
    painter.setBrush(QColor(QStringLiteral("#282828")));
    for (const Point3D &point : points) {
        QPointF screen;
        if (transform_.worldPointToScreen(point, viewportSize, &screen)) {
            painter.drawEllipse(screen, 5.0, 5.0);
        }
    }
    if (validCursor) {
        painter.setBrush(QColor(QStringLiteral("#f0a45a")));
        painter.drawEllipse(cursorScreen, 4.0, 4.0);
    }
    painter.restore();
}

void ViewportOverlay::drawArcPreview(QPainter &painter,
                                     const QVector<QPointF> &pendingPoints,
                                     ArcMode arcMode,
                                     const QPointF &cursorWorld,
                                     bool cursorValid,
                                     qreal arcSweep,
                                     const SnapResult &currentSnap,
                                     const QSize &viewportSize,
                                     const WorkPlaneFrame &workPlaneFrame,
                                     qreal compassRotation,
                                     bool drawCurve) const
{
    if (arcMode == ArcMode::OnePoint) {
        const WorkPlaneFrame frame = isValidWorkPlaneFrame(workPlaneFrame)
                                         ? workPlaneFrame
                                         : transform_.workPlaneFrame();
        if (isValidWorkPlaneFrame(frame)) {
            const QPointF compassCenter = pendingPoints.isEmpty()
                                              ? cursorWorld
                                              : pendingPoints.first();
            if (pendingPoints.isEmpty() ? cursorValid : true) {
                drawArcCompass(painter,
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
                    painter.setPen(QPen(QColor(0, 0, 0), 1.0));
                    painter.drawPolyline(preview);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(0, 0, 0));
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

void ViewportOverlay::drawCirclePreview(QPainter &painter,
                                        ToolId tool,
                                        const QVector<QPointF> &pendingPoints,
                                        const QPointF &cursorWorld,
                                        bool cursorValid,
                                        const SnapResult &currentSnap,
                                        const QSize &viewportSize,
                                        bool drawCurve) const
{
    if (pendingPoints.isEmpty()) {
        return;
    }

    const QColor circleColor(QStringLiteral("#e6b85c"));
    const QColor guideColor(QStringLiteral("#8aa7c7"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
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
            painter.save();
            painter.setPen(QPen(circleColor, 2.0));
            painter.setBrush(Qt::NoBrush);
            renderer_.drawNurbsCurve(painter, curve, viewportSize);
            painter.restore();
        }
    }

    painter.save();
    painter.setPen(QPen(guideColor, 1.0, Qt::DashLine));
    if (cursorValid) {
        const QPointF previous = tool == ToolId::CircleThreePoint && pendingPoints.size() >= 2
                                     ? pendingPoints.back()
                                     : pendingPoints.first();
        painter.drawLine(transform_.worldToScreen(previous, viewportSize),
                         transform_.worldToScreen(cursorWorld, viewportSize));
        if (tool == ToolId::CircleThreePoint && pendingPoints.size() >= 2) {
            painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                             transform_.worldToScreen(pendingPoints[1], viewportSize));
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
    }
    if (definitionValid && !definition.isEmpty()) {
        const QPointF centerScreen = transform_.worldToScreen(definition.first(), viewportSize);
        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(QColor(QStringLiteral("#282828")));
        painter.drawEllipse(centerScreen, 5.0, 5.0);
    }
    painter.restore();

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawEllipsePreview(QPainter &painter,
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

    const QColor curveColor(QStringLiteral("#e6b85c"));
    const QColor guideColor(QStringLiteral("#8aa7c7"));
    const QColor pointColor(QStringLiteral("#f0a45a"));
    QVector<QPointF> candidatePoints = pendingPoints;
    if (cursorValid) {
        candidatePoints.append(cursorWorld);
    }

    const EllipseMode mode = ellipseModeForTool(tool);
    const int requiredPointCount = requiredPoints(tool);
    if (drawCurve && candidatePoints.size() >= requiredPointCount) {
        const Shape::NurbsCurve2D curve = makeEllipseNurbs(mode, candidatePoints);
        if (validateNurbsCurve(curve)) {
            painter.save();
            painter.setPen(QPen(curveColor, 2.0));
            painter.setBrush(Qt::NoBrush);
            renderer_.drawNurbsCurve(painter, curve, viewportSize);
            painter.restore();
        }
    }

    painter.save();
    painter.setPen(QPen(guideColor, 1.0, Qt::DashLine));
    if (cursorValid) {
        const QPointF cursorScreen = transform_.worldToScreen(cursorWorld, viewportSize);
        if (mode == EllipseMode::Corners) {
            const QPointF cornerScreen = transform_.worldToScreen(pendingPoints.first(),
                                                                  viewportSize);
            painter.drawRect(QRectF(cornerScreen, cursorScreen).normalized());
        } else if (pendingPoints.size() == 1) {
            painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                             cursorScreen);
        } else if (mode == EllipseMode::AxisEndpoints ||
                   mode == EllipseMode::FociPoint) {
            painter.drawLine(transform_.worldToScreen(pendingPoints[0], viewportSize),
                             transform_.worldToScreen(pendingPoints[1], viewportSize));
        } else if (mode == EllipseMode::CenterAxisRadius) {
            painter.drawLine(transform_.worldToScreen(pendingPoints[0], viewportSize),
                             transform_.worldToScreen(pendingPoints[1], viewportSize));
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
    painter.restore();

    if (currentSnap.isValid()) {
        drawSnapMarker(painter, currentSnap.type, currentSnap.point, viewportSize);
    }
}

void ViewportOverlay::drawRectanglePreview(QPainter &painter,
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

void ViewportOverlay::drawPolygonPreview(QPainter &painter,
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
    const QColor guideColor(QStringLiteral("#8aa7c7"));
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
    if (vertices.size() < 3 && cursorValid) {
        painter.setPen(QPen(guideColor, 1.0, Qt::DashLine));
        painter.drawLine(transform_.worldToScreen(pendingPoints.first(), viewportSize),
                         transform_.worldToScreen(cursorWorld, viewportSize));
    } else if (drawCurve && vertices.size() >= 3) {
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

void ViewportOverlay::drawPointPreview(QPainter &painter,
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

void ViewportOverlay::drawRotatePreview(QPainter &painter,
                                        const QPointF &cursorWorld,
                                        bool cursorValid,
                                        int rotateStep,
                                        const QPointF &rotateBaseWorld,
                                        const QPointF &rotateReferenceWorld,
                                        qreal rotatePreviewAngle,
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
    const QPointF cursorScreen = transform_.worldToScreen(cursorWorld, viewportSize);

    if (drawGeometry) {
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
            }
        }
        painter.setPen(QPen(pointColor, 1.5));
        painter.setBrush(pointColor);
        painter.drawEllipse(cursorScreen, 4.0, 4.0);
    }
    if (rotateStep >= 2) {
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
                                     int rotateStep,
                                     bool grabActive,
                                     bool grabPickingBasePoint,
                                     bool grabHasBasePoint,
                                     bool duplicateActive,
                                     bool duplicatePickingBasePoint,
                                     bool duplicateHasBasePoint) const
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
    } else if (activeTool == Tool::Picture) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click the first corner, then the opposite corner  •  Aspect ratio is preserved  •  Esc cancels"));
    } else if (activeTool == Tool::TangentFromCurve) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Click a curve, then click the line endpoint  •  Esc cancels"));
    } else if (activeTool == Tool::PerpendicularFromCurve) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(
            18,
            viewportSize.height() - 18,
            QStringLiteral("Click a curve, then click the perpendicular line endpoint  •  Esc cancels"));
    } else if (duplicateActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        const QString duplicateHint = duplicatePickingBasePoint
                                          ? QStringLiteral("DUPLICATE  •  Click a base point on the selection  •  Esc/RMB cancels")
                                          : duplicateHasBasePoint
                                                ? QStringLiteral("DUPLICATE  •  Move preview to destination  •  Click to place  •  Esc cancels")
                                                : QStringLiteral("DUPLICATE  •  Click a base point on the selection  •  Esc/RMB cancels");
        painter.drawText(18, viewportSize.height() - 18, duplicateHint);
    } else if (activeTool != Tool::Select && activeTool != Tool::Arc) {
        QString hint;
        if (isRectangleTool(activeTool)) {
            QString inputDescription;
            switch (rectangleModeForTool(activeTool)) {
            case RectangleMode::CornerCorner:
                inputDescription = QStringLiteral("first corner, opposite corner");
                break;
            case RectangleMode::CenterCorner:
                inputDescription = QStringLiteral("center, corner");
                break;
            case RectangleMode::ThreePoint:
                inputDescription = QStringLiteral("first edge point, second edge point, width point");
                break;
            }
            hint = QStringLiteral("Click to place %1  •  Esc clears current tool input")
                       .arg(inputDescription);
        } else if (isEllipseTool(activeTool)) {
            QString inputDescription;
            switch (ellipseModeForTool(activeTool)) {
            case EllipseMode::CenterAxisRadius:
                inputDescription = QStringLiteral("center, axis end, minor radius");
                break;
            case EllipseMode::AxisEndpoints:
                inputDescription = QStringLiteral("first axis end, second axis end, minor radius");
                break;
            case EllipseMode::Corners:
                inputDescription = QStringLiteral("opposite bounding-box corners");
                break;
            case EllipseMode::FociPoint:
                inputDescription = QStringLiteral("first focus, second focus, point on ellipse");
                break;
            }
            hint = QStringLiteral("Click to place %1  •  Esc clears current tool input")
                       .arg(inputDescription);
        } else if (activeTool == Tool::Arc) {
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
    } else if (grabActive) {
        painter.setPen(QColor(QStringLiteral("#f0a45a")));
        const QString grabHint = grabPickingBasePoint
                                     ? QStringLiteral("Click an OSnap point on the selection for the move base  •  Esc/RMB cancels")
                                     : grabHasBasePoint
                                           ? QStringLiteral("Move base point to destination  •  X/Y: constrain  •  Click: confirm  •  Esc: cancel")
                                           : QStringLiteral("Move selection  •  X/Y: constrain  •  B: choose base point  •  Click: confirm  •  Esc: cancel");
        painter.drawText(18, viewportSize.height() - 18, grabHint);
    }
}

void ViewportOverlay::drawBlenderNavigationGizmo(
    QPainter &painter,
    const QSize &viewportSize,
    const QPointF &hoverPosition) const
{
    if (viewportSize.width() < 180 || viewportSize.height() < 150) {
        return;
    }

    // The navigation controls occupy a small corner of the viewport. Render
    // that region at 2x resolution so thin colored strokes and circles retain
    // smooth edges even when the OpenGL surface has no multisample buffer.
    const QRect bounds(viewportSize.width() - 90,
                       0,
                       90,
                       std::min(viewportSize.height(), 230));
    QImage image(bounds.size() * int(blenderGizmoSupersampling),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(blenderGizmoSupersampling);

    QPainter gizmoPainter(&image);
    gizmoPainter.setRenderHint(QPainter::Antialiasing, true);
    gizmoPainter.setRenderHint(QPainter::TextAntialiasing, true);
    gizmoPainter.translate(-bounds.topLeft());
    drawBlenderNavigationGizmoContents(gizmoPainter, viewportSize, hoverPosition);
    gizmoPainter.end();

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(bounds.topLeft(), image);
    painter.restore();
}

void ViewportOverlay::drawBlenderNavigationGizmoContents(
    QPainter &painter,
    const QSize &viewportSize,
    const QPointF &hoverPosition) const
{
    if (viewportSize.width() < 180 || viewportSize.height() < 150) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QVector<BlenderAxisMarker> markers =
        blenderAxisMarkers(transform_, viewportSize);
    const BlenderNavigationHit hoverHit = blenderNavigationGizmoHitAt(
        hoverPosition, viewportSize);
    const QPointF center = blenderAxisCenter(viewportSize);

    QRegion axisStemClip(QRect(QPoint(0, 0), viewportSize));
    for (const BlenderAxisMarker &marker : markers) {
        if (marker.positive) {
            continue;
        }
        const qreal radius = blenderAxisMarkerRadius - 1.5;
        const QRectF markerInterior(marker.screenPosition.x() - radius,
                                    marker.screenPosition.y() - radius,
                                    radius * 2.0,
                                    radius * 2.0);
        axisStemClip -= QRegion(markerInterior.toAlignedRect(), QRegion::Ellipse);
    }

    painter.save();
    painter.setClipRegion(axisStemClip, Qt::IntersectClip);
    for (int axis = 0; axis < 3; ++axis) {
        const BlenderAxisMarker *positive = nullptr;
        const BlenderAxisMarker *negative = nullptr;
        for (const BlenderAxisMarker &marker : markers) {
            if (marker.axis == axis) {
                (marker.positive ? positive : negative) = &marker;
            }
        }
        if (positive == nullptr || negative == nullptr) {
            continue;
        }
        QColor color = blenderAxisColor(axis);
        const bool axisHovered = hoverHit.action == BlenderNavigationAction::Axis &&
            blenderDirectionDot(hoverHit.direction, positive->direction) > 0.99999;
        const int positiveAlpha = positive->depth >= 0.0 ? 235 : 195;
        const int negativeAlpha = negative->depth >= 0.0 ? 235 : 195;
        color.setAlpha(axisHovered ? 255 : positiveAlpha);
        painter.setPen(QPen(color, axisHovered ? 1.7 : 1.35, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center, positive->screenPosition);
        color.setAlpha(axisHovered ? 255 : negativeAlpha);
        painter.setPen(QPen(color, axisHovered ? 1.7 : 1.35, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center, negative->screenPosition);
    }
    painter.restore();

    for (const BlenderAxisMarker &marker : markers) {
        const QColor axisColor = blenderAxisColor(marker.axis);
        const bool hovered = hoverHit.action == BlenderNavigationAction::Axis &&
            blenderDirectionDot(hoverHit.direction, marker.direction) > 0.99999;
        const bool positiveAxis = marker.positive;
        const qreal radius = hovered ? blenderAxisMarkerRadius + 1.5
                                     : (positiveAxis ? blenderAxisMarkerRadius
                                                     : blenderAxisMarkerRadius - 1.5);
        painter.setPen(QPen(axisColor,
                            hovered ? 1.8 : 1.2,
                            Qt::SolidLine,
                            Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(positiveAxis ? QBrush(axisColor) : QBrush(Qt::NoBrush));
        painter.drawEllipse(marker.screenPosition, radius, radius);
        if (positiveAxis) {
            painter.setPen(QColor(37, 37, 37));
            painter.setFont(QFont(QStringLiteral("Sans"), 8, QFont::Bold));
            painter.drawText(QRectF(marker.screenPosition.x() - radius,
                                    marker.screenPosition.y() - radius,
                                    radius * 2.0,
                                    radius * 2.0),
                             Qt::AlignCenter,
                             blenderAxisName(marker.axis));
        }
    }

    drawBlenderNavigationButton(painter,
                                BlenderNavigationAction::Zoom,
                                viewportSize,
                                hoverPosition,
                                transform_.isPerspectiveEnabled());
    drawBlenderNavigationButton(painter,
                                BlenderNavigationAction::Pan,
                                viewportSize,
                                hoverPosition,
                                transform_.isPerspectiveEnabled());
    drawBlenderNavigationButton(painter,
                                BlenderNavigationAction::Camera,
                                viewportSize,
                                hoverPosition,
                                transform_.isPerspectiveEnabled());
    drawBlenderNavigationButton(painter,
                                BlenderNavigationAction::Projection,
                                viewportSize,
                                hoverPosition,
                                transform_.isPerspectiveEnabled());
    painter.restore();
}

BlenderNavigationHit ViewportOverlay::blenderNavigationGizmoHitAt(
    const QPointF &screenPosition,
    const QSize &viewportSize) const
{
    if (viewportSize.width() < 180 || viewportSize.height() < 150) {
        return {};
    }

    for (const BlenderNavigationAction action : {
             BlenderNavigationAction::Zoom,
             BlenderNavigationAction::Pan,
             BlenderNavigationAction::Camera,
             BlenderNavigationAction::Projection}) {
        if (blenderMiniButtonRect(action, viewportSize).contains(screenPosition)) {
            return {action, {}};
        }
    }

    const QVector<BlenderAxisMarker> markers =
        blenderAxisMarkers(transform_, viewportSize);
    qreal closestDistance = std::numeric_limits<qreal>::infinity();
    const BlenderAxisMarker *closestMarker = nullptr;
    for (const BlenderAxisMarker &marker : markers) {
        const qreal distance = QLineF(marker.screenPosition, screenPosition).length();
        if (distance > 11.5) {
            continue;
        }
        if (distance < closestDistance - 0.25 ||
            (std::abs(distance - closestDistance) <= 0.25 &&
             (closestMarker == nullptr || marker.depth > closestMarker->depth))) {
            closestDistance = distance;
            closestMarker = &marker;
        }
    }
    if (closestMarker != nullptr) {
        return {BlenderNavigationAction::Axis, closestMarker->direction};
    }

    const QPointF center = blenderAxisCenter(viewportSize);
    if (QLineF(center, screenPosition).length() <= blenderAxisRadius + 10.0) {
        return {BlenderNavigationAction::Orbit, {}};
    }
    return {};
}

} // namespace classiCAD
