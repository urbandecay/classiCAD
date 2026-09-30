#include "dimension_layout.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {
namespace {

constexpr qreal kEpsilon = 1.0e-9;
constexpr qreal kPi = 3.14159265358979323846;

qreal vectorLength(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

QPointF normalized(const QPointF &vector)
{
    const qreal length = vectorLength(vector);
    return length > kEpsilon ? vector / length : QPointF{};
}

QPolygonF makeArrowHead(const QPointF &tip,
                        const QPointF &towardInterior,
                        qreal length = 9.0,
                        qreal halfWidth = 3.6)
{
    const QPointF direction = normalized(towardInterior);
    if (direction.isNull()) {
        return {};
    }
    const QPointF base = tip + direction * length;
    const QPointF side(-direction.y() * halfWidth, direction.x() * halfWidth);
    return {tip, base + side, base - side};
}

qreal labelWidth(const QString &label, DimensionFontStyle fontStyle)
{
    // Keep rendering and hit-testing independent of platform font metrics,
    // while leaving enough room for the wider hand-lettered architectural face.
    const qreal characterWidth = fontStyle == DimensionFontStyle::Architectural
                                     ? 7.8
                                     : 7.2;
    return std::max<qreal>(18.0, label.size() * characterWidth + 8.0);
}

void setLabel(DimensionScreenLayout *layout,
              const QString &text,
              const QPointF &center,
              DimensionFontStyle fontStyle)
{
    if (layout == nullptr) {
        return;
    }
    layout->label = text;
    layout->labelCenter = center;
    const qreal width = labelWidth(text, fontStyle);
    layout->labelBounds = QRectF(center.x() - width * 0.5,
                                 center.y() - 10.0,
                                 width,
                                 20.0);
}

void addLinearDimension(const Shape &shape,
                        const ViewportTransform &transform,
                        const QSize &viewportSize,
                        DimensionFontStyle fontStyle,
                        DimensionScreenLayout *layout)
{
    if (shape.points.size() < 3 || layout == nullptr) {
        return;
    }

    const QPointF first = shape.points[0];
    const QPointF second = shape.points[1];
    const QPointF delta = second - first;
    const qreal length = vectorLength(delta);
    if (length <= kEpsilon) {
        return;
    }

    const QPointF direction = delta / length;
    const QPointF normal(-direction.y(), direction.x());
    const QPointF midpoint = (first + second) * 0.5;
    qreal offset = QPointF::dotProduct(shape.points[2] - midpoint, normal);
    if (std::abs(offset) <= kEpsilon) {
        offset = std::max(
            length * 0.25,
            2.0 / std::max(transform.viewScalePixelsPerWorldUnit(viewportSize),
                            1.0e-6));
    }

    const QPointF dimensionFirst = first + normal * offset;
    const QPointF dimensionSecond = second + normal * offset;
    const QPointF firstScreen = transform.worldToScreen(first, viewportSize);
    const QPointF secondScreen = transform.worldToScreen(second, viewportSize);
    const QPointF dimensionFirstScreen = transform.worldToScreen(dimensionFirst,
                                                                 viewportSize);
    const QPointF dimensionSecondScreen = transform.worldToScreen(dimensionSecond,
                                                                  viewportSize);
    const QPointF offsetDirection = normalized(dimensionFirstScreen - firstScreen);
    if (offsetDirection.isNull()) {
        return;
    }

    layout->lines.append(QLineF(firstScreen + offsetDirection * 4.0,
                                dimensionFirstScreen + offsetDirection * 3.0));
    layout->lines.append(QLineF(secondScreen + offsetDirection * 4.0,
                                dimensionSecondScreen + offsetDirection * 3.0));

    const QPointF dimensionVector = dimensionSecondScreen - dimensionFirstScreen;
    const qreal screenLength = vectorLength(dimensionVector);
    if (screenLength <= kEpsilon) {
        return;
    }
    const QPointF screenDirection = dimensionVector / screenLength;
    const QPointF center = (dimensionFirstScreen + dimensionSecondScreen) * 0.5;
    const QString label = QStringLiteral("%1 mm")
                              .arg(length, 0, 'f', length < 1.0 ? 3 : 2);
    setLabel(layout, label, center, fontStyle);

    const qreal arrowLength = 9.0;
    const bool arrowsInside = screenLength >= arrowLength * 2.5;
    layout->arrowHeads.append(makeArrowHead(
        dimensionFirstScreen,
        arrowsInside ? screenDirection : -screenDirection));
    layout->arrowHeads.append(makeArrowHead(
        dimensionSecondScreen,
        arrowsInside ? -screenDirection : screenDirection));

    const qreal labelGap = layout->labelBounds.width() + 6.0;
    if (screenLength > labelGap + arrowLength * 2.0) {
        const QPointF halfGap = screenDirection * (labelGap * 0.5);
        layout->lines.append(QLineF(dimensionFirstScreen, center - halfGap));
        layout->lines.append(QLineF(center + halfGap, dimensionSecondScreen));
    } else {
        layout->lines.append(QLineF(dimensionFirstScreen, dimensionSecondScreen));
    }
    layout->valid = true;
}

void addAngularDimension(const Shape &shape,
                         const ViewportTransform &transform,
                         const QSize &viewportSize,
                         DimensionFontStyle fontStyle,
                         DimensionScreenLayout *layout)
{
    if (shape.points.size() < 3 || layout == nullptr) {
        return;
    }

    const QPointF vertex = shape.points[0];
    const QPointF firstRay = shape.points[1] - vertex;
    const QPointF secondRay = shape.points[2] - vertex;
    const qreal firstLength = vectorLength(firstRay);
    const qreal secondLength = vectorLength(secondRay);
    if (firstLength <= kEpsilon || secondLength <= kEpsilon) {
        return;
    }

    const qreal dot = QPointF::dotProduct(firstRay, secondRay);
    const qreal cross = firstRay.x() * secondRay.y() - firstRay.y() * secondRay.x();
    const qreal sweep = std::atan2(cross, dot);
    if (std::abs(sweep) <= 1.0e-8) {
        return;
    }

    const QPointF firstDirection = firstRay / firstLength;
    const QPointF vertexScreen = transform.worldToScreen(vertex, viewportSize);
    const QPointF firstScreen = transform.worldToScreen(shape.points[1], viewportSize);
    const QPointF secondScreen = transform.worldToScreen(shape.points[2], viewportSize);
    const QPointF firstScreenDirection = normalized(firstScreen - vertexScreen);
    const QPointF secondScreenDirection = normalized(secondScreen - vertexScreen);
    if (firstScreenDirection.isNull() || secondScreenDirection.isNull()) {
        return;
    }

    const qreal screenRadius = vectorLength(
        transform.worldToScreen(vertex + firstDirection * firstLength, viewportSize) -
        vertexScreen);
    const int arcSteps = std::clamp(
        static_cast<int>(std::ceil(std::abs(sweep) * screenRadius / 10.0)),
        8,
        128);
    QVector<QPointF> arcPoints;
    arcPoints.reserve(arcSteps + 1);
    for (int step = 0; step <= arcSteps; ++step) {
        const qreal angle = sweep * static_cast<qreal>(step) / arcSteps;
        const qreal cosine = std::cos(angle);
        const qreal sine = std::sin(angle);
        const QPointF rotated(firstDirection.x() * cosine - firstDirection.y() * sine,
                              firstDirection.x() * sine + firstDirection.y() * cosine);
        arcPoints.append(transform.worldToScreen(vertex + rotated * firstLength,
                                                 viewportSize));
    }

    const QPointF arcStart = arcPoints.first();
    const QPointF arcEnd = arcPoints.last();
    layout->lines.append(QLineF(vertexScreen + firstScreenDirection * 4.0,
                                arcStart + firstScreenDirection * 3.0));
    layout->lines.append(QLineF(vertexScreen + secondScreenDirection * 4.0,
                                arcEnd + secondScreenDirection * 3.0));
    for (int index = 0; index + 1 < arcPoints.size(); ++index) {
        layout->lines.append(QLineF(arcPoints[index], arcPoints[index + 1]));
    }

    layout->arrowHeads.append(makeArrowHead(arcStart, arcPoints[1] - arcStart));
    layout->arrowHeads.append(makeArrowHead(arcEnd, arcPoints[arcPoints.size() - 2] - arcEnd));

    const QPointF arcMidpoint = arcPoints[arcSteps / 2];
    const QPointF radialDirection = normalized(arcMidpoint - vertexScreen);
    const qreal degrees = std::abs(sweep) * 180.0 / kPi;
    QString angleText = QString::number(degrees, 'f', 2);
    while (angleText.contains(QLatin1Char('.')) && angleText.endsWith(QLatin1Char('0'))) {
        angleText.chop(1);
    }
    if (angleText.endsWith(QLatin1Char('.'))) {
        angleText.chop(1);
    }
    setLabel(layout,
             angleText + QChar(0x00B0),
             arcMidpoint + radialDirection * 12.0,
             fontStyle);
    layout->valid = true;
}

qreal distanceToSegment(const QPointF &point, const QLineF &line)
{
    const QPointF direction = line.p2() - line.p1();
    const qreal lengthSquared = QPointF::dotProduct(direction, direction);
    if (lengthSquared <= kEpsilon) {
        return vectorLength(point - line.p1());
    }
    const qreal fraction = std::clamp(
        QPointF::dotProduct(point - line.p1(), direction) / lengthSquared,
        0.0,
        1.0);
    return vectorLength(point - (line.p1() + direction * fraction));
}

} // namespace

DimensionScreenLayout buildDimensionScreenLayout(const Shape &shape,
                                                 const ViewportTransform &transform,
                                                 const QSize &viewportSize,
                                                 DimensionFontStyle fontStyle)
{
    DimensionScreenLayout layout;
    if (shape.geometryType == GeometryType::LinearDimension) {
        addLinearDimension(shape, transform, viewportSize, fontStyle, &layout);
    } else if (shape.geometryType == GeometryType::AngularDimension) {
        addAngularDimension(shape, transform, viewportSize, fontStyle, &layout);
    }
    return layout;
}

qreal distanceToDimensionLayout(const QPointF &screenPosition,
                                const DimensionScreenLayout &layout)
{
    if (!layout.valid) {
        return 1.0e9;
    }
    if (layout.labelBounds.contains(screenPosition)) {
        return 0.0;
    }

    qreal distance = 1.0e9;
    for (const QLineF &line : layout.lines) {
        distance = std::min(distance, distanceToSegment(screenPosition, line));
    }
    for (const QPolygonF &arrowHead : layout.arrowHeads) {
        if (arrowHead.containsPoint(screenPosition, Qt::OddEvenFill)) {
            return 0.0;
        }
        for (int index = 0; index < arrowHead.size(); ++index) {
            distance = std::min(
                distance,
                distanceToSegment(screenPosition,
                                  QLineF(arrowHead[index],
                                         arrowHead[(index + 1) % arrowHead.size()])));
        }
    }
    return distance;
}

} // namespace classiCAD
