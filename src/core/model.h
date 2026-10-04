#pragma once

#include "geometry/geometry_type.h"
#include "geometry/nurbs_curve.h"
#include "geometry/work_plane.h"
#include "document/object_id.h"
#include "tool_id.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace classiCAD {

enum class ArcMode {
    OnePoint,
    TwoPoint,
    ThreePoint,
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
    Near,
};

struct SnapResult {
    SnapType type = SnapType::None;
    QPointF point;
    Point3D worldPoint;
    bool hasWorldPoint = false;

    bool isValid() const
    {
        return type != SnapType::None;
    }
};

struct SnapCandidate {
    SnapCandidate() = default;
    SnapCandidate(SnapType snapType,
                  const QPointF &localPoint,
                  int sourceShapeIndex = -1,
                  int sourceComponentIndex = -1)
        : type(snapType)
        , point(localPoint)
        , shapeIndex(sourceShapeIndex)
        , componentIndex(sourceComponentIndex)
    {
    }

    SnapType type = SnapType::None;
    QPointF point;
    int shapeIndex = -1;
    int componentIndex = -1;
    Point3D worldPoint;
    bool hasWorldPoint = false;
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

enum class DimensionAnchorKind {
    None,
    CurveParameter,
    ControlPoint,
    ShapePoint,
    Center,
};

// Persistent dimensions refer to scene objects by stable identity. Curve
// positions use normalized NURBS parameters to survive knot-domain changes.
struct DimensionAnchorReference {
    ObjectId objectId = ObjectId::invalid();
    DimensionAnchorKind kind = DimensionAnchorKind::None;
    int componentIndex = -1;
    int pointIndex = -1;
    qreal parameterFraction = 0.0;
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
    int targetShapeIndex = -1;
    int targetComponentIndex = -1;

    Point3D worldSourcePoint;
    Point3D worldTargetPoint;
    Point3D worldTranslation;
    bool hasWorldTranslation = false;

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
    // Normally all components share the shape frame. Mixed-plane PolyCurves
    // store one frame per component so the planar NURBS data remains exact.
    // An empty vector is the legacy representation and uses workPlaneFrame.
    QVector<WorkPlaneFrame> componentWorkPlaneFrames;
    QVector<DimensionAnchorReference> dimensionAnchors;
    qreal dimensionOffset = 0.0;
    bool dimensionOffsetValid = false;
    // Picture images are embedded raster scene objects. Their frame is stored
    // as four ordered 2D corners in points (top-left clockwise).
    QImage pictureImage;
    QByteArray pictureImageData;
    WorkPlane workPlane = WorkPlane::XY;
    qreal workPlaneOffset = 0.0;
    // When valid, this frame is authoritative for local 2D geometry. An
    // invalid frame keeps older records on their principal workPlane mapping.
    WorkPlaneFrame workPlaneFrame;
};

WorkPlaneFrame shapeWorkPlaneFrame(const Shape &shape);
WorkPlaneFrame shapeComponentWorkPlaneFrame(const Shape &shape, int componentIndex);
Point3D shapePointToWorld(const Shape &shape, const QPointF &point);
Point3D shapeComponentPointToWorld(const Shape &shape,
                                   int componentIndex,
                                   const QPointF &point);
QPointF shapeWorldPointToLocal(const Shape &shape, const Point3D &point);
QVector<QPointF> polygonVerticesForShape(const Shape &shape);

struct EraseCurveSampleCache {
    int shapeIndex = -1;
    int componentIndex = -1;
    WorkPlaneFrame workPlaneFrame;
    Shape::NurbsCurve2D curve;
    SampledNurbsCurve2D sampled;
    QVector<qreal> intersectionParameters;
    QVector<quint64> intersectionObjectIds;
    QVector<ParameterInterval> previewIntervals;
    int previewStrokePointCount = 0;
};

Shape::NurbsCurve2D makeDegreeOneNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeBezierNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeCircleNurbs(const QVector<QPointF> &points);
Shape::NurbsCurve2D makeEllipseNurbs(EllipseMode mode, const QVector<QPointF> &points);
QVector<QPointF> makeRectanglePoints(RectangleMode mode, const QVector<QPointF> &points);
QVector<QPointF> makeRegularPolygonPoints(PolygonMode mode,
                                          const QVector<QPointF> &points,
                                          int sides);
QVector<QPointF> makePictureFramePoints(const QPointF &firstCorner,
                                         const QPointF &cursorCorner,
                                         qreal imageAspectRatio);
QVector<QPointF> pictureFrameCorners(const Shape &shape);

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
