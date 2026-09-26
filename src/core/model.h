#pragma once

#include "geometry/geometry_type.h"
#include "geometry/nurbs_curve.h"
#include "tool_id.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace classiCAD {

enum class ArcMode {
    OnePoint,
    TwoPoint,
};

enum class SnapType {
    None,
    Endpoint,
    Midpoint,
    Intersection,
    Center,
    Perpendicular,
    Tangent,
    ControlPoint,
};

struct SnapResult {
    SnapType type = SnapType::None;
    QPointF point;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

struct SnapCandidate {
    SnapType type = SnapType::None;
    QPointF point;
};

struct LineSegment {
    QPointF start;
    QPointF end;
};

struct HomogeneousControlPoint2D {
    QPointF weightedPosition;
    qreal weight = 1.0;
};

struct RationalBezierSpan2D {
    qreal startParameter = 0.0;
    qreal endParameter = 0.0;
    QVector<HomogeneousControlPoint2D> controlPoints;
};

struct ParameterInterval {
    qreal start = 0.0;
    qreal end = 0.0;
};

struct SampledNurbsCurve2D {
    QVector<qreal> parameters;
    QVector<QPointF> screenPoints;
    QVector<QRectF> segmentBounds;
    QRectF bounds;
};

HomogeneousControlPoint2D blendHomogeneousControlPoints(
    const HomogeneousControlPoint2D &first,
    const HomogeneousControlPoint2D &second,
    qreal secondFraction);

struct DragSnapResult {
    SnapType type = SnapType::None;
    QPointF sourcePoint;
    QPointF targetPoint;
    QPointF translation;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

struct Shape {
    GeometryType geometryType = GeometryType::Invalid;
    QVector<QPointF> points;
    using NurbsCurve2D = classiCAD::NurbsCurve2D;
    NurbsCurve2D nurbs;

    ArcMode arcMode = ArcMode::TwoPoint;
    qreal arcSweep = 0.0;
    // Interior subdivision locations are stored in the source curve's
    // parameter domain. They are markers, not new curve spans or control
    // vertices, so the original NURBS remains unchanged.
    QVector<double> subdivisionParameters;
    // A joined spline remains a Rhino-style component curve collection. Each
    // component keeps its own degree, weights, knots, and parameter domain.
    QVector<NurbsCurve2D> components;
};

struct EraseCurveSampleCache {
    int shapeIndex = -1;
    int componentIndex = -1;
    Shape::NurbsCurve2D curve;
    SampledNurbsCurve2D sampled;
    QVector<qreal> intersectionParameters;
    QVector<ParameterInterval> previewIntervals;
    int previewStrokePointCount = 0;
};

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeBezierNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeCircleNurbs(const QVector<QPointF> &points);

qreal crossProduct(const QPointF &a, const QPointF &b);
bool segmentIntersection(const QPointF &a,
                         const QPointF &b,
                         const QPointF &c,
                         const QPointF &d,
                         QPointF *intersection);

QString pointText(const QPointF &point);

QJsonObject pointToJson(const QPointF &point);
bool pointFromJson(const QJsonValue &value, QPointF *point);
QJsonArray pointsToJson(const QVector<QPointF> &points);
bool pointsFromJson(const QJsonValue &value, QVector<QPointF> *points);
QJsonObject nurbsToJson(const Shape::NurbsCurve2D &curve);
bool nurbsFromJson(const QJsonValue &value, Shape::NurbsCurve2D *curve);
QJsonObject shapeToJson(const Shape &shape);
bool shapeFromJson(const QJsonValue &value, Shape *shape);

QString toolName(ToolId tool);
QString arcModeName(ArcMode mode);
QString snapTypeName(SnapType type);
int requiredPoints(ToolId tool);

} // namespace classiCAD
