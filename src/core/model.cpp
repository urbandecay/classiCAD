#include "model.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>

namespace classiCAD {

HomogeneousControlPoint2D blendHomogeneousControlPoints(
    const HomogeneousControlPoint2D &first,
    const HomogeneousControlPoint2D &second,
    qreal secondFraction)
{
    const qreal firstFraction = 1.0 - secondFraction;
    return HomogeneousControlPoint2D{
        first.weightedPosition * firstFraction + second.weightedPosition * secondFraction,
        first.weight * firstFraction + second.weight * secondFraction};
}

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 1;
    curve.order = 2;
    curve.rational = false;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    // An open, clamped degree-1 curve represents the same connected
    // segments as a Rhino-style polyline while remaining NURBS data.
    const int pointCount = points.size();
    curve.knots.reserve(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        curve.knots.append(static_cast<double>(i));
    }

    return curve;
}

Shape::NurbsCurve2D makeBezierNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.controlPoints = points;
    curve.weights.fill(1.0, points.size());

    if (points.size() < 2) {
        return curve;
    }

    curve.degree = points.size() - 1;
    curve.order = curve.degree + 1;
    curve.rational = false;

    // A single Bezier span is a clamped NURBS with degree repeated at each
    // end. Store the Rhino/openNURBS knot array without the two redundant
    // outer entries.
    curve.knots.reserve(curve.controlPoints.size() + curve.degree - 1);
    for (int index = 0; index < curve.degree; ++index) {
        curve.knots.append(0.0);
    }
    for (int index = 0; index < curve.degree; ++index) {
        curve.knots.append(1.0);
    }

    return curve;
}

Shape::NurbsCurve2D makeCircleNurbs(const QVector<QPointF> &points)
{
    Shape::NurbsCurve2D curve;
    curve.dimension = 2;
    curve.degree = 2;
    curve.order = 3;
    curve.rational = true;

    if (points.size() < 2) {
        return curve;
    }

    constexpr qreal pi = 3.14159265358979323846;
    constexpr qreal halfPi = pi / 2.0;
    const QPointF center = points[0];
    const QPointF edge = points[1];
    const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
    if (radius <= 1e-9) {
        return curve;
    }

    const qreal startAngle = std::atan2(edge.y() - center.y(), edge.x() - center.x());
    const qreal middleWeight = std::cos(pi / 4.0);
    const qreal middleRadius = radius / middleWeight;

    curve.controlPoints.reserve(9);
    curve.weights.reserve(9);
    for (int span = 0; span < 4; ++span) {
        const qreal spanStart = startAngle + halfPi * span;
        const qreal spanEnd = spanStart + halfPi;
        const qreal spanMiddle = (spanStart + spanEnd) * 0.5;
        const auto circlePoint = [center, radius](qreal angle) {
            return QPointF(center.x() + radius * std::cos(angle),
                           center.y() + radius * std::sin(angle));
        };

        if (span == 0) {
            curve.controlPoints.append(circlePoint(spanStart));
            curve.weights.append(1.0);
        }
        curve.controlPoints.append(QPointF(
            center.x() + middleRadius * std::cos(spanMiddle),
            center.y() + middleRadius * std::sin(spanMiddle)));
        curve.weights.append(middleWeight);
        curve.controlPoints.append(circlePoint(spanEnd));
        curve.weights.append(1.0);
    }

    // This is the same reduced knot array used by Rhino's documented
    // degree-2 rational NURBS circle construction.
    curve.knots = {0.0,
                   0.0,
                   halfPi,
                   halfPi,
                   pi,
                   pi,
                   3.0 * halfPi,
                   3.0 * halfPi,
                   2.0 * pi,
                   2.0 * pi};
    return curve;
}

qreal crossProduct(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

bool segmentIntersection(const QPointF &a,
                         const QPointF &b,
                         const QPointF &c,
                         const QPointF &d,
                         QPointF *intersection)
{
    const QPointF firstDirection = b - a;
    const QPointF secondDirection = d - c;
    const qreal denominator = crossProduct(firstDirection, secondDirection);

    if (std::abs(denominator) < 1e-9) {
        return false;
    }

    const QPointF betweenStarts = c - a;
    const qreal firstParameter = crossProduct(betweenStarts, secondDirection) / denominator;
    const qreal secondParameter = crossProduct(betweenStarts, firstDirection) / denominator;
    constexpr qreal tolerance = 1e-9;

    if (firstParameter < -tolerance || firstParameter > 1.0 + tolerance ||
        secondParameter < -tolerance || secondParameter > 1.0 + tolerance) {
        return false;
    }

    if (intersection != nullptr) {
        *intersection = a + firstDirection * firstParameter;
    }
    return true;
}

QString pointText(const QPointF &point)
{
    return QStringLiteral("(%1, %2)")
        .arg(point.x(), 0, 'f', 3)
        .arg(point.y(), 0, 'f', 3);
}

QJsonObject pointToJson(const QPointF &point)
{
    QJsonObject object;
    object.insert(QStringLiteral("x"), point.x());
    object.insert(QStringLiteral("y"), point.y());
    return object;
}

bool pointFromJson(const QJsonValue &value, QPointF *point)
{
    if (point == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    const QJsonValue xValue = object.value(QStringLiteral("x"));
    const QJsonValue yValue = object.value(QStringLiteral("y"));
    if (!xValue.isDouble() || !yValue.isDouble()) {
        return false;
    }

    const qreal x = xValue.toDouble();
    const qreal y = yValue.toDouble();
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    *point = QPointF(x, y);
    return true;
}

QJsonArray pointsToJson(const QVector<QPointF> &points)
{
    QJsonArray array;
    for (const QPointF &point : points) {
        array.append(pointToJson(point));
    }
    return array;
}

bool pointsFromJson(const QJsonValue &value, QVector<QPointF> *points)
{
    if (points == nullptr || !value.isArray()) {
        return false;
    }

    QVector<QPointF> restoredPoints;
    const QJsonArray array = value.toArray();
    restoredPoints.reserve(array.size());
    for (const QJsonValue &pointValue : array) {
        QPointF point;
        if (!pointFromJson(pointValue, &point)) {
            return false;
        }
        restoredPoints.append(point);
    }

    *points = restoredPoints;
    return true;
}

QJsonObject nurbsToJson(const Shape::NurbsCurve2D &curve)
{
    QJsonObject object;
    object.insert(QStringLiteral("dimension"), curve.dimension);
    object.insert(QStringLiteral("degree"), curve.degree);
    object.insert(QStringLiteral("order"), curve.order);
    object.insert(QStringLiteral("rational"), curve.rational);
    object.insert(QStringLiteral("controlPoints"), pointsToJson(curve.controlPoints));

    QJsonArray weights;
    for (const double weight : curve.weights) {
        weights.append(weight);
    }
    object.insert(QStringLiteral("weights"), weights);

    QJsonArray knots;
    for (const double knot : curve.knots) {
        knots.append(knot);
    }
    object.insert(QStringLiteral("knots"), knots);
    return object;
}

bool nurbsFromJson(const QJsonValue &value, Shape::NurbsCurve2D *curve)
{
    if (curve == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    QVector<QPointF> controlPoints;
    if (!pointsFromJson(object.value(QStringLiteral("controlPoints")), &controlPoints)) {
        return false;
    }

    QVector<double> weights;
    const QJsonValue weightsValue = object.value(QStringLiteral("weights"));
    if (!weightsValue.isArray()) {
        return false;
    }
    for (const QJsonValue &weightValue : weightsValue.toArray()) {
        if (!weightValue.isDouble() || !std::isfinite(weightValue.toDouble())) {
            return false;
        }
        weights.append(weightValue.toDouble());
    }

    QVector<double> knots;
    const QJsonValue knotsValue = object.value(QStringLiteral("knots"));
    if (!knotsValue.isArray()) {
        return false;
    }
    for (const QJsonValue &knotValue : knotsValue.toArray()) {
        if (!knotValue.isDouble() || !std::isfinite(knotValue.toDouble())) {
            return false;
        }
        knots.append(knotValue.toDouble());
    }

    curve->dimension = object.value(QStringLiteral("dimension")).toInt(2);
    curve->degree = object.value(QStringLiteral("degree")).toInt(1);
    curve->order = object.value(QStringLiteral("order")).toInt(2);
    curve->rational = object.value(QStringLiteral("rational")).toBool(false);
    curve->controlPoints = controlPoints;
    curve->weights = weights;
    curve->knots = knots;
    // Point and rectangle records historically carried an empty placeholder
    // NURBS object. Preserve that representation, but reject any populated
    // curve that does not satisfy the shared core invariants.
    if (!curve->controlPoints.isEmpty() || !curve->weights.isEmpty() ||
        !curve->knots.isEmpty()) {
        if (!validateNurbsCurve(*curve)) {
            return false;
        }
    }
    return true;
}

QJsonObject shapeToJson(const Shape &shape)
{
    QJsonObject object;
    const int geometryTypeValue = legacyValueForGeometryType(shape.geometryType);
    // geometryType is the canonical field. Keep writing the old "tool"
    // integer as a compatibility bridge for version-1 session readers.
    object.insert(QStringLiteral("geometryType"), geometryTypeValue);
    object.insert(QStringLiteral("tool"), geometryTypeValue);
    object.insert(QStringLiteral("points"), pointsToJson(shape.points));
    object.insert(QStringLiteral("nurbs"), nurbsToJson(shape.nurbs));
    object.insert(QStringLiteral("arcMode"), static_cast<int>(shape.arcMode));
    object.insert(QStringLiteral("arcSweep"), shape.arcSweep);

    QJsonArray subdivisionParameters;
    for (const double parameter : shape.subdivisionParameters) {
        subdivisionParameters.append(parameter);
    }
    object.insert(QStringLiteral("subdivisionParameters"), subdivisionParameters);

    QJsonArray components;
    for (const Shape::NurbsCurve2D &component : shape.components) {
        components.append(nurbsToJson(component));
    }
    object.insert(QStringLiteral("components"), components);
    return object;
}

bool shapeFromJson(const QJsonValue &value, Shape *shape)
{
    if (shape == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    GeometryType geometryType = GeometryType::Invalid;
    const QJsonValue geometryTypeValue = object.value(QStringLiteral("geometryType"));
    if (!geometryTypeValue.isUndefined()) {
        if (!geometryTypeValue.isDouble() ||
            !geometryTypeFromLegacyValue(geometryTypeValue.toInt(), &geometryType)) {
            return false;
        }
    } else {
        const QJsonValue legacyToolValue = object.value(QStringLiteral("tool"));
        if (!legacyToolValue.isDouble() ||
            !geometryTypeFromLegacyValue(legacyToolValue.toInt(), &geometryType)) {
            return false;
        }
    }

    const int arcModeValue = object.value(QStringLiteral("arcMode")).toInt(-1);
    if (arcModeValue < static_cast<int>(ArcMode::OnePoint) ||
        arcModeValue > static_cast<int>(ArcMode::TwoPoint)) {
        return false;
    }

    QVector<QPointF> points;
    if (!pointsFromJson(object.value(QStringLiteral("points")), &points)) {
        return false;
    }

    Shape::NurbsCurve2D nurbs;
    if (!nurbsFromJson(object.value(QStringLiteral("nurbs")), &nurbs)) {
        return false;
    }

    const QJsonValue arcSweepValue = object.value(QStringLiteral("arcSweep"));
    if (!arcSweepValue.isDouble() || !std::isfinite(arcSweepValue.toDouble())) {
        return false;
    }

    QVector<double> subdivisionParameters;
    const QJsonValue subdivisionValue = object.value(QStringLiteral("subdivisionParameters"));
    if (!subdivisionValue.isUndefined()) {
        if (!subdivisionValue.isArray()) {
            return false;
        }
        for (const QJsonValue &parameterValue : subdivisionValue.toArray()) {
            if (!parameterValue.isDouble() || !std::isfinite(parameterValue.toDouble())) {
                return false;
            }
            subdivisionParameters.append(parameterValue.toDouble());
        }
    }

    QVector<Shape::NurbsCurve2D> components;
    const QJsonValue componentsValue = object.value(QStringLiteral("components"));
    if (!componentsValue.isUndefined()) {
        if (!componentsValue.isArray()) {
            return false;
        }
        for (const QJsonValue &componentValue : componentsValue.toArray()) {
            Shape::NurbsCurve2D component;
            if (!nurbsFromJson(componentValue, &component)) {
                return false;
            }
            components.append(component);
        }
    }

    shape->geometryType = geometryType;
    shape->points = points;
    shape->nurbs = nurbs;
    shape->arcMode = static_cast<ArcMode>(arcModeValue);
    shape->arcSweep = arcSweepValue.toDouble();
    shape->subdivisionParameters = subdivisionParameters;
    shape->components = components;
    return true;
}

QString arcModeName(ArcMode mode)
{
    switch (mode) {
    case ArcMode::OnePoint:
        return QStringLiteral("1 Point Arc");
    case ArcMode::TwoPoint:
        return QStringLiteral("2 Point Arc");
    }

    return QStringLiteral("Arc");
}

QString snapTypeName(SnapType type)
{
    switch (type) {
    case SnapType::Endpoint:
        return QStringLiteral("Endpoint");
    case SnapType::Midpoint:
        return QStringLiteral("Midpoint");
    case SnapType::Intersection:
        return QStringLiteral("Intersection");
    case SnapType::Center:
        return QStringLiteral("Center");
    case SnapType::Perpendicular:
        return QStringLiteral("Perpendicular");
    case SnapType::Tangent:
        return QStringLiteral("Tangent");
    case SnapType::ControlPoint:
        return QStringLiteral("ControlPoint");
    case SnapType::None:
        return QStringLiteral("None");
    }

    return QStringLiteral("None");
}

} // namespace classiCAD
