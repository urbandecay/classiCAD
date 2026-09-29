#include "viewport_overlay.h"

#include "core/geometry/circle_construction.h"

#include <QFont>
#include <QLineF>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace classiCAD {
namespace {

struct ViewCubeFace {
    QString label;
    Point3D normal;
    std::array<Point3D, 4> corners;
    QColor color;
};

struct ProjectedCubeTile {
    Point3D direction;
    QString label;
    QPolygonF polygon;
    QColor color;
    qreal depth = 0.0;
    bool mainFace = false;
};

const std::array<ViewCubeFace, 6> &viewCubeFaces()
{
    static const std::array<ViewCubeFace, 6> faces = {{
        {QStringLiteral("TOP"),
         {0.0, 0.0, 1.0},
         {{{-1.0, -1.0, 1.0}, {1.0, -1.0, 1.0},
           {1.0, 1.0, 1.0}, {-1.0, 1.0, 1.0}}},
         QColor(QStringLiteral("#64696f"))},
        {QStringLiteral("BOTTOM"),
         {0.0, 0.0, -1.0},
         {{{-1.0, 1.0, -1.0}, {1.0, 1.0, -1.0},
           {1.0, -1.0, -1.0}, {-1.0, -1.0, -1.0}}},
         QColor(QStringLiteral("#3d4146"))},
        {QStringLiteral("FRONT"),
         {0.0, -1.0, 0.0},
         {{{-1.0, -1.0, -1.0}, {-1.0, -1.0, 1.0},
           {1.0, -1.0, 1.0}, {1.0, -1.0, -1.0}}},
         QColor(QStringLiteral("#555a60"))},
        {QStringLiteral("BACK"),
         {0.0, 1.0, 0.0},
         {{{1.0, 1.0, -1.0}, {1.0, 1.0, 1.0},
           {-1.0, 1.0, 1.0}, {-1.0, 1.0, -1.0}}},
         QColor(QStringLiteral("#44494f"))},
        {QStringLiteral("RIGHT"),
         {1.0, 0.0, 0.0},
         {{{1.0, -1.0, -1.0}, {1.0, -1.0, 1.0},
           {1.0, 1.0, 1.0}, {1.0, 1.0, -1.0}}},
         QColor(QStringLiteral("#4b5056"))},
        {QStringLiteral("LEFT"),
         {-1.0, 0.0, 0.0},
         {{{-1.0, 1.0, -1.0}, {-1.0, 1.0, 1.0},
           {-1.0, -1.0, 1.0}, {-1.0, -1.0, -1.0}}},
         QColor(QStringLiteral("#41464c"))},
    }};
    return faces;
}

QPointF viewCubeCenter(const QSize &viewportSize, const QPointF &placementOffset)
{
    return {viewportSize.width() - 76.0 + placementOffset.x(),
            70.0 + placementOffset.y()};
}

QPointF projectedCubePoint(const ViewportTransform &transform,
                           const Point3D &point,
                           const QPointF &center,
                           qreal scale)
{
    const ViewportDirectionProjection projected =
        transform.worldDirectionToView(point);
    return {center.x() + projected.horizontal * scale,
            center.y() - projected.vertical * scale};
}

Point3D addDirection(const Point3D &first, const Point3D &second)
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Point3D scaledDirection(const Point3D &direction, qreal factor)
{
    return {direction.x * factor, direction.y * factor, direction.z * factor};
}

Point3D unitDirection(const Point3D &direction)
{
    const qreal length = std::sqrt(direction.x * direction.x +
                                   direction.y * direction.y +
                                   direction.z * direction.z);
    if (length <= 1.0e-12) {
        return {};
    }
    return scaledDirection(direction, 1.0 / length);
}

Point3D interpolate(const Point3D &first, const Point3D &second, qreal amount)
{
    return {first.x + (second.x - first.x) * amount,
            first.y + (second.y - first.y) * amount,
            first.z + (second.z - first.z) * amount};
}

Point3D facePoint(const ViewCubeFace &face, qreal u, qreal v)
{
    const Point3D left = interpolate(face.corners[0], face.corners[3], v);
    const Point3D right = interpolate(face.corners[1], face.corners[2], v);
    return interpolate(left, right, u);
}

QVector<ProjectedCubeTile> projectedCubeTiles(const ViewportTransform &transform,
                                               const QSize &viewportSize,
                                               const QPointF &placementOffset)
{
    constexpr qreal cubeScale = 25.0;
    const QPointF center = viewCubeCenter(viewportSize, placementOffset);
    QVector<ProjectedCubeTile> projected;
    for (const ViewCubeFace &face : viewCubeFaces()) {
        const ViewportDirectionProjection facing =
            transform.worldDirectionToView(face.normal);
        if (facing.towardCamera <= 1.0e-6) {
            continue;
        }
        const Point3D uAxis = unitDirection({face.corners[1].x - face.corners[0].x,
                                             face.corners[1].y - face.corners[0].y,
                                             face.corners[1].z - face.corners[0].z});
        const Point3D vAxis = unitDirection({face.corners[3].x - face.corners[0].x,
                                             face.corners[3].y - face.corners[0].y,
                                             face.corners[3].z - face.corners[0].z});
        constexpr std::array<qreal, 4> tileBounds = {0.0, 0.22, 0.78, 1.0};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const qreal u0 = tileBounds[column];
                const qreal u1 = tileBounds[column + 1];
                const qreal v0 = tileBounds[row];
                const qreal v1 = tileBounds[row + 1];
                ProjectedCubeTile tile;
                tile.direction = face.normal;
                if (column == 0) {
                    tile.direction = addDirection(tile.direction, scaledDirection(uAxis, -1.0));
                } else if (column == 2) {
                    tile.direction = addDirection(tile.direction, uAxis);
                }
                if (row == 0) {
                    tile.direction = addDirection(tile.direction, scaledDirection(vAxis, -1.0));
                } else if (row == 2) {
                    tile.direction = addDirection(tile.direction, vAxis);
                }
                tile.direction = unitDirection(tile.direction);
                tile.label = face.label;
                tile.color = face.color;
                tile.depth = facing.towardCamera;
                tile.mainFace = row == 1 && column == 1;
                tile.polygon << projectedCubePoint(transform, facePoint(face, u0, v0), center, cubeScale)
                              << projectedCubePoint(transform, facePoint(face, u1, v0), center, cubeScale)
                              << projectedCubePoint(transform, facePoint(face, u1, v1), center, cubeScale)
                              << projectedCubePoint(transform, facePoint(face, u0, v1), center, cubeScale);
                projected.append(tile);
            }
        }
    }
    std::sort(projected.begin(), projected.end(), [](const ProjectedCubeTile &first,
                                                     const ProjectedCubeTile &second) {
        return first.depth < second.depth;
    });
    return projected;
}

QRectF navigationPanelRect(const QSize &viewportSize, const QPointF &placementOffset)
{
    return QRectF(viewportSize.width() - 151.0 + placementOffset.x(),
                  7.0 + placementOffset.y(),
                  143.0,
                  127.0);
}

bool withinButton(const QPointF &position, const QPointF &center, qreal radius = 9.0)
{
    return QLineF(position, center).length() <= radius;
}

QPointF axisScreenDirection(const ViewportDirectionProjection &direction)
{
    return {direction.horizontal, -direction.vertical};
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
                                        ToolId tool,
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

    if (definitionValid) {
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
                                         const QSize &viewportSize) const
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
    if (candidatePoints.size() >= requiredPointCount) {
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
                                           const QSize &viewportSize) const
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

        painter.setPen(QPen(rectangleColor, 2.0));
        painter.setBrush(Qt::NoBrush);
        for (int index = 0; index < screenPoints.size(); ++index) {
            painter.drawLine(screenPoints[index],
                             screenPoints[(index + 1) % screenPoints.size()]);
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
                                         const QSize &viewportSize) const
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
    } else if (vertices.size() >= 3) {
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
    } else if (activeTool != Tool::Select) {
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
    } else if (!joinActive) {
        painter.setPen(QColor(QStringLiteral("#777777")));
        painter.drawText(18,
                         viewportSize.height() - 18,
                         QStringLiteral("Shift-click: add/remove  •  Box select  •  G: Grab  •  Shift+D: copy in place  •  B: base point  •  X/Y: axis lock"));
    }
}

void ViewportOverlay::drawNavigationGizmo(QPainter &painter,
                                          const QSize &viewportSize,
                                          const QPointF &hoverPosition,
                                          const QPointF &placementOffset) const
{
    if (viewportSize.width() < 180 || viewportSize.height() < 150) {
        return;
    }

    painter.save();
    const QPointF cubeCenter = viewCubeCenter(viewportSize, placementOffset);
    const QRectF panel = navigationPanelRect(viewportSize, placementOffset);
    painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
    painter.setBrush(QColor(22, 22, 22, 110));
    painter.drawRoundedRect(panel, 6.0, 6.0);

    const NavigationGizmoHit hoveredHit = navigationGizmoHitAt(
        hoverPosition, viewportSize, placementOffset);
    const QVector<ProjectedCubeTile> tiles =
        projectedCubeTiles(transform_, viewportSize, placementOffset);
    for (const ProjectedCubeTile &tile : tiles) {
        const bool hovered = hoveredHit.action == NavigationGizmoAction::SetViewDirection &&
            std::abs(tile.direction.x * hoveredHit.direction.x +
                     tile.direction.y * hoveredHit.direction.y +
                     tile.direction.z * hoveredHit.direction.z - 1.0) <= 1.0e-5;
        painter.setPen(QPen(hovered ? QColor(QStringLiteral("#e9b16b"))
                                    : QColor(20, 20, 20, 115),
                            hovered ? 1.5 : 0.8));
        QColor tileColor = tile.color;
        if (!tile.mainFace) {
            tileColor = tileColor.darker(112);
        }
        if (hovered) {
            tileColor = QColor(QStringLiteral("#a66d3c"));
        }
        painter.setBrush(tileColor);
        painter.drawPolygon(tile.polygon);
        if (tile.mainFace) {
            painter.setPen(QColor(QStringLiteral("#e0e2e4")));
            painter.setFont(QFont(QStringLiteral("Sans"), 7, QFont::DemiBold));
            painter.drawText(tile.polygon.boundingRect(), Qt::AlignCenter, tile.label);
        }
    }

    const auto drawArrow = [&painter](const QPointF &center,
                                     const QPointF &direction,
                                     bool hovered) {
        const QPointF perpendicular(-direction.y(), direction.x());
        QPolygonF triangle;
        triangle << center + direction * 6.5
                 << center - direction * 4.5 + perpendicular * 4.0
                 << center - direction * 4.5 - perpendicular * 4.0;
        painter.setPen(QPen(QColor(10, 10, 10, 130), 0.8));
        painter.setBrush(hovered ? QColor(QStringLiteral("#e9b16b"))
                                 : QColor(QStringLiteral("#aeb3b8")));
        painter.drawPolygon(triangle);
    };
    drawArrow(cubeCenter + QPointF(-51.0, 0.0), QPointF(-1.0, 0.0),
              hoveredHit.action == NavigationGizmoAction::Orbit &&
                  hoveredHit.arrowDirection == QPointF(-1.0, 0.0));
    drawArrow(cubeCenter + QPointF(51.0, 0.0), QPointF(1.0, 0.0),
              hoveredHit.action == NavigationGizmoAction::Orbit &&
                  hoveredHit.arrowDirection == QPointF(1.0, 0.0));
    drawArrow(cubeCenter + QPointF(0.0, -51.0), QPointF(0.0, -1.0),
              hoveredHit.action == NavigationGizmoAction::Orbit &&
                  hoveredHit.arrowDirection == QPointF(0.0, -1.0));
    drawArrow(cubeCenter + QPointF(0.0, 51.0), QPointF(0.0, 1.0),
              hoveredHit.action == NavigationGizmoAction::Orbit &&
                  hoveredHit.arrowDirection == QPointF(0.0, 1.0));

    const auto drawButton = [&painter](const QPointF &center, bool hovered) {
        painter.setPen(QPen(QColor(0, 0, 0, 125), 0.8));
        painter.setBrush(hovered ? QColor(QStringLiteral("#6f747a"))
                                 : QColor(QStringLiteral("#33373b")));
        painter.drawEllipse(center, 8.5, 8.5);
    };
    const QPointF homeCenter = cubeCenter + QPointF(-55.0, -53.0);
    const QPointF reverseCenter = cubeCenter + QPointF(55.0, -53.0);
    const QPointF menuCenter = cubeCenter + QPointF(55.0, 53.0);
    const QPointF rollLeftCenter = cubeCenter + QPointF(-30.0, -53.0);
    const QPointF rollRightCenter = cubeCenter + QPointF(30.0, -53.0);
    const auto isHovered = [&hoveredHit](NavigationGizmoAction action, qreal amount = 0.0) {
        return hoveredHit.action == action &&
               std::abs(hoveredHit.amount - amount) <= 1.0e-6;
    };

    drawButton(homeCenter, isHovered(NavigationGizmoAction::Home));
    QPolygonF house;
    house << homeCenter + QPointF(-4.2, 0.0)
          << homeCenter + QPointF(0.0, -3.8)
          << homeCenter + QPointF(4.2, 0.0)
          << homeCenter + QPointF(3.2, 0.0)
          << homeCenter + QPointF(3.2, 3.4)
          << homeCenter + QPointF(-3.2, 3.4)
          << homeCenter + QPointF(-3.2, 0.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#d8dbde")));
    painter.drawPolygon(house);

    drawButton(reverseCenter, isHovered(NavigationGizmoAction::Reverse));
    painter.setPen(QPen(QColor(QStringLiteral("#d8dbde")), 1.5, Qt::SolidLine,
                        Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(reverseCenter.x() - 4.5, reverseCenter.y() - 4.5,
                           9.0, 9.0), 35 * 16, 285 * 16);
    QPolygonF reverseArrow;
    reverseArrow << reverseCenter + QPointF(4.7, -1.2)
                 << reverseCenter + QPointF(1.5, -1.2)
                 << reverseCenter + QPointF(4.1, -4.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#d8dbde")));
    painter.drawPolygon(reverseArrow);

    drawButton(rollLeftCenter, isHovered(NavigationGizmoAction::Roll, -45.0));
    drawButton(rollRightCenter, isHovered(NavigationGizmoAction::Roll, 45.0));
    const auto drawRollArrow = [&painter](const QPointF &center, bool clockwise) {
        const QRectF bounds(center.x() - 4.3, center.y() - 4.3, 8.6, 8.6);
        painter.setPen(QPen(QColor(QStringLiteral("#c9cdd1")), 1.3,
                            Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(bounds, clockwise ? -15 * 16 : 195 * 16, 235 * 16);
        const qreal sign = clockwise ? 1.0 : -1.0;
        const QPointF tip = center + QPointF(sign * 4.0, -1.4);
        QPolygonF head;
        head << tip << tip + QPointF(-sign * 3.0, -0.2)
             << tip + QPointF(-sign * 0.8, 2.8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#c9cdd1")));
        painter.drawPolygon(head);
    };
    drawRollArrow(rollLeftCenter, false);
    drawRollArrow(rollRightCenter, true);

    drawButton(menuCenter, isHovered(NavigationGizmoAction::Menu));
    const QPointF miniCubeTop = menuCenter + QPointF(0.0, -4.1);
    const QPointF miniCubeLeft = menuCenter + QPointF(-3.7, -2.0);
    const QPointF miniCubeRight = menuCenter + QPointF(3.7, -2.0);
    const QPointF miniCubeCenter = menuCenter + QPointF(0.0, 0.1);
    const QPointF miniCubeBottomLeft = menuCenter + QPointF(-3.7, 2.2);
    const QPointF miniCubeBottom = menuCenter + QPointF(0.0, 4.2);
    const QPointF miniCubeBottomRight = menuCenter + QPointF(3.7, 2.2);
    painter.setPen(QPen(QColor(QStringLiteral("#d8dbde")), 0.9));
    painter.setBrush(QColor(QStringLiteral("#777d83")));
    QPolygonF miniCubeLeftFace;
    miniCubeLeftFace << miniCubeLeft << miniCubeCenter
                     << miniCubeBottom << miniCubeBottomLeft;
    painter.drawPolygon(miniCubeLeftFace);
    painter.setBrush(QColor(QStringLiteral("#555b61")));
    QPolygonF miniCubeRightFace;
    miniCubeRightFace << miniCubeCenter << miniCubeRight
                      << miniCubeBottomRight << miniCubeBottom;
    painter.drawPolygon(miniCubeRightFace);
    painter.setBrush(QColor(QStringLiteral("#a0a5aa")));
    QPolygonF miniCubeTopFace;
    miniCubeTopFace << miniCubeTop << miniCubeRight
                    << miniCubeCenter << miniCubeLeft;
    painter.drawPolygon(miniCubeTopFace);

    // The compact lower-right triad follows the camera, as in Blender. An
    // axis aimed directly toward/away from the camera is shown as a dot.
    const QPointF triadOrigin(viewportSize.width() - 35.0,
                              viewportSize.height() - 32.0);
    struct AxisMark {
        Point3D direction;
        QString label;
        QColor color;
    };
    const std::array<AxisMark, 3> axes = {{
        {{1.0, 0.0, 0.0}, QStringLiteral("X"), QColor(QStringLiteral("#df5260"))},
        {{0.0, 1.0, 0.0}, QStringLiteral("Y"), QColor(QStringLiteral("#86c84a"))},
        {{0.0, 0.0, 1.0}, QStringLiteral("Z"), QColor(QStringLiteral("#438ce0"))},
    }};
    painter.setFont(QFont(QStringLiteral("Sans"), 8, QFont::Bold));
    for (const AxisMark &axis : axes) {
        const ViewportDirectionProjection projected =
            transform_.worldDirectionToView(axis.direction);
        QPointF direction = axisScreenDirection(projected);
        const qreal length = std::hypot(direction.x(), direction.y());
        if (length <= 0.08) {
            painter.setPen(QPen(QColor(QStringLiteral("#202020")), 1.0));
            painter.setBrush(axis.color);
            painter.drawEllipse(triadOrigin, 3.2, 3.2);
            painter.setPen(axis.color);
            painter.drawText(QRectF(triadOrigin.x() - 10.0,
                                    triadOrigin.y() - 16.0,
                                    20.0,
                                    12.0),
                             Qt::AlignCenter,
                             axis.label);
            continue;
        }

        direction /= length;
        const QPointF tip = triadOrigin + direction * 22.0;
        const QPointF perpendicular(-direction.y(), direction.x());
        painter.setPen(QPen(QColor(QStringLiteral("#1a1a1a")), 4.0,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(triadOrigin, tip);
        painter.setPen(QPen(axis.color, 2.2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(triadOrigin, tip);
        QPolygonF arrowHead;
        arrowHead << tip
                  << tip - direction * 6.5 + perpendicular * 3.2
                  << tip - direction * 6.5 - perpendicular * 3.2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(axis.color);
        painter.drawPolygon(arrowHead);
        painter.setPen(axis.color);
        const QPointF labelPosition = tip + direction * 8.0;
        painter.drawText(QRectF(labelPosition.x() - 6.0,
                                labelPosition.y() - 6.0,
                                12.0,
                                12.0),
                         Qt::AlignCenter,
                         axis.label);
    }
    painter.restore();
}

NavigationGizmoHit ViewportOverlay::navigationGizmoHitAt(
    const QPointF &screenPosition,
    const QSize &viewportSize,
    const QPointF &placementOffset) const
{
    if (!navigationCubeContains(screenPosition, viewportSize, placementOffset)) {
        return {};
    }

    const QPointF center = viewCubeCenter(viewportSize, placementOffset);
    const std::array<std::pair<QPointF, NavigationGizmoHit>, 9> buttons = {{
        {center + QPointF(-55.0, -53.0), {NavigationGizmoAction::Home, {}, 0.0, {}}},
        {center + QPointF(55.0, -53.0), {NavigationGizmoAction::Reverse, {}, 0.0, {}}},
        {center + QPointF(55.0, 53.0), {NavigationGizmoAction::Menu, {}, 0.0, {}}},
        {center + QPointF(-51.0, 0.0),
         {NavigationGizmoAction::Orbit, {}, 45.0, QPointF(-1.0, 0.0)}},
        {center + QPointF(51.0, 0.0),
         {NavigationGizmoAction::Orbit, {}, 45.0, QPointF(1.0, 0.0)}},
        {center + QPointF(0.0, -51.0),
         {NavigationGizmoAction::Orbit, {}, -45.0, QPointF(0.0, -1.0)}},
        {center + QPointF(0.0, 51.0),
         {NavigationGizmoAction::Orbit, {}, -45.0, QPointF(0.0, 1.0)}},
        {center + QPointF(-30.0, -53.0), {NavigationGizmoAction::Roll, {}, -45.0, {}}},
        {center + QPointF(30.0, -53.0), {NavigationGizmoAction::Roll, {}, 45.0, {}}},
    }};
    for (const auto &button : buttons) {
        if (withinButton(screenPosition, button.first)) {
            return button.second;
        }
    }

    const QVector<ProjectedCubeTile> tiles =
        projectedCubeTiles(transform_, viewportSize, placementOffset);
    for (auto tile = tiles.crbegin(); tile != tiles.crend(); ++tile) {
        if (tile->polygon.containsPoint(screenPosition, Qt::OddEvenFill)) {
            return {NavigationGizmoAction::SetViewDirection,
                    tile->direction,
                    0.0,
                    {}};
        }
    }
    return {};
}

bool ViewportOverlay::navigationCubeContains(const QPointF &screenPosition,
                                             const QSize &viewportSize,
                                             const QPointF &placementOffset) const
{
    if (viewportSize.width() < 180 || viewportSize.height() < 150) {
        return false;
    }
    return navigationPanelRect(viewportSize, placementOffset).contains(screenPosition);
}

bool ViewportOverlay::navigationCubeSurfaceContains(
    const QPointF &screenPosition,
    const QSize &viewportSize,
    const QPointF &placementOffset) const
{
    if (!navigationCubeContains(screenPosition, viewportSize, placementOffset)) {
        return false;
    }
    if (navigationGizmoHitAt(screenPosition, viewportSize, placementOffset).action !=
        NavigationGizmoAction::SetViewDirection) {
        return false;
    }
    return true;
}

} // namespace classiCAD
