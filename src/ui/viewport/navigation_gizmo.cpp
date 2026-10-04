/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "navigation_gizmo.h"

#include <QFont>
#include <QImage>
#include <QLineF>
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

ViewportNavigationGizmo::ViewportNavigationGizmo(
    const ViewportTransform &transform)
    : transform_(transform)
{
}

void ViewportNavigationGizmo::draw(
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
    drawContents(gizmoPainter, viewportSize, hoverPosition);
    gizmoPainter.end();

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(bounds.topLeft(), image);
    painter.restore();
}

void ViewportNavigationGizmo::drawContents(
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
    const BlenderNavigationHit hoverHit = hitAt(hoverPosition, viewportSize);
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

BlenderNavigationHit ViewportNavigationGizmo::hitAt(
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
