#include "core/document/document.h"
#include "core/document/layer_id.h"
#include "core/document/object_id.h"
#include "core/document/selection_model.h"
#include "core/geometry/circle_construction.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/geometry_transform.h"
#include "core/geometry/curve_evaluator.h"
#include "core/geometry/nurbs_curve.h"
#include "core/history/history.h"
#include "core/model.h"
#include "core/serialization/document_serializer.h"
#include "core/tool_id.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/dimensions/dimension_association.h"
#include "services/dimensions/dimension_font.h"
#include "services/dimensions/dimension_layout.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
#include "services/viewport/viewport_transform.h"
#include "tools/line_tool.h"
#include "ui/viewport/line_type_style.h"
#include "ui/viewport/blender_grid_frame.h"
#include "ui/viewport/blender_grid_appearance.h"
#include "ui/viewport/blender_grid_scale.h"
#include "ui/viewport/viewport_depth_geometry.h"
#include "ui/viewport/viewport_renderer.h"
#include "ui/viewport/viewport_overlay.h"
#include "ui/viewport/viewport_scene_renderer.h"
#include "tools/circle_tool.h"
#include "tools/circle_tangent_tool.h"
#include "tools/dimension_tool.h"
#include "tools/perpendicular_from_curve_tool.h"
#include "tools/point_tool.h"
#include "tools/polygon_tool.h"
#include "tools/tangent_from_curve_tool.h"
#include "tools/tool_context.h"
#include "tools/tool_registry.h"

#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <type_traits>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    bool passed = true;

    const QStringList layerLineTypes = standardLayerLineTypes();
    const QVector<qreal> dashedPattern = layerLineTypePattern(QStringLiteral("DASHED"));
    const QVector<qreal> dashedHalfPattern = layerLineTypePattern(QStringLiteral("DASHED2"));
    const QVector<qreal> dashedDoublePattern = layerLineTypePattern(QStringLiteral("DASHEDX2"));
    const QPen dashedPen = layerLineTypePen(Qt::white, 1.0, QStringLiteral("DASHED"));
    passed &= check(layerLineTypes.size() >= 24 &&
                        layerLineTypes.contains(QStringLiteral("CENTER")) &&
                        layerLineTypes.contains(QStringLiteral("DIVIDEX2")) &&
                        layerLineTypes.contains(QStringLiteral("DOTX2")) &&
                        canonicalLayerLineTypeName(QStringLiteral("Dash-Dot")) ==
                            QStringLiteral("DASHDOT"),
                    "layer linetype picker must include standard CAD pattern variants and legacy aliases");
    passed &= check(dashedPattern.size() == 2 && dashedHalfPattern.size() == 2 &&
                        dashedDoublePattern.size() == 2 &&
                        qFuzzyCompare(dashedHalfPattern.first() * 2.0 + 1.0,
                                      dashedPattern.first() + 1.0) &&
                        qFuzzyCompare(dashedDoublePattern.first() + 1.0,
                                      dashedPattern.first() * 2.0 + 1.0) &&
                        dashedPen.style() == Qt::CustomDashLine,
                    "linetype size variants must use scaled, visibly distinct custom dash patterns");

    ViewportSceneStroke gpuStroke;
    gpuStroke.width = 2.0f;
    const ViewportSceneStrokePattern solidGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dashed;
    const ViewportSceneStrokePattern dashedGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dotted;
    const ViewportSceneStrokePattern dottedGpuPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dashed;
    const ViewportSceneStrokePattern highDpiDashPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width * 2.0f);
    gpuStroke.linePatternScale = 0.5f;
    const ViewportSceneStrokePattern halfScaleDashPattern =
        viewportSceneStrokePattern(gpuStroke, gpuStroke.width);
    const LayerGpuLinePattern dashedLayerPattern =
        layerGpuLinePattern(QStringLiteral("DASHED2"));
    const LayerGpuLinePattern dottedLayerPattern =
        layerGpuLinePattern(QStringLiteral("DOTX2"));
    const LayerGpuLinePattern complexLayerPattern =
        layerGpuLinePattern(QStringLiteral("CENTER"));
    passed &= check(solidGpuPattern.style == ViewportSceneLineStyle::Solid &&
                        solidGpuPattern.periodPixels == 0.0f &&
                        dashedGpuPattern.style == ViewportSceneLineStyle::Dashed &&
                        dashedGpuPattern.periodPixels == 20.0f &&
                        dashedGpuPattern.onLengthPixels == 14.0f &&
                        dottedGpuPattern.style == ViewportSceneLineStyle::Dotted &&
                        dottedGpuPattern.periodPixels == 6.0f &&
                        dottedGpuPattern.onLengthPixels == 2.0f &&
                        highDpiDashPattern.periodPixels == 40.0f &&
                        highDpiDashPattern.onLengthPixels == 28.0f &&
                        halfScaleDashPattern.periodPixels == 10.0f &&
                        halfScaleDashPattern.onLengthPixels == 7.0f &&
                        dashedLayerPattern.kind ==
                            LayerGpuLinePatternKind::Dashed &&
                        dashedLayerPattern.scale == 0.5 &&
                        dottedLayerPattern.kind ==
                            LayerGpuLinePatternKind::Dotted &&
                        dottedLayerPattern.scale == 2.0 &&
                        complexLayerPattern.kind ==
                            LayerGpuLinePatternKind::Unsupported,
                    "GPU scene strokes must expose solid, 7:3 dashed, and round-dot 1:2 patterns scaled by framebuffer width");
    gpuStroke.lineStyle = ViewportSceneLineStyle::Dotted;
    gpuStroke.dashed = true;
    passed &= check(effectiveViewportSceneLineStyle(gpuStroke) ==
                            ViewportSceneLineStyle::Dashed,
                    "legacy dashed scene strokes must retain dashed rendering when explicit styles are available");
    gpuStroke.dashed = false;
    gpuStroke.controlGuide = true;
    passed &= check(effectiveViewportSceneLineStyle(gpuStroke) ==
                            ViewportSceneLineStyle::Dashed,
                    "control-guide strokes must remain dashed regardless of the selected layer line style");

    passed &= check(!std::is_same_v<ToolId, GeometryType>,
                    "tool and geometry vocabularies must be distinct types");
    passed &= check(geometryTypeForTool(ToolId::Line) == GeometryType::Line,
                    "line tool must map to line geometry");
    passed &= check(geometryTypeForTool(ToolId::Picture) == GeometryType::Picture &&
                        toolName(ToolId::Picture) == QStringLiteral("Picture") &&
                        requiredPoints(ToolId::Picture) == 2 &&
                        isPersistentGeometryType(GeometryType::Picture),
                    "Picture must be a persisted two-corner placement tool");
    passed &= check(isDimensionTool(ToolId::LinearDimension) &&
                        isDimensionTool(ToolId::AngularDimension) &&
                        geometryTypeForTool(ToolId::LinearDimension) ==
                            GeometryType::LinearDimension &&
                        geometryTypeForTool(ToolId::AngularDimension) ==
                            GeometryType::AngularDimension &&
                        requiredPoints(ToolId::LinearDimension) == 3 &&
                        requiredPoints(ToolId::AngularDimension) == 3,
                    "linear and angular dimension commands must be registered scene-annotation tools");
    passed &= check(geometryTypeForTool(ToolId::Trim) == GeometryType::Invalid,
                    "trim must not map to persisted geometry");
    passed &= check(geometryTypeForTool(ToolId::PerpendicularFromCurve) ==
                            GeometryType::Invalid &&
                        toolName(ToolId::PerpendicularFromCurve) ==
                            QStringLiteral("Perpendicular from Curve"),
                    "perpendicular-from-curve must remain a named line command, not persisted geometry");
    passed &= check(geometryTypeForTool(ToolId::Ellipse) == GeometryType::Ellipse &&
                        geometryTypeForTool(ToolId::EllipseFromEndpoints) ==
                            GeometryType::Ellipse &&
                        geometryTypeForTool(ToolId::EllipseFromCorners) ==
                            GeometryType::Ellipse &&
                        geometryTypeForTool(ToolId::EllipseFromFoci) ==
                            GeometryType::Ellipse,
                    "all ellipse construction tools must create ellipse geometry");
    passed &= check(geometryTypeForTool(ToolId::Rectangle) == GeometryType::Rectangle &&
                        geometryTypeForTool(ToolId::RectangleFromCenter) ==
                            GeometryType::Rectangle &&
                        geometryTypeForTool(ToolId::RectangleThreePoint) ==
                            GeometryType::Rectangle &&
                        requiredPoints(ToolId::Rectangle) == 2 &&
                        requiredPoints(ToolId::RectangleFromCenter) == 2 &&
                        requiredPoints(ToolId::RectangleThreePoint) == 3,
                    "rectangle construction tools must map to rectangle geometry and request the correct clicks");
    passed &= check(isPolygonTool(ToolId::PolygonCenterCorner) &&
                        geometryTypeForTool(ToolId::PolygonCenterCorner) ==
                            GeometryType::Polygon &&
                        geometryTypeForTool(ToolId::PolygonCenterTangent) ==
                            GeometryType::Polygon &&
                        geometryTypeForTool(ToolId::PolygonCornerCorner) ==
                            GeometryType::Polygon &&
                        geometryTypeForTool(ToolId::PolygonEdge) == GeometryType::Polygon &&
                        requiredPoints(ToolId::PolygonCenterCorner) == 2 &&
                        requiredPoints(ToolId::PolygonCenterTangent) == 2 &&
                        requiredPoints(ToolId::PolygonCornerCorner) == 2 &&
                        requiredPoints(ToolId::PolygonEdge) == 2,
                    "each polygon construction mode must create polygon geometry from two clicks");
    passed &= check(isCircleConstructionTool(ToolId::Circle) &&
                        isCircleConstructionTool(ToolId::CircleDiameter) &&
                        isCircleConstructionTool(ToolId::CircleThreePoint) &&
                        isCircleTangentTool(ToolId::CircleTangentTwo) &&
                        isCircleTangentTool(ToolId::CircleTangentThree) &&
                        geometryTypeForTool(ToolId::CircleDiameter) == GeometryType::Circle &&
                        geometryTypeForTool(ToolId::CircleThreePoint) == GeometryType::Circle &&
                        geometryTypeForTool(ToolId::CircleTangentTwo) == GeometryType::Circle &&
                        geometryTypeForTool(ToolId::CircleTangentThree) == GeometryType::Circle &&
                        requiredPoints(ToolId::Circle) == 2 &&
                        requiredPoints(ToolId::CircleDiameter) == 2 &&
                        requiredPoints(ToolId::CircleThreePoint) == 3,
                    "circle construction variants must map to circle geometry and request their defining clicks");
    passed &= check(geometryTypeForTool(ToolId::Mirror) == GeometryType::Invalid &&
                        toolName(ToolId::Mirror) == QStringLiteral("Mirror"),
                    "mirror must remain a command rather than persisted geometry");
    passed &= check(legacyValueForGeometryType(GeometryType::PolyCurve) == 8,
                    "PolyCurve legacy value must remain eight");

    const ObjectId objectId = ObjectId::fromValue(42);
    const LayerId layerId = LayerId::fromValue(42);
    passed &= check(objectId.isValid() && objectId.value() == 42,
                    "object IDs must preserve their stable value");
    passed &= check(layerId.isValid() && layerId.value() == 42,
                    "layer IDs must preserve their stable value");
    passed &= check(ObjectId::invalid() != objectId && LayerId::invalid() != layerId,
                    "zero IDs must remain the invalid sentinel");

    const NurbsCurve2D line = makeDegreeOneNurbs({QPointF(0.0, 0.0), QPointF(10.0, 0.0)});
    QString validationError;
    passed &= check(validateNurbsCurve(line, &validationError) && validationError.isEmpty(),
                    "factory line must satisfy NURBS invariants");

    NurbsCurve2D invalidCurve = line;
    invalidCurve.order = 3;
    passed &= check(!validateNurbsCurve(invalidCurve),
                    "invalid NURBS order must be rejected");

    const Shape lineShape{GeometryType::Line,
                          {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                          line,
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};

    const Shape circle{GeometryType::Circle,
                       {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                       makeCircleNurbs({QPointF(0.0, 0.0), QPointF(10.0, 0.0)}),
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
    passed &= check(validateNurbsCurve(circle.nurbs),
                    "factory circle must satisfy rational NURBS invariants");
    Shape editedCircleSeam = circle;
    editedCircleSeam.nurbs.controlPoints.last() += QPointF(0.0, 2.0);
    const QPointF movedFirstSeam =
        editedCircleSeam.nurbs.controlPoints.first() + QPointF(3.0, -4.0);
    passed &= check(setClosedNurbsSeamControlPoint(&editedCircleSeam,
                                                   0,
                                                   movedFirstSeam) &&
                        editedCircleSeam.nurbs.controlPoints.first() == movedFirstSeam &&
                        editedCircleSeam.nurbs.controlPoints.last() == movedFirstSeam,
                    "moving a circle seam control point must move both duplicated CVs together");
    const int lastCircleControlPoint =
        editedCircleSeam.nurbs.controlPoints.size() - 1;
    const QPointF movedLastSeam = movedFirstSeam + QPointF(-2.0, 5.0);
    passed &= check(setClosedNurbsSeamControlPoint(&editedCircleSeam,
                                                   lastCircleControlPoint,
                                                   movedLastSeam) &&
                        editedCircleSeam.nurbs.controlPoints.first() == movedLastSeam &&
                        editedCircleSeam.nurbs.controlPoints.last() == movedLastSeam,
                    "dragging either circle seam CV must preserve closure and heal an open seam");
    Shape brokenSavedCircle = circle;
    brokenSavedCircle.nurbs.controlPoints.last() += QPointF(0.0, 2.0);
    Shape restoredClosedCircle;
    passed &= check(shapeFromJson(shapeToJson(brokenSavedCircle), &restoredClosedCircle) &&
                        restoredClosedCircle.nurbs.controlPoints.first() ==
                            restoredClosedCircle.nurbs.controlPoints.last() &&
                        restoredClosedCircle.nurbs.controlPoints.first() ==
                            circle.nurbs.controlPoints.first() + QPointF(0.0, 1.0),
                    "loading a circle with a split seam must restore a closed control-point loop");
    QVector<QPointF> diameterCircleDefinition;
    QVector<QPointF> threePointCircleDefinition;
    passed &= check(makeCircleDefinitionFromDiameter(QPointF(-5.0, 0.0),
                                                     QPointF(5.0, 0.0),
                                                     &diameterCircleDefinition) &&
                        diameterCircleDefinition ==
                            QVector<QPointF>{QPointF(0.0, 0.0), QPointF(-5.0, 0.0)} &&
                        makeCircleDefinitionFromThreePoints(QPointF(1.0, 0.0),
                                                            QPointF(0.0, 1.0),
                                                            QPointF(-1.0, 0.0),
                                                            &threePointCircleDefinition) &&
                        std::hypot(threePointCircleDefinition[0].x(),
                                   threePointCircleDefinition[0].y()) <= 1.0e-9 &&
                        std::abs(std::hypot(threePointCircleDefinition[1].x() -
                                                threePointCircleDefinition[0].x(),
                                            threePointCircleDefinition[1].y() -
                                                threePointCircleDefinition[0].y()) -
                                 1.0) <= 1.0e-9,
                    "diameter and three-point circle construction must produce a center-radius definition");
    passed &= check(!makeCircleDefinitionFromThreePoints(QPointF(0.0, 0.0),
                                                         QPointF(1.0, 0.0),
                                                         QPointF(2.0, 0.0),
                                                         &threePointCircleDefinition),
                    "three-point circle construction must reject collinear points");
    constexpr qreal quarterTurn = 0.78539816339744830962;
    QPointF circleDerivative;
    passed &= check(evaluateNurbsDerivative(circle.nurbs,
                                            quarterTurn,
                                            &circleDerivative) &&
                        std::abs(circleDerivative.x() + circleDerivative.y()) <= 1.0e-8 &&
                        circleDerivative.x() < 0.0 && circleDerivative.y() > 0.0,
                    "rational NURBS derivative must match the exact circle tangent");

    const QVector<QPointF> centerAxisEllipsePoints{QPointF(0.0, 0.0),
                                                   QPointF(5.0, 0.0),
                                                   QPointF(0.0, 4.0)};
    const QVector<QPointF> axisEndpointEllipsePoints{QPointF(-5.0, 0.0),
                                                     QPointF(5.0, 0.0),
                                                     QPointF(0.0, 4.0)};
    const QVector<QPointF> cornerEllipsePoints{QPointF(-5.0, -4.0),
                                               QPointF(5.0, 4.0)};
    const QVector<QPointF> fociEllipsePoints{QPointF(-3.0, 0.0),
                                             QPointF(3.0, 0.0),
                                             QPointF(0.0, 4.0)};
    const NurbsCurve2D centerAxisEllipse = makeEllipseNurbs(
        EllipseMode::CenterAxisRadius, centerAxisEllipsePoints);
    const QVector<NurbsCurve2D> ellipseMethods{
        centerAxisEllipse,
        makeEllipseNurbs(EllipseMode::AxisEndpoints, axisEndpointEllipsePoints),
        makeEllipseNurbs(EllipseMode::Corners, cornerEllipsePoints),
        makeEllipseNurbs(EllipseMode::FociPoint, fociEllipsePoints)};
    bool ellipseCurvesValid = true;
    bool ellipseMethodsAgree = true;
    bool ellipseQuarterPointsCorrect = true;
    const auto pointsAlmostEqual = [](const QPointF &first, const QPointF &second) {
        return std::hypot(first.x() - second.x(), first.y() - second.y()) <= 1.0e-8;
    };
    for (const NurbsCurve2D &ellipseCurve : ellipseMethods) {
        bool thisMethodAgrees = true;
        ellipseCurvesValid &= validateNurbsCurve(ellipseCurve) &&
                              ellipseCurve.dimension == 2 && ellipseCurve.degree == 2 &&
                              ellipseCurve.order == 3 && ellipseCurve.rational &&
                              ellipseCurve.controlPoints.size() == 9 &&
                              ellipseCurve.weights.size() == 9 &&
                              ellipseCurve.knots.size() == 10;
        for (int index = 0;
             index < ellipseCurve.controlPoints.size() &&
             index < centerAxisEllipse.controlPoints.size();
             ++index) {
            const QPointF actualPoint = ellipseCurve.controlPoints[index];
            const QPointF expectedPoint = centerAxisEllipse.controlPoints[index];
            thisMethodAgrees &= pointsAlmostEqual(actualPoint, expectedPoint);
            thisMethodAgrees &= std::abs(ellipseCurve.weights[index] -
                                         centerAxisEllipse.weights[index]) <= 1.0e-12;
        }
        thisMethodAgrees &= ellipseCurve.knots == centerAxisEllipse.knots;
        ellipseMethodsAgree &= thisMethodAgrees;
    }
    const QVector<QPointF> ellipseExpectedQuarterPoints{QPointF(5.0, 0.0),
                                                        QPointF(0.0, 4.0),
                                                        QPointF(-5.0, 0.0),
                                                        QPointF(0.0, -4.0),
                                                        QPointF(5.0, 0.0)};
    constexpr qreal ellipseQuarterTurn = 1.57079632679489661923;
    for (int index = 0; index < ellipseExpectedQuarterPoints.size(); ++index) {
        QPointF evaluatedPoint;
        ellipseQuarterPointsCorrect &= evaluateNurbsPoint(centerAxisEllipse,
                                                           ellipseQuarterTurn * index,
                                                           &evaluatedPoint) &&
                                       pointsAlmostEqual(
                                           evaluatedPoint,
                                           ellipseExpectedQuarterPoints[index]);
    }
    passed &= check(ellipseCurvesValid,
                    "ellipse builder must create valid rational degree-2 NURBS data");
    passed &= check(ellipseMethodsAgree,
                    "ellipse construction methods must produce equivalent NURBS geometry");
    passed &= check(ellipseQuarterPointsCorrect,
                    "ellipse evaluation must hit the four axis extrema over its full domain");

    const QVector<QPointF> cornerRectangle = makeRectanglePoints(
        RectangleMode::CornerCorner,
        {QPointF(1.0, 2.0), QPointF(5.0, 6.0)});
    const QVector<QPointF> centerRectangle = makeRectanglePoints(
        RectangleMode::CenterCorner,
        {QPointF(0.0, 0.0), QPointF(2.0, 1.0)});
    const QVector<QPointF> threePointRectangle = makeRectanglePoints(
        RectangleMode::ThreePoint,
        {QPointF(1.0, 1.0), QPointF(3.0, 3.0), QPointF(0.0, 4.0)});
    const auto pointListsAlmostEqual = [&pointsAlmostEqual](const QVector<QPointF> &first,
                                                            const QVector<QPointF> &second) {
        if (first.size() != second.size()) {
            return false;
        }
        for (int index = 0; index < first.size(); ++index) {
            if (!pointsAlmostEqual(first[index], second[index])) {
                return false;
            }
        }
        return true;
    };
    passed &= check(pointListsAlmostEqual(
                        cornerRectangle,
                        {QPointF(1.0, 2.0), QPointF(5.0, 2.0),
                         QPointF(5.0, 6.0), QPointF(1.0, 6.0)}) &&
                        pointListsAlmostEqual(
                            centerRectangle,
                            {QPointF(2.0, 1.0), QPointF(-2.0, 1.0),
                             QPointF(-2.0, -1.0), QPointF(2.0, -1.0)}) &&
                        pointListsAlmostEqual(
                            threePointRectangle,
                            {QPointF(1.0, 1.0), QPointF(3.0, 3.0),
                             QPointF(1.0, 5.0), QPointF(-1.0, 3.0)}),
                    "rectangle builders must support corner, center, and oriented three-point construction");

    const QVector<QPointF> centerCornerPolygon = makeRegularPolygonPoints(
        PolygonMode::CenterCorner,
        {QPointF(0.0, 0.0), QPointF(2.0, 0.0)},
        6);
    const QVector<QPointF> centerTangentPolygon = makeRegularPolygonPoints(
        PolygonMode::CenterTangent,
        {QPointF(0.0, 0.0), QPointF(0.0, 2.0)},
        4);
    const QVector<QPointF> cornerCornerPolygon = makeRegularPolygonPoints(
        PolygonMode::CornerCorner,
        {QPointF(0.0, 0.0), QPointF(2.0, 0.0)},
        4);
    const QVector<QPointF> edgePolygon = makeRegularPolygonPoints(
        PolygonMode::Edge,
        {QPointF(0.0, 0.0), QPointF(0.0, 5.0)},
        6);
    const QPointF centerTangentSideMidpoint =
        (centerTangentPolygon.value(0) + centerTangentPolygon.value(1)) * 0.5;
    const QPointF oppositeEdgeMidpoint =
        (edgePolygon.value(3) + edgePolygon.value(4)) * 0.5;
    bool centerCornerRadiusIsConsistent = centerCornerPolygon.size() == 6;
    for (const QPointF &vertex : centerCornerPolygon) {
        centerCornerRadiusIsConsistent &=
            std::abs(std::hypot(vertex.x(), vertex.y()) - 2.0) <= 1.0e-8;
    }
    passed &= check(centerCornerRadiusIsConsistent &&
                        pointsAlmostEqual(centerCornerPolygon.value(0), QPointF(2.0, 0.0)) &&
                        pointsAlmostEqual(centerTangentSideMidpoint, QPointF(0.0, 2.0)) &&
                        pointsAlmostEqual(cornerCornerPolygon.value(0), QPointF(0.0, 0.0)) &&
                        pointsAlmostEqual(cornerCornerPolygon.value(1), QPointF(2.0, 0.0)) &&
                        edgePolygon.size() == 7 &&
                        pointsAlmostEqual(edgePolygon.value(0), QPointF(0.0, 0.0)) &&
                        pointsAlmostEqual(oppositeEdgeMidpoint, QPointF(0.0, 5.0)),
                    "polygon builders must honor center/corner, center/tangent, corner/edge, and odd-side span construction");

    const Shape polygonShape{GeometryType::Polygon,
                             centerCornerPolygon,
                             {},
                             ArcMode::TwoPoint,
                             0.0,
                             {},
                             {}};

    const Shape ellipse{GeometryType::Ellipse,
                        {QPointF(0.0, 0.0)},
                        centerAxisEllipse,
                        ArcMode::TwoPoint,
                        0.0,
                        {},
                        {}};

    Shape mirroredLine;
    passed &= check(mirrorShapeAcrossLine(lineShape,
                                          QPointF(0.0, 0.0),
                                          QPointF(0.0, 10.0),
                                          &mirroredLine) &&
                        mirroredLine.points == QVector<QPointF>{QPointF(0.0, 0.0),
                                                                QPointF(-10.0, 0.0)} &&
                        mirroredLine.nurbs.controlPoints ==
                            QVector<QPointF>{QPointF(0.0, 0.0), QPointF(-10.0, 0.0)} &&
                        mirroredLine.nurbs.weights == lineShape.nurbs.weights &&
                        mirroredLine.nurbs.knots == lineShape.nurbs.knots &&
                        validateNurbsCurve(mirroredLine.nurbs),
                    "mirror must transform stored NURBS positions while preserving invariants");

    Shape rectangle{GeometryType::Rectangle,
                    {QPointF(1.0, 1.0), QPointF(3.0, 4.0)},
                    {},
                    ArcMode::TwoPoint,
                    0.0,
                    {},
                    {}};
    Shape mirroredRectangle;
    passed &= check(mirrorShapeAcrossLine(rectangle,
                                          QPointF(0.0, 0.0),
                                          QPointF(1.0, 0.0),
                                          &mirroredRectangle) &&
                        mirroredRectangle.points.size() == 4 &&
                        mirroredRectangle.points[0] == QPointF(1.0, -1.0) &&
                        mirroredRectangle.points[2] == QPointF(3.0, -4.0),
                    "mirror must expand and reflect two-point rectangles");

    const QJsonObject serialized = shapeToJson(circle);
    passed &= check(serialized.value(QStringLiteral("geometryType")).toInt(-1) == 6,
                    "new sessions must write the geometry type field");
    passed &= check(serialized.value(QStringLiteral("tool")).toInt(-1) == 6,
                    "new sessions must retain the legacy compatibility field");

    Shape xzCircle = circle;
    xzCircle.workPlane = WorkPlane::XZ;
    xzCircle.workPlaneOffset = 12.5;
    Shape restoredXzCircle;
    passed &= check(shapeFromJson(shapeToJson(xzCircle), &restoredXzCircle) &&
                        restoredXzCircle.workPlane == WorkPlane::XZ &&
                        qFuzzyCompare(restoredXzCircle.workPlaneOffset + 1.0, 13.5),
                    "geometry serialization must preserve a curve's construction plane and offset");

    const QJsonObject serializedEllipse = shapeToJson(ellipse);
    passed &= check(serializedEllipse.value(QStringLiteral("geometryType")).toInt(-1) == 9 &&
                        serializedEllipse.value(QStringLiteral("tool")).toInt(-1) == 0,
                    "ellipse sessions must use their canonical type without colliding with legacy tool values");
    const QJsonObject serializedPolygon = shapeToJson(polygonShape);
    Shape restoredPolygon{GeometryType::Invalid,
                          {},
                          {},
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};
    passed &= check(serializedPolygon.value(QStringLiteral("geometryType")).toInt(-1) == 10 &&
                        serializedPolygon.value(QStringLiteral("tool")).toInt(-1) == 0 &&
                        shapeFromJson(serializedPolygon, &restoredPolygon) &&
                        restoredPolygon.geometryType == GeometryType::Polygon &&
                        restoredPolygon.points == centerCornerPolygon,
                    "polygon geometry must use its canonical persisted type and survive session serialization");

    Shape picture;
    picture.geometryType = GeometryType::Picture;
    picture.points = makePictureFramePoints(QPointF(5.0, 8.0),
                                            QPointF(10.0, 2.0),
                                            2.0);
    picture.pictureImage = QImage(4, 2, QImage::Format_ARGB32);
    picture.pictureImage.fill(QColor(QStringLiteral("#d06040")));
    const qreal pictureWidth = picture.points[1].x() - picture.points[0].x();
    const qreal pictureHeight = picture.points[0].y() - picture.points[3].y();
    Shape restoredPicture;
    const QJsonObject serializedPicture = shapeToJson(picture);
    passed &= check(picture.points.size() == 4 &&
                        qFuzzyCompare(pictureWidth / pictureHeight, 2.0) &&
                        serializedPicture.value(QStringLiteral("geometryType")).toInt(-1) == 13 &&
                        shapeFromJson(serializedPicture, &restoredPicture) &&
                        restoredPicture.geometryType == GeometryType::Picture &&
                        restoredPicture.points == picture.points &&
                        restoredPicture.pictureImage.size() == picture.pictureImage.size() &&
                        restoredPicture.pictureImage.pixelColor(0, 0) ==
                            picture.pictureImage.pixelColor(0, 0),
                    "Picture frames must preserve image aspect ratio and embed their pixels in shape JSON");
    QJsonObject pictureWithoutImage = serializedPicture;
    pictureWithoutImage.remove(QStringLiteral("pictureImage"));
    passed &= check(!shapeFromJson(pictureWithoutImage, &restoredPicture),
                    "a persisted Picture without embedded image data must be rejected");
    const Shape linearDimension{GeometryType::LinearDimension,
                                {QPointF(0.0, 0.0),
                                 QPointF(12.0, 0.0),
                                 QPointF(6.0, 4.0)},
                                {},
                                ArcMode::TwoPoint,
                                0.0,
                                {},
                                {}};
    Shape restoredDimension{GeometryType::Invalid, {}, {}, ArcMode::TwoPoint, 0.0, {}, {}};
    const QJsonObject serializedDimension = shapeToJson(linearDimension);
    passed &= check(serializedDimension.value(QStringLiteral("geometryType")).toInt(-1) == 11 &&
                        serializedDimension.value(QStringLiteral("tool")).toInt(-1) == 0 &&
                        shapeFromJson(serializedDimension, &restoredDimension) &&
                        restoredDimension.geometryType == GeometryType::LinearDimension &&
                        restoredDimension.points == linearDimension.points,
                    "linear dimension annotations must persist their anchors without colliding with legacy tool values");
    Shape angularDimension{GeometryType::AngularDimension,
                           {QPointF(0.0, 0.0),
                            QPointF(10.0, 0.0),
                            QPointF(0.0, 10.0)},
                           {},
                           ArcMode::TwoPoint,
                           0.0,
                           {},
                           {}};
    const QJsonObject serializedAngularDimension = shapeToJson(angularDimension);
    passed &= check(serializedAngularDimension.value(QStringLiteral("geometryType")).toInt(-1) == 12 &&
                        shapeFromJson(serializedAngularDimension, &restoredDimension) &&
                        restoredDimension.geometryType == GeometryType::AngularDimension &&
                        restoredDimension.points == angularDimension.points,
                    "angular dimension annotations must survive session serialization");

    Document associativeDocument;
    Shape associativeLine{GeometryType::Line,
                          {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                          {},
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};
    associativeLine.nurbs = makeDegreeOneNurbs(associativeLine.points);
    const ObjectId associativeLineId = associativeDocument.append(associativeLine);
    CurveSampler associationSampler;
    ViewportTransform associationTransform;
    associationTransform.zoom() = 20.0;
    const QSize associationViewportSize(320, 240);
    const DimensionAnchorReference firstAssociation = captureDimensionAnchor(
        associativeDocument,
        QPointF(0.0, 0.0),
        SnapType::Endpoint,
        associationSampler,
        associationTransform,
        associationViewportSize);
    const DimensionAnchorReference secondAssociation = captureDimensionAnchor(
        associativeDocument,
        QPointF(10.0, 0.0),
        SnapType::Endpoint,
        associationSampler,
        associationTransform,
        associationViewportSize);
    Shape associativeDimension = linearDimension;
    associativeDimension.dimensionAnchors = {firstAssociation, secondAssociation};
    associativeDimension.dimensionOffset = 4.0;
    associativeDimension.dimensionOffsetValid = true;
    passed &= check(firstAssociation.objectId == associativeLineId &&
                        secondAssociation.objectId == associativeLineId &&
                        firstAssociation.kind == DimensionAnchorKind::CurveParameter &&
                        secondAssociation.kind == DimensionAnchorKind::CurveParameter,
                    "dimension clicks on a curve must capture stable geometry references");
    const QJsonObject serializedAssociativeDimension = shapeToJson(associativeDimension);
    Shape restoredAssociativeDimension;
    passed &= check(shapeFromJson(serializedAssociativeDimension,
                                  &restoredAssociativeDimension) &&
                        restoredAssociativeDimension.dimensionAnchors.size() == 2 &&
                        restoredAssociativeDimension.dimensionAnchors[1].objectId ==
                            associativeLineId &&
                        restoredAssociativeDimension.dimensionOffsetValid &&
                        std::abs(restoredAssociativeDimension.dimensionOffset - 4.0) < 1.0e-9,
                    "associative dimension targets and placement must survive serialization");
    const ObjectId associativeDimensionId =
        associativeDocument.append(associativeDimension);
    Document restoredAssociationDocument;
    QString associationDocumentError;
    const bool associationDocumentRestored = documentFromJson(
        documentToJson(associativeDocument),
        &restoredAssociationDocument,
        &associationDocumentError);
    Shape *editedAssociativeLine =
        restoredAssociationDocument.shape(associativeLineId);
    const bool associationTargetEdited = editedAssociativeLine != nullptr;
    if (associationTargetEdited) {
        editedAssociativeLine->points = {QPointF(5.0, 2.0), QPointF(25.0, 2.0)};
        editedAssociativeLine->nurbs = makeDegreeOneNurbs(editedAssociativeLine->points);
    }
    updateAssociativeDimensions(restoredAssociationDocument, associationSampler);
    const Shape *updatedAssociativeDimension =
        restoredAssociationDocument.shape(associativeDimensionId);
    passed &= check(associationDocumentRestored && associationDocumentError.isEmpty() &&
                        associationTargetEdited &&
                        updatedAssociativeDimension != nullptr &&
                        updatedAssociativeDimension->points.size() == 3 &&
                        std::abs(updatedAssociativeDimension->points[0].x() - 5.0) < 1.0e-3 &&
                        std::abs(updatedAssociativeDimension->points[0].y() - 2.0) < 1.0e-3 &&
                        std::abs(updatedAssociativeDimension->points[1].x() - 25.0) < 1.0e-3 &&
                        std::abs(updatedAssociativeDimension->points[1].y() - 2.0) < 1.0e-3 &&
                        std::abs(updatedAssociativeDimension->points[2].x() - 15.0) < 1.0e-3 &&
                        std::abs(updatedAssociativeDimension->points[2].y() - 6.0) < 1.0e-3,
                    "associative dimensions must follow geometry edits and preserve their offset");

    Shape associativeBezier{GeometryType::Bezier,
                            {QPointF(0.0, 0.0),
                             QPointF(5.0, 10.0),
                             QPointF(10.0, 0.0)},
                            {},
                            ArcMode::TwoPoint,
                            0.0,
                            {},
                            {}};
    associativeBezier.nurbs = makeBezierNurbs(associativeBezier.points);
    const ObjectId associativeBezierId = associativeDocument.append(associativeBezier);
    qreal bezierDomainStart = 0.0;
    qreal bezierDomainEnd = 0.0;
    QPointF bezierAnchorPoint;
    const bool bezierAnchorEvaluated =
        nurbsParameterDomain(associativeBezier.nurbs,
                             &bezierDomainStart,
                             &bezierDomainEnd) &&
        evaluateNurbsPoint(associativeBezier.nurbs,
                           (bezierDomainStart + bezierDomainEnd) * 0.5,
                           &bezierAnchorPoint);
    const DimensionAnchorReference bezierAssociation = captureDimensionAnchor(
        associativeDocument,
        bezierAnchorPoint,
        SnapType::Near,
        associationSampler,
        associationTransform,
        associationViewportSize);
    Shape *editedAssociativeBezier = associativeDocument.shape(associativeBezierId);
    editedAssociativeBezier->points[1].setY(20.0);
    editedAssociativeBezier->nurbs = makeBezierNurbs(editedAssociativeBezier->points);
    QPointF movedBezierAnchor;
    passed &= check(bezierAnchorEvaluated &&
                        bezierAssociation.objectId == associativeBezierId &&
                        resolveDimensionAnchor(associativeDocument,
                                               bezierAssociation,
                                               associationSampler,
                                               &movedBezierAnchor) &&
                        movedBezierAnchor.y() > bezierAnchorPoint.y() + 4.0,
                    "associative curve anchors must reevaluate on edited NURBS geometry");

    Shape associativeEllipse{GeometryType::Ellipse,
                             {QPointF(-5.0, 0.0),
                              QPointF(5.0, 0.0),
                              QPointF(0.0, 3.0)},
                             makeEllipseNurbs(EllipseMode::AxisEndpoints,
                                              {QPointF(-5.0, 0.0),
                                               QPointF(5.0, 0.0),
                                               QPointF(0.0, 3.0)}),
                             ArcMode::TwoPoint,
                             0.0,
                             {},
                             {}};
    const ObjectId associativeEllipseId = associativeDocument.append(associativeEllipse);
    const DimensionAnchorReference ellipseCenterAssociation = captureDimensionAnchor(
        associativeDocument,
        QPointF(0.0, 0.0),
        SnapType::Center,
        associationSampler,
        associationTransform,
        associationViewportSize);
    Shape *editedAssociativeEllipse = associativeDocument.shape(associativeEllipseId);
    editedAssociativeEllipse->points = {QPointF(-3.0, 3.0),
                                        QPointF(7.0, 3.0),
                                        QPointF(2.0, 6.0)};
    editedAssociativeEllipse->nurbs = makeEllipseNurbs(
        EllipseMode::AxisEndpoints, editedAssociativeEllipse->points);
    QPointF movedEllipseCenter;
    passed &= check(ellipseCenterAssociation.objectId == associativeEllipseId &&
                        resolveDimensionAnchor(associativeDocument,
                                               ellipseCenterAssociation,
                                               associationSampler,
                                               &movedEllipseCenter) &&
                        std::abs(movedEllipseCenter.x() - 2.0) < 1.0e-9 &&
                        std::abs(movedEllipseCenter.y() - 3.0) < 1.0e-9,
                    "ellipse center dimensions must follow the actual center across ellipse modes");

    Shape restoredEllipse{GeometryType::Invalid,
                          {},
                          {},
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};
    passed &= check(shapeFromJson(serializedEllipse, &restoredEllipse) &&
                        restoredEllipse.geometryType == GeometryType::Ellipse &&
                        validateNurbsCurve(restoredEllipse.nurbs),
                    "ellipse NURBS geometry must survive session serialization");

    Shape restored{GeometryType::Invalid,
                   {},
                   {},
                   ArcMode::TwoPoint,
                   0.0,
                   {},
                   {}};
    passed &= check(shapeFromJson(serialized, &restored) &&
                        restored.geometryType == GeometryType::Circle,
                    "new geometry type sessions must restore");

    QJsonObject legacySerialized = serialized;
    legacySerialized.remove(QStringLiteral("geometryType"));
    Shape legacyRestored{GeometryType::Invalid,
                         {},
                         {},
                         ArcMode::TwoPoint,
                         0.0,
                         {},
                         {}};
    passed &= check(shapeFromJson(legacySerialized, &legacyRestored) &&
                        legacyRestored.geometryType == GeometryType::Circle,
                    "version-1 tool sessions must remain readable");

    QJsonObject commandAsShape = legacySerialized;
    commandAsShape.insert(QStringLiteral("tool"), static_cast<int>(ToolId::Trim));
    passed &= check(!shapeFromJson(commandAsShape, &legacyRestored),
                    "commands must not deserialize as persistent geometry");

    QJsonObject invalidCurveSerialized = serialized;
    QJsonObject invalidNurbs = invalidCurveSerialized.value(QStringLiteral("nurbs")).toObject();
    QJsonArray invalidWeights = invalidNurbs.value(QStringLiteral("weights")).toArray();
    invalidWeights.replace(0, -1.0);
    invalidNurbs.insert(QStringLiteral("weights"), invalidWeights);
    invalidCurveSerialized.insert(QStringLiteral("nurbs"), invalidNurbs);
    passed &= check(!shapeFromJson(invalidCurveSerialized, &restored),
                    "serialized curves with non-positive weights must be rejected");

    Document document;
    passed &= check(document.layers().size() == 2 &&
                        document.layers().first().name == QStringLiteral("0") &&
                        document.layers().last().name == QStringLiteral("Defpoints") &&
                        !document.layers().last().plotted,
                    "new drawings must start with layer 0 and a non-plot Defpoints layer");
    const LayerId sketchLayer = document.createLayer(QStringLiteral("Sketch"));
    const ObjectId lineObject = document.append(lineShape);
    const ObjectId circleObject = document.append(circle);
    passed &= check(document.indexOf(lineObject) == 0 &&
                        document.indexOf(circleObject) == 1,
                    "document must own objects in insertion order");
    passed &= check(document.moveObjectToLayer(circleObject, sketchLayer) &&
                        document.layer(sketchLayer)->objectIds.contains(circleObject),
                    "document must maintain layer object membership");
    passed &= check(document.setLayerLocked(sketchLayer, true) &&
                        !document.isObjectEditable(circleObject) &&
                        document.isObjectEditable(lineObject),
                    "layer locking must affect object editability");

    const Document::Snapshot documentSnapshot = document.snapshot();
    document.remove(lineObject);
    passed &= check(document.indexOf(circleObject) == 0,
                    "removing an object must not change its stable identity");
    document.restoreSnapshot(documentSnapshot);
    passed &= check(document.indexOf(lineObject) == 0 &&
                        document.indexOf(circleObject) == 1,
                    "document snapshots must restore stable object identities");

    SelectionModel selection;
    selection.setObjectIds({lineObject, circleObject}, circleObject);
    selection.setActiveControlPoint(circleObject, 2);
    passed &= check(selection.objectIds().size() == 2 &&
                        selection.primaryObjectId() == circleObject &&
                        selection.activeControlPoint().isValid(),
                    "selection model must own object and control-point references");
    document.remove(lineObject);
    selection.prune(document);
    passed &= check(selection.objectIds() == QVector<ObjectId>{circleObject} &&
                        selection.primaryObjectId() == circleObject,
                    "selection model must prune deleted object IDs");

    Document layerDocument;
    DocumentSettings persistedGridSettings;
    persistedGridSettings.lengthUnit = DocumentLengthUnit::Inch;
    persistedGridSettings.gridSpacing = 0.25;
    passed &= check(layerDocument.setSettings(persistedGridSettings) &&
                        std::abs(documentGridSpacingInMillimeters(layerDocument.settings()) -
                                 6.35) < 1.0e-12,
                    "document grid spacing must convert the chosen length unit to canonical millimeters");
    const LayerId layeredSketch = layerDocument.createLayer(QStringLiteral("Sketch"));
    const LayerId layeredConstruction =
        layerDocument.createLayer(QStringLiteral("Construction"));
    const QColor sketchLayerColor(QStringLiteral("#56aaff"));
    const ObjectId layeredObject = layerDocument.append(lineShape);
    passed &= check(layerDocument.moveObjectToLayer(layeredObject, layeredSketch) &&
                        layerDocument.setLayerColor(layeredSketch, sketchLayerColor) &&
                        layerDocument.setLayerLineType(layeredSketch,
                                                       QStringLiteral("Dashed")) &&
                        layerDocument.setLayerLineWeight(layeredSketch, 0.35) &&
                        layerDocument.setLayerPlotted(layeredSketch, false) &&
                        layerDocument.setLayerDescription(layeredSketch,
                                                          QStringLiteral("Sketch geometry")) &&
                        layerDocument.renameLayer(layeredSketch, QStringLiteral("Sketch Curves")) &&
                        layerDocument.moveLayer(layeredConstruction, 0),
                    "document must support stable layer rename and reorder operations");
    passed &= check(layerDocument.setActiveLayer(layeredConstruction) &&
                        layerDocument.activeLayerId() == layeredConstruction,
                    "document must track an editable active layer");
    passed &= check(layerDocument.setLayerVisible(layeredSketch, false) &&
                        !layerDocument.isObjectVisible(layeredObject) &&
                        !layerDocument.isObjectEditable(layeredObject),
                    "layer visibility must filter object visibility and editability");
    passed &= check(layerDocument.setLayerVisible(layeredSketch, true) &&
                        layerDocument.setLayerLocked(layeredSketch, true) &&
                        !layerDocument.isObjectEditable(layeredObject) &&
                        !layerDocument.moveObjectToLayer(layeredObject, layeredConstruction),
                    "layer locking must prevent object movement from a locked layer");
    passed &= check(layerDocument.setLayerFrozen(layeredSketch, true) &&
                        !layerDocument.isObjectVisible(layeredObject) &&
                        !layerDocument.isObjectEditable(layeredObject) &&
                        !layerDocument.setLayerLineWeight(layeredSketch, 2.5),
                    "frozen layers must hide and protect their geometry and reject invalid lineweights");

    const QJsonObject serializedDocument = documentToJson(layerDocument);
    Document restoredLayerDocument;
    QString documentError;
    passed &= check(documentFromJson(serializedDocument,
                                     &restoredLayerDocument,
                                     &documentError) &&
                        documentError.isEmpty() &&
                        restoredLayerDocument.activeLayerId() == layeredConstruction &&
                        restoredLayerDocument.object(layeredObject) != nullptr &&
                        restoredLayerDocument.object(layeredObject)->layerId == layeredSketch &&
                        restoredLayerDocument.layer(layeredSketch) != nullptr &&
                        restoredLayerDocument.layer(layeredSketch)->name ==
                            QStringLiteral("Sketch Curves") &&
                        restoredLayerDocument.layer(layeredSketch)->color == sketchLayerColor &&
                        restoredLayerDocument.layer(layeredSketch)->lineType ==
                            QStringLiteral("Dashed") &&
                        qFuzzyCompare(restoredLayerDocument.layer(layeredSketch)->lineWeightMm + 1.0,
                                      1.35) &&
                        restoredLayerDocument.layer(layeredSketch)->description ==
                            QStringLiteral("Sketch geometry") &&
                        restoredLayerDocument.layer(layeredSketch)->frozen &&
                        !restoredLayerDocument.layer(layeredSketch)->plotted &&
                        restoredLayerDocument.layer(layeredSketch)->locked &&
                        restoredLayerDocument.settings() == persistedGridSettings,
                    "document serialization must preserve layers, object membership, and explicit unit/grid settings");

    QJsonObject versionThreeDocument = serializedDocument;
    versionThreeDocument.insert(QStringLiteral("version"), 3);
    Document restoredVersionThreeDocument;
    passed &= check(documentFromJson(versionThreeDocument,
                                     &restoredVersionThreeDocument,
                                     &documentError) &&
                        restoredVersionThreeDocument.settings() == DocumentSettings{},
                    "version-3 documents must load with the previous millimeter grid defaults");

    QJsonObject versionOneDocument = serializedDocument;
    versionOneDocument.insert(QStringLiteral("version"), 1);
    QJsonArray versionOneLayers;
    for (const QJsonValue &layerValue : serializedDocument.value(QStringLiteral("layers")).toArray()) {
        QJsonObject versionOneLayer = layerValue.toObject();
        versionOneLayer.remove(QStringLiteral("color"));
        versionOneLayers.append(versionOneLayer);
    }
    versionOneDocument.insert(QStringLiteral("layers"), versionOneLayers);
    Document restoredVersionOneDocument;
    passed &= check(documentFromJson(versionOneDocument,
                                     &restoredVersionOneDocument,
                                     &documentError) &&
                        restoredVersionOneDocument.layer(layeredSketch) != nullptr &&
                        restoredVersionOneDocument.layer(layeredSketch)->color ==
                            QColor(QStringLiteral("#d28b45")),
                    "version-1 drawings must load with the legacy default layer color");

    QJsonObject versionTwoDocument = serializedDocument;
    versionTwoDocument.insert(QStringLiteral("version"), 2);
    QJsonArray versionTwoLayers;
    for (const QJsonValue &layerValue : serializedDocument.value(QStringLiteral("layers")).toArray()) {
        QJsonObject versionTwoLayer = layerValue.toObject();
        versionTwoLayer.remove(QStringLiteral("lineType"));
        versionTwoLayer.remove(QStringLiteral("lineWeightMm"));
        versionTwoLayer.remove(QStringLiteral("description"));
        versionTwoLayer.remove(QStringLiteral("frozen"));
        versionTwoLayer.remove(QStringLiteral("plotted"));
        versionTwoLayers.append(versionTwoLayer);
    }
    versionTwoDocument.insert(QStringLiteral("layers"), versionTwoLayers);
    Document restoredVersionTwoDocument;
    passed &= check(documentFromJson(versionTwoDocument,
                                     &restoredVersionTwoDocument,
                                     &documentError) &&
                        restoredVersionTwoDocument.layer(layeredSketch) != nullptr &&
                        restoredVersionTwoDocument.layer(layeredSketch)->color ==
                            sketchLayerColor &&
                        restoredVersionTwoDocument.layer(layeredSketch)->lineType ==
                            QStringLiteral("Continuous") &&
                        !restoredVersionTwoDocument.layer(layeredSketch)->frozen,
                    "version-2 drawings must retain color and receive defaults for new layer properties");

    Document historyDocument;
    History history(historyDocument);
    history.record();
    const ObjectId historyObject = historyDocument.append(lineShape);
    passed &= check(history.canUndo() && history.undoCount() == 1,
                    "history must record the pre-edit document snapshot");
    passed &= check(history.undo() && historyDocument.isEmpty() && history.canRedo(),
                    "history undo must restore the document snapshot");
    passed &= check(history.redo() && historyDocument.indexOf(historyObject) == 0,
                    "history redo must restore object identity");
    history.record();
    historyDocument.replace(historyObject, circle);
    passed &= check(history.undo() &&
                        historyDocument.shape(historyObject) != nullptr &&
                        historyDocument.shape(historyObject)->geometryType == GeometryType::Line,
                    "history must restore geometry edits through the same path");
    history.record();
    DocumentSettings historyGridSettings;
    historyGridSettings.lengthUnit = DocumentLengthUnit::Inch;
    historyGridSettings.gridSpacing = 0.5;
    historyDocument.setSettings(historyGridSettings);
    passed &= check(history.undo() &&
                        historyDocument.settings() == DocumentSettings{} &&
                        history.redo() &&
                        historyDocument.settings() == historyGridSettings,
                    "document grid units and spacing must participate in undo and redo snapshots");

    const QSize viewportSize(640, 480);
    ViewportTransform viewportTransform;
    Shape depthLine = lineShape;
    depthLine.workPlane = WorkPlane::XZ;
    depthLine.workPlaneOffset = 7.0;
    Shape depthPoint;
    depthPoint.geometryType = GeometryType::Point;
    depthPoint.points = {QPointF(2.0, 3.0)};
    depthPoint.workPlane = WorkPlane::YZ;
    depthPoint.workPlaneOffset = 6.0;
    Shape depthPicture = picture;
    depthPicture.workPlane = WorkPlane::XZ;
    depthPicture.workPlaneOffset = 7.0;
    Shape depthDimension;
    depthDimension.geometryType = GeometryType::LinearDimension;
    depthDimension.points = {QPointF(0.0, 0.0),
                              QPointF(5.0, 0.0),
                              QPointF(2.5, 1.0)};
    const QVector<Shape> depthShapes{
        depthLine, depthPoint, depthPicture, depthDimension};
    const ViewportDepthGeometry depthGeometry =
        buildViewportDepthGeometry(depthShapes);
    const ViewportDepthGeometry linearDepthGeometry =
        buildViewportDepthGeometry(depthLine);
    const QByteArray depthGeometryKey =
        viewportDepthGeometryCacheKey(depthShapes);
    QVector<Shape> editedDepthShapes = depthShapes;
    editedDepthShapes[0].points[0].rx() += 1.0;
    bool lineDepthUsesShapePlane = depthGeometry.lineVertices.size() >= 2;
    for (const QVector3D &vertex : depthGeometry.lineVertices) {
        lineDepthUsesShapePlane &= std::abs(vertex.y() - 7.0f) < 1.0e-5f;
    }
    passed &= check(lineDepthUsesShapePlane &&
                        depthGeometry.pointVertices.size() == 1 &&
                        std::abs(depthGeometry.pointVertices.first().x() - 6.0f) < 1.0e-5f &&
                        std::abs(depthGeometry.pointVertices.first().y() - 2.0f) < 1.0e-5f &&
                        std::abs(depthGeometry.pointVertices.first().z() - 3.0f) < 1.0e-5f &&
                        depthGeometry.surfaceVertices.size() == 6 &&
                        std::abs(depthGeometry.surfaceVertices.first().y() - 7.0f) < 1.0e-5f &&
                        depthGeometryKey !=
                            viewportDepthGeometryCacheKey(editedDepthShapes),
                    "viewport depth geometry must follow each object's workplane and represent curves, point markers, and picture surfaces while excluding dimension annotations");
    passed &= check(linearDepthGeometry.lineVertices.size() == 2,
                    "degree-one NURBS scene strokes must emit only exact knot-span endpoints");

    Shape closeZoomCircle;
    closeZoomCircle.geometryType = GeometryType::Circle;
    closeZoomCircle.points = {QPointF(0.0, 0.0), QPointF(100.0, 0.0)};
    closeZoomCircle.nurbs = makeCircleNurbs(closeZoomCircle.points);
    const ViewportDepthGeometry closeZoomCircleGeometry =
        buildViewportDepthGeometry(closeZoomCircle);
    qreal maximumCloseZoomErrorPixels = 0.0;
    for (int index = 0;
         index + 1 < closeZoomCircleGeometry.lineVertices.size();
         index += 2) {
        const QVector3D &first = closeZoomCircleGeometry.lineVertices[index];
        const QVector3D &second = closeZoomCircleGeometry.lineVertices[index + 1];
        const qreal midpointX = (first.x() + second.x()) * 0.5;
        const qreal midpointY = (first.y() + second.y()) * 0.5;
        const qreal radialError = std::abs(
            100.0 - std::hypot(midpointX, midpointY));
        maximumCloseZoomErrorPixels =
            std::max(maximumCloseZoomErrorPixels, radialError * 12.0);
    }
    passed &= check(closeZoomCircleGeometry.lineVertices.size() >= 1024 &&
                        maximumCloseZoomErrorPixels < 0.1,
                    "GPU curve tessellation must keep an exact radius-100 NURBS circle below 0.1 viewport pixels of chord error at maximum orthographic zoom");

    const QPointF serviceWorldPoint(12.5, -7.25);
    const QPointF serviceScreenPoint =
        viewportTransform.worldToScreen(serviceWorldPoint, viewportSize);
    const QPointF roundTripWorld =
        viewportTransform.screenToWorld(serviceScreenPoint, viewportSize);
    passed &= check(std::hypot(roundTripWorld.x() - serviceWorldPoint.x(),
                               roundTripWorld.y() - serviceWorldPoint.y()) <= 1.0e-9,
                    "viewport transform must preserve world/screen round trips");

    ViewportTransform blenderViewTransform;
    const BlenderGridLevelSelection fixedAxisGridLevel =
        selectBlenderGridLevel(60.0, true);
    const BlenderGridLevelSelection closeAxisGridLevel =
        selectBlenderGridLevel(0.001, true);
    const BlenderGridLevelSelection perspectiveGridLevel =
        selectBlenderGridLevel(5.5, false);
    const BlenderGridLevelSelection belowPerspectiveBaseGridLevel =
        selectBlenderGridLevel(0.5, false);
    passed &= check(fixedAxisGridLevel.baseStep == 10.0 &&
                        fixedAxisGridLevel.baseStepIndex == 4 &&
                        std::abs(fixedAxisGridLevel.levelFraction - 50.0 / 90.0) < 1.0e-12 &&
                        closeAxisGridLevel.baseStep == 0.001 &&
                        closeAxisGridLevel.baseStepIndex == 0 &&
                        perspectiveGridLevel.baseStep == 1.0 &&
                        std::abs(perspectiveGridLevel.levelFraction - 0.5) < 1.0e-12 &&
                        belowPerspectiveBaseGridLevel.baseStep == 1.0 &&
                        belowPerspectiveBaseGridLevel.levelFraction < 0.0,
                    "Blender grid LOD must use its view-specific decimal steps and linear level blending");
    const BlenderGridLevelSelection inchGridLevel =
        selectBlenderGridLevel(300.0, false, 25.4);
    BlenderGridAppearance defaultGridAppearance;
    BlenderGridAppearance invalidGridAppearance = defaultGridAppearance;
    invalidGridAppearance.opacity = 2.5;
    passed &= check(std::abs(inchGridLevel.baseStep - 254.0) < 1.0e-10 &&
                        std::abs(blenderGridStepAtLevel(inchGridLevel, 0) - 25.4) <
                            1.0e-10 &&
                        isValidBlenderGridAppearance(defaultGridAppearance) &&
                        !isValidBlenderGridAppearance(invalidGridAppearance),
                    "grid LOD must scale from document units and reject invalid theme opacity values");
    passed &= check(isBlenderAxisAlignedView(ViewportViewPreset::Top) &&
                        isBlenderAxisAlignedView(ViewportViewPreset::Back) &&
                        !isBlenderAxisAlignedView(ViewportViewPreset::Isometric) &&
                        !isBlenderAxisAlignedView(ViewportViewPreset::Custom),
                    "only Blender's six cardinal presets may use fixed-axis orthographic grid subdivisions");

    const QVector<QPair<ViewportViewPreset, WorkPlane>> gridViewPlanes{
        {ViewportViewPreset::Top, WorkPlane::XY},
        {ViewportViewPreset::Bottom, WorkPlane::XY},
        {ViewportViewPreset::Front, WorkPlane::XZ},
        {ViewportViewPreset::Back, WorkPlane::XZ},
        {ViewportViewPreset::Right, WorkPlane::YZ},
        {ViewportViewPreset::Left, WorkPlane::YZ},
    };
    ViewportTransform gridFrameTransform;
    gridFrameTransform.setWorkPlane(WorkPlane::XZ, 9.0);
    for (const auto &viewPlane : gridViewPlanes) {
        gridFrameTransform.setViewPreset(viewPlane.first);
        const BlenderGridFrame frame = resolveBlenderGridFrame(
            gridFrameTransform, viewportSize);
        const bool xVisible = viewPlane.second != WorkPlane::YZ;
        const bool yVisible = viewPlane.second == WorkPlane::XY ||
                              viewPlane.second == WorkPlane::YZ;
        passed &= check(frame.plane == viewPlane.second &&
                            frame.planeOffset == 0.0 &&
                            frame.fixedAxisOrthographic &&
                            frame.visibleAxes[0] == xVisible &&
                            frame.visibleAxes[1] == yVisible &&
                            !frame.visibleAxes[2] &&
                            gridFrameTransform.workPlane() == WorkPlane::XZ &&
                            gridFrameTransform.workPlaneOffset() == 9.0,
                        "Blender cardinal views must select their matching visual plane and global in-plane axes without changing the CAD construction plane");
    }
    gridFrameTransform.setViewPreset(ViewportViewPreset::Top);
    gridFrameTransform.pan() = QPointF(14.0, -6.0);
    const BlenderGridFrame pannedGridFrame = resolveBlenderGridFrame(
        gridFrameTransform, viewportSize);
    passed &= check(std::abs(pannedGridFrame.cameraRelativeOffset.x() + 14.0) < 1.0e-9 &&
                        std::abs(pannedGridFrame.cameraRelativeOffset.y() - 6.0) < 1.0e-9,
                    "orthographic grid origin must track viewport pan in the displayed plane");

    ViewportTransform customOrthoGridTransform;
    customOrthoGridTransform.setWorkPlane(WorkPlane::XZ, 9.0);
    customOrthoGridTransform.setViewDirection({1.0, 0.0, 0.25});
    ViewportCameraState customOrthoCamera = customOrthoGridTransform.cameraState();
    customOrthoCamera.gridViewDistance = 37.0;
    customOrthoGridTransform.setCameraState(customOrthoCamera);
    const BlenderGridFrame customOrthoFrame = resolveBlenderGridFrame(
        customOrthoGridTransform, viewportSize);
    passed &= check(customOrthoGridTransform.viewPreset() == ViewportViewPreset::Custom &&
                        customOrthoFrame.plane == WorkPlane::XY &&
                        !customOrthoFrame.fixedAxisOrthographic &&
                        std::abs(customOrthoFrame.focusDistance - 37.0) < 1.0e-9 &&
                        customOrthoFrame.visibleAxes[0] &&
                        customOrthoFrame.visibleAxes[1] &&
                        !customOrthoFrame.visibleAxes[2] &&
                        customOrthoGridTransform.workPlane() == WorkPlane::XZ,
                    "free-angle orthographic views must use Blender's XY grid and independent grid distance while retaining the CAD workplane");
    customOrthoGridTransform.zoomAt(QPointF(viewportSize.width() * 0.5,
                                            viewportSize.height() * 0.5),
                                   2.0,
                                   viewportSize);
    const BlenderGridFrame zoomedCustomOrthoFrame = resolveBlenderGridFrame(
        customOrthoGridTransform, viewportSize);
    passed &= check(std::abs(zoomedCustomOrthoFrame.focusDistance - 18.5) < 1.0e-8,
                    "free-angle orthographic zoom must update Blender-style grid distance for LOD selection");

    ViewportTransform perspectiveGridFrameTransform;
    perspectiveGridFrameTransform.setViewPreset(ViewportViewPreset::Perspective);
    const BlenderGridFrame perspectiveGridFrame = resolveBlenderGridFrame(
        perspectiveGridFrameTransform, viewportSize);
    passed &= check(perspectiveGridFrame.plane == WorkPlane::XY &&
                        !perspectiveGridFrame.fixedAxisOrthographic &&
                        perspectiveGridFrame.focusDistance > 0.0 &&
                        perspectiveGridFrame.focusDistance <= 60.0 &&
                        perspectiveGridFrame.visibleAxes[0] &&
                        perspectiveGridFrame.visibleAxes[1] &&
                        !perspectiveGridFrame.visibleAxes[2],
                    "Blender perspective views must display the floor plane and global X/Y axes");

    const QVector<QPair<ViewportViewPreset, Point3D>> axisViews{
        {ViewportViewPreset::Top, {0.0, 0.0, 1.0}},
        {ViewportViewPreset::Bottom, {0.0, 0.0, -1.0}},
        {ViewportViewPreset::Front, {0.0, -1.0, 0.0}},
        {ViewportViewPreset::Back, {0.0, 1.0, 0.0}},
        {ViewportViewPreset::Right, {1.0, 0.0, 0.0}},
        {ViewportViewPreset::Left, {-1.0, 0.0, 0.0}},
    };
    for (const auto &axisView : axisViews) {
        blenderViewTransform.setViewPreset(axisView.first);
        const Point3D direction = blenderViewTransform.viewDirection();
        passed &= check(direction.x * axisView.second.x +
                                direction.y * axisView.second.y +
                                direction.z * axisView.second.z > 1.0 - 1.0e-8,
                        "Blender gizmo axis views must align the camera with all six world axes");
    }
    blenderViewTransform.setPerspectiveEnabled(true);
    blenderViewTransform.setViewDirection({1.0, 0.0, 0.0});
    passed &= check(!blenderViewTransform.isPerspectiveEnabled() &&
                        blenderViewTransform.viewPreset() == ViewportViewPreset::Right,
                    "selecting a Blender gizmo axis must enter its orthographic axis view");

    ViewportTransform customViewTransform;
    customViewTransform.zoom() = 2.5;
    customViewTransform.pan() = QPointF(12.0, -8.0);
    const ViewportCameraState beforeCustomPreset = customViewTransform.cameraState();
    customViewTransform.setViewPreset(ViewportViewPreset::Custom);
    const ViewportCameraState afterCustomPreset = customViewTransform.cameraState();
    passed &= check(afterCustomPreset.zoom == beforeCustomPreset.zoom &&
                        afterCustomPreset.pan == beforeCustomPreset.pan &&
                        afterCustomPreset.orbitPivot.x == beforeCustomPreset.orbitPivot.x &&
                        afterCustomPreset.orbitPivot.y == beforeCustomPreset.orbitPivot.y &&
                        afterCustomPreset.orbitPivot.z == beforeCustomPreset.orbitPivot.z,
                    "selecting the custom view must not alter the existing camera pose");

    blenderViewTransform.setViewPreset(ViewportViewPreset::Top);
    CurveHitTester blenderGizmoHitTester;
    ViewportRenderer blenderGizmoRenderer(blenderViewTransform,
                                          blenderGizmoHitTester);
    ViewportOverlay blenderGizmoOverlay(blenderGizmoRenderer,
                                        blenderViewTransform);
    const QPointF blenderGizmoCenter(viewportSize.width() - 50.0, 50.0);
    const BlenderNavigationHit centerAxisHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(blenderGizmoCenter,
                                                        viewportSize);
    const BlenderNavigationHit positiveXAxisHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(
            blenderGizmoCenter + QPointF(32.0, 0.0), viewportSize);
    const BlenderNavigationHit orbitHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(
            blenderGizmoCenter + QPointF(0.0, 18.0), viewportSize);
    const BlenderNavigationHit zoomHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(QPointF(618.0, 116.0),
                                                        viewportSize);
    const BlenderNavigationHit panHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(QPointF(618.0, 144.0),
                                                        viewportSize);
    const BlenderNavigationHit cameraHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(QPointF(618.0, 172.0),
                                                        viewportSize);
    const BlenderNavigationHit projectionHit =
        blenderGizmoOverlay.blenderNavigationGizmoHitAt(QPointF(618.0, 200.0),
                                                        viewportSize);
    passed &= check(centerAxisHit.action == BlenderNavigationAction::Axis &&
                        centerAxisHit.direction.z > 0.99 &&
                        positiveXAxisHit.action == BlenderNavigationAction::Axis &&
                        positiveXAxisHit.direction.x > 0.99 &&
                        orbitHit.action == BlenderNavigationAction::Orbit &&
                        zoomHit.action == BlenderNavigationAction::Zoom &&
                        panHit.action == BlenderNavigationAction::Pan &&
                        cameraHit.action == BlenderNavigationAction::Camera &&
                        projectionHit.action == BlenderNavigationAction::Projection,
                    "Blender navigation gizmo must hit its axis, orbit, zoom, pan, camera, and projection controls");

    blenderViewTransform.setViewDirection({-0.8, 0.6, 0.1});
    QImage gizmoTransparencyImage(viewportSize,
                                  QImage::Format_ARGB32_Premultiplied);
    gizmoTransparencyImage.fill(Qt::white);
    {
        QPainter gizmoPainter(&gizmoTransparencyImage);
        blenderGizmoOverlay.drawBlenderNavigationGizmo(
            gizmoPainter, viewportSize, QPointF(-1000.0, -1000.0));
    }
    const QColor gizmoIconCanvas(40, 40, 40);
    auto renderGizmoIcons = [&](bool perspectiveEnabled) {
        blenderViewTransform.setPerspectiveEnabled(perspectiveEnabled);
        QImage image(viewportSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(gizmoIconCanvas);
        QPainter painter(&image);
        blenderGizmoOverlay.drawBlenderNavigationGizmo(
            painter, viewportSize, QPointF(-1000.0, -1000.0));
        return image;
    };
    const QImage isoGizmoIconImage = renderGizmoIcons(false);
    const QImage perspectiveGizmoIconImage = renderGizmoIcons(true);
    const QRect panIconRect(viewportSize.width() - 36, 130, 28, 28);
    const QRect cameraIconRect(viewportSize.width() - 36, 158, 28, 28);
    const QRect projectionIconRect(viewportSize.width() - 36,
                                   186,
                                   28,
                                   28);
    auto hasIconPixels = [&](const QImage &image, const QRect &rect) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            for (int x = rect.left(); x <= rect.right(); ++x) {
                if (image.pixelColor(x, y) != gizmoIconCanvas) {
                    return true;
                }
            }
        }
        return false;
    };
    int brightestPanIconPixel = 0;
    for (int y = panIconRect.top(); y <= panIconRect.bottom(); ++y) {
        for (int x = panIconRect.left(); x <= panIconRect.right(); ++x) {
            brightestPanIconPixel = std::max(
                brightestPanIconPixel,
                isoGizmoIconImage.pixelColor(x, y).red());
        }
    }
    passed &= check(hasIconPixels(isoGizmoIconImage,
                                 QRect(viewportSize.width() - 36, 102, 28, 28)) &&
                        hasIconPixels(isoGizmoIconImage,
                                      QRect(viewportSize.width() - 36, 130, 28, 28)) &&
                        hasIconPixels(isoGizmoIconImage,
                                      QRect(viewportSize.width() - 36, 158, 28, 28)) &&
                        isoGizmoIconImage.copy(panIconRect) !=
                            isoGizmoIconImage.copy(cameraIconRect) &&
                        brightestPanIconPixel >= 170 &&
                        brightestPanIconPixel <= 200 &&
                        hasIconPixels(isoGizmoIconImage, projectionIconRect) &&
                        isoGizmoIconImage.copy(projectionIconRect) !=
                            perspectiveGizmoIconImage.copy(projectionIconRect),
                    "Blender navigation buttons must use their exact glyph artwork and switch between distinct perspective and iso grids");
    blenderViewTransform.setPerspectiveEnabled(false);

    const Point3D negativeXAxisDirection{-1.0, 0.0, 0.0};
    const ViewportDirectionProjection negativeXAxisProjection =
        blenderViewTransform.worldDirectionToView(negativeXAxisDirection);
    const QPointF gizmoCenter(viewportSize.width() - 50.0, 50.0);
    const QPointF negativeXAxisPosition =
        gizmoCenter + QPointF(negativeXAxisProjection.horizontal,
                              -negativeXAxisProjection.vertical) * 32.0;
    const QPointF negativeXAxisVector = negativeXAxisPosition - gizmoCenter;
    const qreal negativeXAxisVectorLength =
        std::hypot(negativeXAxisVector.x(), negativeXAxisVector.y());
    const QPointF negativeXAxisInteriorSample =
        negativeXAxisPosition +
        QPointF(-negativeXAxisVector.y() / negativeXAxisVectorLength,
               negativeXAxisVector.x() / negativeXAxisVectorLength) * 3.0;
    const QColor negativeMarkerCenter = gizmoTransparencyImage.pixelColor(
        negativeXAxisPosition.toPoint());
    const QColor negativeMarkerInterior = gizmoTransparencyImage.pixelColor(
        negativeXAxisInteriorSample.toPoint());
    const ViewportDirectionProjection positiveXAxisProjection =
        blenderViewTransform.worldDirectionToView({1.0, 0.0, 0.0});
    const QPointF positiveXAxisPosition =
        gizmoCenter + QPointF(positiveXAxisProjection.horizontal,
                              -positiveXAxisProjection.vertical) * 32.0;
    const QPointF positiveXAxisVector = positiveXAxisPosition - gizmoCenter;
    const qreal positiveXAxisVectorLength =
        std::hypot(positiveXAxisVector.x(), positiveXAxisVector.y());
    const QPointF positiveXAxisInteriorSample =
        positiveXAxisPosition +
        QPointF(-positiveXAxisVector.y() / positiveXAxisVectorLength,
               positiveXAxisVector.x() / positiveXAxisVectorLength) * 5.0;
    const QColor positiveMarkerInterior = gizmoTransparencyImage.pixelColor(
        positiveXAxisInteriorSample.toPoint());
    passed &= check(negativeXAxisProjection.towardCamera > 0.0 &&
                        positiveXAxisProjection.towardCamera < 0.0 &&
                        negativeMarkerCenter == QColor(Qt::white) &&
                        negativeMarkerInterior == QColor(Qt::white) &&
                        positiveMarkerInterior == QColor(QStringLiteral("#e75b61")),
                    "Blender axis marker fill must stay fixed to positive/negative axis signs as the camera orbits");

    ViewportTransform xzTransform;
    xzTransform.setWorkPlane(WorkPlane::XZ, 7.0);
    xzTransform.setViewPreset(ViewportViewPreset::Front);
    const QPointF xzPoint(14.0, -9.0);
    const QPointF xzScreen = xzTransform.worldToScreen(xzPoint, viewportSize);
    const QPointF xzRoundTrip = xzTransform.screenToWorld(xzScreen, viewportSize);
    passed &= check(std::hypot(xzRoundTrip.x() - xzPoint.x(),
                               xzRoundTrip.y() - xzPoint.y()) <= 1.0e-9,
                    "front view must project and pick points on the offset XZ workplane");
    xzTransform.setViewPreset(ViewportViewPreset::Isometric);
    const QPointF isoScreen = xzTransform.worldToScreen(xzPoint, viewportSize);
    const QPointF isoRoundTrip = xzTransform.screenToWorld(isoScreen, viewportSize);
    passed &= check(std::hypot(isoRoundTrip.x() - xzPoint.x(),
                               isoRoundTrip.y() - xzPoint.y()) <= 1.0e-8,
                    "isometric view must ray-pick back onto the active workplane");

    ViewportTransform perspectiveZoomTransform;
    const ViewportCameraPreferences blenderCameraDefaults =
        perspectiveZoomTransform.cameraPreferences();
    passed &= check(std::abs(blenderCameraDefaults.focalLengthMillimeters - 50.0) < 1.0e-9 &&
                        std::abs(blenderCameraDefaults.clipStart - 0.01) < 1.0e-9 &&
                        std::abs(blenderCameraDefaults.clipEnd - 1000.0) < 1.0e-9,
                    "viewport camera defaults must match the user's Blender lens and clipping values");
    ViewportTransform perspectiveDistanceTransform;
    perspectiveDistanceTransform.setViewPreset(ViewportViewPreset::Perspective);
    const Point3D perspectiveTarget = perspectiveDistanceTransform.viewTarget();
    const Point3D perspectiveEye =
        perspectiveDistanceTransform.cameraPosition(viewportSize);
    const Point3D largerViewportEye =
        perspectiveDistanceTransform.cameraPosition(QSize(1920, 1080));
    const qreal perspectiveDistance = std::hypot(
        std::hypot(perspectiveEye.x - perspectiveTarget.x,
                   perspectiveEye.y - perspectiveTarget.y),
        perspectiveEye.z - perspectiveTarget.z);
    const qreal largerViewportDistance = std::hypot(
        std::hypot(largerViewportEye.x - perspectiveTarget.x,
                   largerViewportEye.y - perspectiveTarget.y),
        largerViewportEye.z - perspectiveTarget.z);
    perspectiveDistanceTransform.zoom() = 0.15;
    QPointF zoomedOutTargetScreen;
    passed &= check(std::abs(perspectiveDistance - 60.0) < 1.0e-8 &&
                        std::abs(largerViewportDistance - perspectiveDistance) < 1.0e-8 &&
                        perspectiveDistanceTransform.worldPointToScreen(
                            perspectiveDistanceTransform.viewTarget(),
                            viewportSize,
                            &zoomedOutTargetScreen),
                    "perspective distance must use scene units independent of widget pixels and keep the view target inside the far clip while zooming out");
    ViewportTransform blenderProjectionTransform;
    blenderProjectionTransform.setViewPreset(ViewportViewPreset::Front);
    blenderProjectionTransform.setPerspectiveEnabled(true);
    QPointF oneUnitFromTargetScreen;
    passed &= check(blenderProjectionTransform.worldPointToScreen(
                        {1.0, 0.0, 0.0}, viewportSize,
                        &oneUnitFromTargetScreen) &&
                        std::abs(oneUnitFromTargetScreen.x() -
                                 viewportSize.width() * 0.5 -
                                 viewportSize.width() * 50.0 / (72.0 * 60.0)) <
                            1.0e-8,
                    "perspective projection must include Blender's 2x viewport zoom factor");
    const QPointF edgeOnCursor(500.0, 180.0);
    passed &= check(std::abs(viewportWheelStepsFromDeltas(120, 120) - 1.0) <
                            1.0e-12 &&
                        std::abs(viewportWheelStepsFromDeltas(-60, -60) + 0.5) <
                            1.0e-12 &&
                        std::abs(viewportWheelStepsFromDeltas(0, 40) - 1.0) <
                            1.0e-12,
                    "viewport wheel zoom must prefer notch deltas when Qt also reports pixel deltas, with pixel-only smooth scrolling as fallback");
    ViewportTransform combinedDeltaZoomTransform;
    const ViewportCameraState initialCombinedDeltaCamera =
        combinedDeltaZoomTransform.cameraState();
    const auto applyCombinedDeltaWheel = [&](int angleDeltaY, int pixelDeltaY) {
        const qreal wheelSteps =
            viewportWheelStepsFromDeltas(angleDeltaY, pixelDeltaY);
        const qreal factor = std::exp(std::log(1.2) * wheelSteps);
        combinedDeltaZoomTransform.zoomAt(QPointF(420.0, 160.0),
                                           factor,
                                           viewportSize);
    };
    for (int step = 0; step < 8; ++step) {
        applyCombinedDeltaWheel(-60, -60);
    }
    for (int step = 0; step < 8; ++step) {
        applyCombinedDeltaWheel(60, 60);
    }
    const ViewportCameraState returnedCombinedDeltaCamera =
        combinedDeltaZoomTransform.cameraState();
    passed &= check(std::abs(returnedCombinedDeltaCamera.zoom -
                             initialCombinedDeltaCamera.zoom) < 1.0e-12 &&
                        std::hypot(returnedCombinedDeltaCamera.pan.x() -
                                       initialCombinedDeltaCamera.pan.x(),
                                   returnedCombinedDeltaCamera.pan.y() -
                                       initialCombinedDeltaCamera.pan.y()) <
                            1.0e-10,
                    "eight combined-delta wheel steps out and back at an off-center cursor must return to the original camera without clamp-induced zoom or pan drift");
    ViewportTransform twentyStepZoomTransform;
    const QPointF twentyStepZoomCenter(viewportSize.width() * 0.5,
                                       viewportSize.height() * 0.5);
    for (int step = 0; step < 20; ++step) {
        twentyStepZoomTransform.zoomAt(twentyStepZoomCenter,
                                       1.0 / 1.2,
                                       viewportSize);
    }
    const ViewportCameraState twentyStepZoomOutState =
        twentyStepZoomTransform.cameraState();
    const qreal expectedZoomAfterTwentySteps = std::pow(1.2, -20.0);
    const qreal expectedDistanceAfterTwentySteps = 60.0 * std::pow(1.2, 20.0);
    const qreal blenderMinimumZoom = 60.0 / (1000.0 * 10.0);
    passed &= check(std::abs(twentyStepZoomOutState.zoom -
                             expectedZoomAfterTwentySteps) < 1.0e-12 &&
                        std::abs(twentyStepZoomOutState.gridViewDistance -
                                 expectedDistanceAfterTwentySteps) < 1.0e-8 &&
                        twentyStepZoomOutState.zoom > blenderMinimumZoom,
                    "twenty Blender wheel steps out must not hit the soft zoom limit with default clipping and grid spacing");
    for (int step = 0; step < 20; ++step) {
        twentyStepZoomTransform.zoomAt(twentyStepZoomCenter,
                                       1.2,
                                       viewportSize);
    }
    const ViewportCameraState returnedTwentyStepZoomState =
        twentyStepZoomTransform.cameraState();
    passed &= check(std::abs(returnedTwentyStepZoomState.zoom - 1.0) < 1.0e-12 &&
                        std::abs(returnedTwentyStepZoomState.gridViewDistance - 60.0) <
                            1.0e-8 &&
                        std::hypot(returnedTwentyStepZoomState.pan.x(),
                                   returnedTwentyStepZoomState.pan.y()) < 1.0e-10,
                    "twenty Blender wheel steps out and back must restore zoom, view distance, and centering");
    ViewportTransform blenderZoomLimitsTransform;
    blenderZoomLimitsTransform.zoomAt(twentyStepZoomCenter,
                                      1.0e-12,
                                      viewportSize);
    const ViewportCameraState zoomAtFarLimit =
        blenderZoomLimitsTransform.cameraState();
    blenderZoomLimitsTransform.zoomAt(twentyStepZoomCenter,
                                      1.0e12,
                                      viewportSize);
    const ViewportCameraState zoomAtNearLimit =
        blenderZoomLimitsTransform.cameraState();
    passed &= check(std::abs(zoomAtFarLimit.zoom - blenderMinimumZoom) < 1.0e-12 &&
                        std::abs(zoomAtFarLimit.gridViewDistance - 10000.0) < 1.0e-8 &&
                        std::abs(zoomAtNearLimit.zoom - 60000.0) < 1.0e-7 &&
                        std::abs(zoomAtNearLimit.gridViewDistance - 0.001) < 1.0e-12,
                    "viewport zoom limits must map Blender's grid-based near distance and clip-based far distance");
    const qreal targetPlanePixelSize = 60.0 /
        (viewportSize.width() * 50.0 / 72.0);
    const Point3D edgeOnAnchor{
        (edgeOnCursor.x() - viewportSize.width() * 0.5) *
            targetPlanePixelSize,
        0.0,
        (viewportSize.height() * 0.5 - edgeOnCursor.y()) *
            targetPlanePixelSize};
    blenderProjectionTransform.setWorkPlane(WorkPlane::XY);
    QPointF edgeOnPlanePick;
    passed &= check(!blenderProjectionTransform.screenToWorkPlane(
                        edgeOnCursor, viewportSize, WorkPlane::XY, 0.0,
                        &edgeOnPlanePick),
                    "edge-on construction plane must not provide a cursor anchor");
    blenderProjectionTransform.zoomAt(edgeOnCursor, 1.2, viewportSize);
    QPointF zoomedEdgeOnAnchor;
    passed &= check(blenderProjectionTransform.worldPointToScreen(
                        edgeOnAnchor, viewportSize, &zoomedEdgeOnAnchor) &&
                        std::hypot(zoomedEdgeOnAnchor.x() - edgeOnCursor.x(),
                                   zoomedEdgeOnAnchor.y() - edgeOnCursor.y()) <
                            1.0e-8,
                    "zoom to mouse must keep the target-depth point under the cursor when the CAD plane is edge-on");
    ViewportTransform farZoomTransform;
    farZoomTransform.setViewPreset(ViewportViewPreset::Perspective);
    farZoomTransform.zoomAt(QPointF(viewportSize.width() * 0.5,
                                   viewportSize.height() * 0.5),
                            0.03, viewportSize);
    const Point3D farZoomTarget = farZoomTransform.viewTarget();
    const Point3D farZoomEye = farZoomTransform.cameraPosition(viewportSize);
    const qreal farZoomDistance = std::hypot(
        std::hypot(farZoomEye.x - farZoomTarget.x,
                   farZoomEye.y - farZoomTarget.y),
        farZoomEye.z - farZoomTarget.z);
    farZoomTransform.zoomAt(QPointF(viewportSize.width() * 0.5,
                                   viewportSize.height() * 0.5),
                            1.0 / 0.03, viewportSize);
    QPointF returnedTargetScreen;
    passed &= check(std::abs(farZoomDistance - 2000.0) < 1.0e-7 &&
                        farZoomTransform.worldPointToScreen(
                            farZoomTransform.viewTarget(), viewportSize,
                            &returnedTargetScreen),
                    "Blender's distance range must allow far zoom and recover on zooming back");
    ViewportCameraPreferences extendedFarClip = blenderCameraDefaults;
    extendedFarClip.clipEnd = 3000.0;
    perspectiveZoomTransform.setCameraPreferences(extendedFarClip);
    perspectiveZoomTransform.setViewPreset(ViewportViewPreset::Front);
    perspectiveZoomTransform.setPerspectiveEnabled(true);
    perspectiveZoomTransform.zoom() = 1.0;
    QPointF nearPerspectiveScreen;
    QPointF farPerspectiveScreen;
    const bool projectedPerspectiveDepthPair =
        perspectiveZoomTransform.worldPointToScreen({100.0, 0.0, 0.0},
                                                    viewportSize,
                                                    &nearPerspectiveScreen) &&
        perspectiveZoomTransform.worldPointToScreen({100.0, 100.0, 0.0},
                                                    viewportSize,
                                                    &farPerspectiveScreen);
    const qreal viewportCenterX = viewportSize.width() / 2.0;
    const qreal unitZoomPerspectiveRatio =
        std::abs((nearPerspectiveScreen.x() - viewportCenterX) /
                 (farPerspectiveScreen.x() - viewportCenterX));
    perspectiveZoomTransform.zoom() = 0.5;
    const bool projectedZoomedOutDepthPair =
        perspectiveZoomTransform.worldPointToScreen({100.0, 0.0, 0.0},
                                                    viewportSize,
                                                    &nearPerspectiveScreen) &&
        perspectiveZoomTransform.worldPointToScreen({100.0, 100.0, 0.0},
                                                    viewportSize,
                                                    &farPerspectiveScreen);
    const qreal zoomedOutPerspectiveRatio =
        std::abs((nearPerspectiveScreen.x() - viewportCenterX) /
                 (farPerspectiveScreen.x() - viewportCenterX));
    passed &= check(projectedPerspectiveDepthPair &&
                        projectedZoomedOutDepthPair &&
                        zoomedOutPerspectiveRatio < unitZoomPerspectiveRatio,
                    "perspective zoom-out must move the camera back at a fixed Blender-like lens instead of widening the field of view");

    const Point3D cameraEye = perspectiveZoomTransform.cameraPosition(viewportSize);
    const Point3D cameraOut = perspectiveZoomTransform.viewDirection();
    const Point3D cameraForward{-cameraOut.x, -cameraOut.y, -cameraOut.z};
    const auto pointAlongView = [&cameraEye, &cameraForward](qreal depth) {
        return Point3D{cameraEye.x + cameraForward.x * depth,
                       cameraEye.y + cameraForward.y * depth,
                       cameraEye.z + cameraForward.z * depth};
    };
    QPointF clippedPoint;
    passed &= check(!perspectiveZoomTransform.worldPointToScreen(
                        pointAlongView(0.005), viewportSize, &clippedPoint) &&
                        !perspectiveZoomTransform.worldPointToScreen(
                            pointAlongView(3001.0), viewportSize, &clippedPoint),
                    "perspective camera clip start and clip end must reject points outside the clipping range");

    ViewportTransform focalLengthTransform;
    focalLengthTransform.setViewPreset(ViewportViewPreset::Front);
    focalLengthTransform.setPerspectiveEnabled(true);
    const Point3D eyeBeforeFocalChange = focalLengthTransform.cameraPosition(viewportSize);
    QPointF shortLensScreen;
    focalLengthTransform.worldPointToScreen({100.0, 0.0, 0.0},
                                            viewportSize,
                                            &shortLensScreen);
    ViewportCameraPreferences longLens = focalLengthTransform.cameraPreferences();
    longLens.focalLengthMillimeters = 100.0;
    focalLengthTransform.setCameraPreferences(longLens);
    QPointF longLensScreen;
    focalLengthTransform.worldPointToScreen({100.0, 0.0, 0.0},
                                            viewportSize,
                                            &longLensScreen);
    const Point3D eyeAfterFocalChange = focalLengthTransform.cameraPosition(viewportSize);
    passed &= check(std::abs(eyeBeforeFocalChange.x - eyeAfterFocalChange.x) < 1.0e-8 &&
                        std::abs(eyeBeforeFocalChange.y - eyeAfterFocalChange.y) < 1.0e-8 &&
                        std::abs(eyeBeforeFocalChange.z - eyeAfterFocalChange.z) < 1.0e-8 &&
                        longLensScreen.x() > shortLensScreen.x(),
                    "changing focal length must alter perspective framing without dollying the camera");

    ViewportTransform autoPerspectiveTransform;
    autoPerspectiveTransform.setViewPreset(ViewportViewPreset::Front);
    autoPerspectiveTransform.orbitByPixels(QPointF(1.0, 0.0));
    passed &= check(autoPerspectiveTransform.isPerspectiveEnabled(),
                    "Blender's auto-perspective preference must switch orthographic orbit to perspective");

    ViewportTransform fixedOrthoOrbitTransform;
    fixedOrthoOrbitTransform.setViewPreset(ViewportViewPreset::Front);
    ViewportNavigationPreferences fixedOrthoNavigation;
    fixedOrthoNavigation.autoPerspective = false;
    fixedOrthoOrbitTransform.setNavigationPreferences(fixedOrthoNavigation);
    fixedOrthoOrbitTransform.orbitByPixels(QPointF(1.0, 0.0));
    passed &= check(!fixedOrthoOrbitTransform.isPerspectiveEnabled(),
                    "disabling auto-perspective must preserve orthographic projection while orbiting");

    ViewportTransform scaleZoomTransform;
    scaleZoomTransform.setViewPreset(ViewportViewPreset::Front);
    scaleZoomTransform.setPerspectiveEnabled(true);
    ViewportNavigationPreferences scaleZoomPreferences;
    scaleZoomPreferences.zoomMethod = ViewportZoomMethod::Scale;
    scaleZoomTransform.setNavigationPreferences(scaleZoomPreferences);
    const Point3D eyeBeforeScaleZoom = scaleZoomTransform.cameraPosition(viewportSize);
    QPointF pointBeforeScaleZoom;
    scaleZoomTransform.worldPointToScreen({100.0, 0.0, 0.0},
                                          viewportSize,
                                          &pointBeforeScaleZoom);
    scaleZoomTransform.zoomAt(QPointF(viewportSize.width() * 0.5,
                                      viewportSize.height() * 0.5),
                              2.0,
                              viewportSize);
    const Point3D eyeAfterScaleZoom = scaleZoomTransform.cameraPosition(viewportSize);
    QPointF pointAfterScaleZoom;
    scaleZoomTransform.worldPointToScreen({100.0, 0.0, 0.0},
                                          viewportSize,
                                          &pointAfterScaleZoom);
    passed &= check(std::abs(scaleZoomTransform.cameraPreferences().focalLengthMillimeters -
                             100.0) < 1.0e-8 &&
                        std::hypot(eyeBeforeScaleZoom.x - eyeAfterScaleZoom.x,
                                   eyeBeforeScaleZoom.y - eyeAfterScaleZoom.y) < 1.0e-8 &&
                        std::abs(eyeBeforeScaleZoom.z - eyeAfterScaleZoom.z) < 1.0e-8 &&
                        pointAfterScaleZoom.x() > pointBeforeScaleZoom.x(),
                    "Scale zoom must change focal length while preserving camera distance");

    ViewportTransform perspectiveRoundTripTransform;
    ViewportCameraPreferences roundTripCamera =
        perspectiveRoundTripTransform.cameraPreferences();
    roundTripCamera.clipEnd = 3000.0;
    perspectiveRoundTripTransform.setCameraPreferences(roundTripCamera);
    perspectiveRoundTripTransform.setViewPreset(ViewportViewPreset::Isometric);
    perspectiveRoundTripTransform.setPerspectiveEnabled(true);
    perspectiveRoundTripTransform.zoom() = 0.65;
    const QPointF perspectiveWorldPoint(12.0, -8.0);
    const QPointF perspectiveScreenPoint =
        perspectiveRoundTripTransform.worldToScreen(perspectiveWorldPoint,
                                                    viewportSize);
    QPointF perspectiveRoundTripWorld;
    const bool perspectiveRoundTripSucceeded =
        perspectiveRoundTripTransform.screenToWorkPlane(
            perspectiveScreenPoint,
            viewportSize,
            WorkPlane::XY,
            0.0,
            &perspectiveRoundTripWorld);
    passed &= check(perspectiveRoundTripSucceeded &&
                        std::hypot(perspectiveRoundTripWorld.x() -
                                       perspectiveWorldPoint.x(),
                                   perspectiveRoundTripWorld.y() -
                                       perspectiveWorldPoint.y()) <= 1.0e-8,
                    "fixed-lens perspective projection and workplane picking must round-trip consistently");

    ViewportTransform perspectiveGridTransform;
    ViewportCameraState lowHorizonCamera;
    lowHorizonCamera.zoom = 1.0;
    lowHorizonCamera.yawRadians = 0.35;
    lowHorizonCamera.pitchRadians = 0.025;
    lowHorizonCamera.perspective = true;
    lowHorizonCamera.preset = ViewportViewPreset::Custom;
    perspectiveGridTransform.setCameraState(lowHorizonCamera);
    CurveHitTester perspectiveGridHitTester;
    ViewportRenderer perspectiveGridRenderer(perspectiveGridTransform,
                                             perspectiveGridHitTester);
    QImage perspectiveGridImage(viewportSize,
                                QImage::Format_ARGB32_Premultiplied);
    perspectiveGridImage.fill(QColor(QStringLiteral("#292929")));
    {
        QPainter painter(&perspectiveGridImage);
        perspectiveGridRenderer.drawGrid(painter, viewportSize);
    }
    auto averageLuminance = [&](const QRect &rect) {
        qint64 total = 0;
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            for (int x = rect.left(); x <= rect.right(); ++x) {
                total += qGray(perspectiveGridImage.pixel(x, y));
            }
        }
        return static_cast<qreal>(total) / rect.width() / rect.height();
    };
    const qreal horizonBandLuminance = averageLuminance(QRect(0, 0, 640, 80));
    const qreal foregroundBandLuminance =
        averageLuminance(QRect(0, 400, 640, 80));
    auto countGridPixels = [&](const QRect &rect) {
        int count = 0;
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            for (int x = rect.left(); x <= rect.right(); ++x) {
                if (qGray(perspectiveGridImage.pixel(x, y)) > 42) {
                    ++count;
                }
            }
        }
        return count;
    };
    const int horizonGridPixels = countGridPixels(QRect(0, 0, 640, 80));
    const int foregroundGridPixels = countGridPixels(QRect(0, 400, 640, 80));
    passed &= check(foregroundBandLuminance > horizonBandLuminance + 0.2,
                    "perspective floor grid must fade smoothly toward its horizon instead of staying uniformly bright");
    passed &= check(foregroundGridPixels > 100 &&
                        foregroundGridPixels > horizonGridPixels * 4,
                    "Blender-like perspective grid must remain visible in the foreground and fade out above the horizon at grazing camera angles");

    const QColor gridRegressionBackground(QStringLiteral("#292929"));
    const auto renderCpuGrid = [&](const ViewportTransform &transform,
                                   const BlenderGridAppearance &appearance =
                                       BlenderGridAppearance{},
                                   qreal baseGridStep = 1.0) {
        CurveHitTester hitTester;
        ViewportRenderer renderer(transform, hitTester);
        renderer.setGridAppearance(appearance);
        renderer.setGridBaseStep(baseGridStep);
        QImage image(viewportSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(gridRegressionBackground);
        QPainter painter(&image);
        renderer.drawGrid(painter, viewportSize);
        renderer.drawOrigin(painter, viewportSize);
        return image;
    };
    const auto changedGridPixelCount = [&](const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y) != gridRegressionBackground) {
                    ++count;
                }
            }
        }
        return count;
    };
    const QVector<ViewportViewPreset> blenderGridViewMatrix{
        ViewportViewPreset::Top,
        ViewportViewPreset::Bottom,
        ViewportViewPreset::Front,
        ViewportViewPreset::Back,
        ViewportViewPreset::Right,
        ViewportViewPreset::Left,
        ViewportViewPreset::Isometric,
    };
    bool allOrthographicViewsRenderGrid = true;
    for (const ViewportViewPreset preset : blenderGridViewMatrix) {
        ViewportTransform matrixTransform;
        matrixTransform.setViewPreset(preset);
        const QImage image = renderCpuGrid(matrixTransform);
        allOrthographicViewsRenderGrid &= changedGridPixelCount(image) > 100;
    }
    ViewportTransform perspectiveMatrixTransform;
    perspectiveMatrixTransform.setViewPreset(ViewportViewPreset::Isometric);
    perspectiveMatrixTransform.setPerspectiveEnabled(true);
    const QImage perspectiveMatrixImage = renderCpuGrid(perspectiveMatrixTransform);
    passed &= check(allOrthographicViewsRenderGrid &&
                        changedGridPixelCount(perspectiveMatrixImage) > 100,
                    "CPU fallback must render Blender grid and axis overlays in all six cardinal views, isometric ortho, and perspective");

    ViewportTransform topGridMatrixTransform;
    const QImage topGridMatrixImage = renderCpuGrid(topGridMatrixTransform);
    int redAxisPixels = 0;
    int greenAxisPixels = 0;
    for (int offset = -2; offset <= 2; ++offset) {
        const int y = viewportSize.height() / 2 + offset;
        const int x = viewportSize.width() / 2 + offset;
        for (int axisOffset = -80; axisOffset <= 80; ++axisOffset) {
            const QColor horizontal = topGridMatrixImage.pixelColor(
                viewportSize.width() / 2 + axisOffset, y);
            const QColor vertical = topGridMatrixImage.pixelColor(
                x, viewportSize.height() / 2 + axisOffset);
            redAxisPixels += horizontal.red() > horizontal.green() + 35 &&
                             horizontal.red() > horizontal.blue() + 35;
            greenAxisPixels += vertical.green() > vertical.red() + 35 &&
                               vertical.green() > vertical.blue() + 20;
        }
    }
    passed &= check(redAxisPixels > 40 && greenAxisPixels > 40,
                    "CPU grid fallback must preserve Blender red X and green Y axis colors in top view");

    ViewportTransform closeGridTransform;
    closeGridTransform.zoom() = 2.0;
    ViewportTransform farGridTransform;
    farGridTransform.zoom() = 0.5;
    const QImage closeGridImage = renderCpuGrid(closeGridTransform);
    const QImage farGridImage = renderCpuGrid(farGridTransform);
    ViewportTransform pannedGridTransform;
    pannedGridTransform.panByPixels(QPointF(47.0, -23.0), viewportSize);
    const QImage pannedGridImage = renderCpuGrid(pannedGridTransform);
    ViewportTransform orbitedGridTransform;
    orbitedGridTransform.orbitByPixels(QPointF(36.0, -20.0));
    const QImage orbitedGridImage = renderCpuGrid(orbitedGridTransform);
    passed &= check(closeGridImage != farGridImage &&
                        topGridMatrixImage != pannedGridImage &&
                        topGridMatrixImage != orbitedGridImage,
                    "CPU grid output must respond to close/far zoom, pan, and orbit camera navigation");

    xzTransform.setViewPreset(ViewportViewPreset::Front);
    QPointF edgeOnPick;
    passed &= check(!xzTransform.screenToWorkPlane(QPointF(320.0, 240.0),
                                                   viewportSize,
                                                   WorkPlane::XY,
                                                   0.0,
                                                   &edgeOnPick),
                    "a workplane viewed edge-on must report that screen points cannot be picked onto it");
    const Point3D xzWorldPoint = workPlanePointToWorld(xzPoint, WorkPlane::XZ, 7.0);
    passed &= check(xzWorldPoint.x == xzPoint.x() && xzWorldPoint.y == 7.0 &&
                        xzWorldPoint.z == xzPoint.y() &&
                        worldPointToWorkPlane(xzWorldPoint, WorkPlane::XZ) == xzPoint,
                    "2D NURBS coordinates must lift consistently into world XYZ on XZ");

    Document serviceDocument;
    serviceDocument.append(lineShape);
    CurveSampler sampler;
    SampledNurbsCurve2D sampled;
    passed &= check(sampler.sampleNurbsCurve(line,
                                             viewportTransform,
                                             viewportSize,
                                             &sampled) &&
                        sampled.parameters.size() >= 2 &&
                        sampled.parameters.size() == sampled.screenPoints.size(),
                    "curve sampler must sample the stored NURBS curve");
    const QVector<EraseCurveSampleCache> sampledScene =
        sampler.sampleDocument(serviceDocument, viewportTransform, viewportSize);
    passed &= check(sampledScene.size() == 1 &&
                        sampledScene.first().shapeIndex == 0 &&
                        sampledScene.first().componentIndex == 0,
                    "curve sampler must build erase caches from document geometry");

    CurveHitTester hitTester;
    const QPointF lineScreenPoint =
        viewportTransform.worldToScreen(QPointF(5.0, 0.0), viewportSize);
    passed &= check(hitTester.hitTestShape(serviceDocument,
                                           lineScreenPoint,
                                           viewportTransform,
                                           viewportSize) == 0,
                    "curve hit tester must hit committed NURBS geometry");

    Shape xzLine = lineShape;
    xzLine.workPlane = WorkPlane::XZ;
    Document xzDocument;
    xzDocument.append(xzLine);
    passed &= check(hitTester.hitTestShape(xzDocument,
                                           viewportTransform.worldToScreen(
                                               QPointF(5.0, 0.0), viewportSize),
                                           viewportTransform,
                                           viewportSize) == -1,
                    "XY-plane selection must not treat an XZ curve as local XY geometry");
    ViewportTransform frontTransform;
    frontTransform.setWorkPlane(WorkPlane::XZ);
    frontTransform.setViewPreset(ViewportViewPreset::Front);
    const QPointF frontLineScreen = frontTransform.worldToScreen(QPointF(5.0, 0.0),
                                                                 viewportSize);
    passed &= check(hitTester.hitTestShape(xzDocument,
                                           frontLineScreen,
                                           frontTransform,
                                           viewportSize) == 0,
                    "front-view selection must hit curves on the active XZ workplane");
    xzLine.workPlaneOffset = 7.0;
    Document depthDocument;
    depthDocument.append(xzLine);
    ViewportTransform depthTransform;
    depthTransform.setViewPreset(ViewportViewPreset::Front);
    depthTransform.setPerspectiveEnabled(true);
    const QPointF depthScreen = depthTransform.workPlaneToScreen(
        QPointF(5.0, 0.0), viewportSize, WorkPlane::XZ, 7.0);
    Point3D pickedDepth;
    const bool depthHit = hitTester.hitTestVisibleDepth(
        depthDocument, depthScreen, depthTransform, viewportSize, &pickedDepth);
    passed &= check(depthHit && std::abs(pickedDepth.x - 5.0) < 1.0e-6 &&
                        std::abs(pickedDepth.y - 7.0) < 1.0e-6 &&
                        std::abs(pickedDepth.z) < 1.0e-6,
                    "mouse-depth orbit must pick visible geometry on its own offset plane, not the active drawing plane");
    Shape foregroundLine = xzLine;
    foregroundLine.workPlaneOffset = -7.0;
    depthDocument.append(foregroundLine);
    Point3D frontmostDepth;
    const QPointF overlapScreen = depthTransform.workPlaneToScreen(
        QPointF(0.0, 0.0), viewportSize, WorkPlane::XZ, 7.0);
    passed &= check(hitTester.hitTestVisibleDepth(
                        depthDocument, overlapScreen, depthTransform, viewportSize,
                        &frontmostDepth) &&
                        std::abs(frontmostDepth.y + 7.0) < 1.0e-6,
                    "overlapping orbit targets must choose the nearest visible stroke depth");
    const Point3D eyeBeforePivot = depthTransform.cameraPosition(viewportSize);
    if (depthHit) {
        depthTransform.setOrbitPivotPreservingView(pickedDepth);
    }
    const Point3D eyeAfterPivot = depthTransform.cameraPosition(viewportSize);
    passed &= check(std::abs(eyeAfterPivot.x - eyeBeforePivot.x) < 1.0e-5 &&
                        std::abs(eyeAfterPivot.y - eyeBeforePivot.y) < 1.0e-5 &&
                        std::abs(eyeAfterPivot.z - eyeBeforePivot.z) < 1.0e-5,
                    "choosing an off-plane orbit pivot must not jump the perspective camera");
    ViewportTransform poleOrbit;
    poleOrbit.setViewPreset(ViewportViewPreset::Front);
    poleOrbit.orbitByPixels(QPointF(0.0, 300.0));
    passed &= check(poleOrbit.viewDirection().z > 0.0 &&
                        poleOrbit.viewDirection().y > 0.0 &&
                        std::abs(poleOrbit.viewUp().z) > 0.1,
                    "quaternion turntable orbit must rotate through the pole without a pitch clamp");

    ViewportNavigationPreferences trackballPreferences;
    trackballPreferences.autoPerspective = false;
    trackballPreferences.orbitMethod = ViewportOrbitMethod::Trackball;
    trackballPreferences.trackballSensitivity = 1.0;
    const QSize trackballViewportSize(800, 600);
    const QPointF trackballStart(400.0, 300.0);
    const QPointF trackballEnd(520.0, 375.0);
    const auto orientationDistance = [](const ViewportOrientation &first,
                                        const ViewportOrientation &second) {
        const qreal cosine = std::clamp(std::abs(first.dot(second)), 0.0, 1.0);
        return 2.0 * std::acos(cosine);
    };
    ViewportTransform fullTrackballOrbit;
    fullTrackballOrbit.setViewPreset(ViewportViewPreset::Front);
    fullTrackballOrbit.setNavigationPreferences(trackballPreferences);
    const ViewportOrientation trackballInitialOrientation =
        fullTrackballOrbit.cameraState().orientation;
    fullTrackballOrbit.beginOrbitGesture(trackballStart, trackballViewportSize);
    fullTrackballOrbit.orbitToPosition(trackballEnd, trackballViewportSize);
    const qreal fullTrackballAngle = orientationDistance(
        trackballInitialOrientation,
        fullTrackballOrbit.cameraState().orientation);
    const qreal trackballRadius = 1.1;
    const qreal sphereX = 120.0 / 300.0;
    const qreal sphereY = -75.0 / 300.0;
    const qreal sphereZ = std::sqrt(trackballRadius * trackballRadius -
                                    sphereX * sphereX - sphereY * sphereY);
    const qreal sphereDragLength = std::sqrt(
        sphereX * sphereX + sphereY * sphereY +
        (sphereZ - trackballRadius) * (sphereZ - trackballRadius));
    const qreal expectedSphereAngle = sphereDragLength *
        (3.14159265358979323846 / (2.0 * trackballRadius));
    passed &= check(std::abs(fullTrackballAngle - expectedSphereAngle) < 1.0e-8,
                    "Blender trackball sphere mapping must use drag distance divided by twice the trackball radius");

    ViewportTransform halfTrackballOrbit;
    halfTrackballOrbit.setViewPreset(ViewportViewPreset::Front);
    trackballPreferences.trackballSensitivity = 0.5;
    halfTrackballOrbit.setNavigationPreferences(trackballPreferences);
    halfTrackballOrbit.beginOrbitGesture(trackballStart, trackballViewportSize);
    halfTrackballOrbit.orbitToPosition(trackballEnd, trackballViewportSize);
    // Compare both results from the same initial pose; the half-sensitivity
    // path must rotate by half the full-sensitivity trackball angle.
    const qreal measuredHalfTrackballAngle = orientationDistance(
        trackballInitialOrientation,
        halfTrackballOrbit.cameraState().orientation);
    passed &= check(fullTrackballAngle > 0.0 &&
                        std::abs(measuredHalfTrackballAngle / fullTrackballAngle - 0.5) <
                            1.0e-8 &&
                        halfTrackballOrbit.viewPreset() == ViewportViewPreset::Custom,
                    "Blender trackball sensitivity must scale the drag angle from its initial view pose");

    trackballPreferences.trackballSensitivity = 1.0;
    const auto trackballDragAngle = [&](const QPointF &endPosition) {
        ViewportTransform transform;
        transform.setViewPreset(ViewportViewPreset::Front);
        transform.setNavigationPreferences(trackballPreferences);
        const ViewportOrientation startOrientation = transform.cameraState().orientation;
        transform.beginOrbitGesture(trackballStart, trackballViewportSize);
        transform.orbitToPosition(endPosition, trackballViewportSize);
        return orientationDistance(startOrientation,
                                  transform.cameraState().orientation);
    };
    const qreal horizontalTrackballAngle =
        trackballDragAngle(trackballStart + QPointF(120.0, 0.0));
    const qreal verticalTrackballAngle =
        trackballDragAngle(trackballStart + QPointF(0.0, 120.0));
    passed &= check(std::abs(horizontalTrackballAngle - verticalTrackballAngle) <
                        1.0e-8,
                    "Blender trackball mapping must aspect-correct horizontal and vertical cursor travel");
    const qreal hyperbolaZ = (trackballRadius * trackballRadius / 2.0) / 2.0;
    const qreal hyperbolaDragLength =
        std::sqrt(2.0 * 2.0 +
                  (hyperbolaZ - trackballRadius) *
                      (hyperbolaZ - trackballRadius));
    const qreal wrappedHyperbolaAngle = std::abs(std::remainder(
        hyperbolaDragLength *
            (3.14159265358979323846 / (2.0 * trackballRadius)),
        2.0 * 3.14159265358979323846));
    passed &= check(std::abs(trackballDragAngle(QPointF(1000.0, 300.0)) -
                             wrappedHyperbolaAngle) < 1.0e-8,
                    "Blender trackball must continue over the hyperbola outside the virtual sphere");

    ViewportTransform absoluteTrackballOrbit;
    absoluteTrackballOrbit.setViewPreset(ViewportViewPreset::Front);
    absoluteTrackballOrbit.setNavigationPreferences(trackballPreferences);
    absoluteTrackballOrbit.beginOrbitGesture(trackballStart, trackballViewportSize);
    absoluteTrackballOrbit.orbitToPosition(QPointF(455.0, 335.0),
                                          trackballViewportSize);
    absoluteTrackballOrbit.orbitToPosition(trackballEnd, trackballViewportSize);
    ViewportTransform directTrackballOrbit;
    directTrackballOrbit.setViewPreset(ViewportViewPreset::Front);
    directTrackballOrbit.setNavigationPreferences(trackballPreferences);
    directTrackballOrbit.beginOrbitGesture(trackballStart, trackballViewportSize);
    directTrackballOrbit.orbitToPosition(trackballEnd, trackballViewportSize);
    passed &= check(std::abs(absoluteTrackballOrbit.cameraState().orientation.dot(
                                 directTrackballOrbit.cameraState().orientation)) >
                        1.0 - 1.0e-12,
                    "Blender trackball motion must be computed from the drag-start cursor and orientation, not accumulated deltas");

    ViewportNavigationPreferences savedTrackballPreferences =
        fullTrackballOrbit.navigationPreferences();
    passed &= check(savedTrackballPreferences.orbitMethod ==
                            ViewportOrbitMethod::Trackball &&
                        std::abs(savedTrackballPreferences.trackballSensitivity - 1.0) <
                            1.0e-12,
                    "viewport navigation preferences must retain the selected trackball mode and sensitivity");

    passed &= check(sampler.sampleDocument(xzDocument,
                                           viewportTransform,
                                           viewportSize).isEmpty() &&
                        sampler.sampleDocument(xzDocument,
                                               frontTransform,
                                               viewportSize).size() == 1,
                    "erase sampling must only include curves on the active workplane");
    SnapEngine planeSnapEngine;
    SnapSettings planeSnapSettings;
    planeSnapSettings.enabled = true;
    planeSnapSettings.midpoint = false;
    planeSnapSettings.intersection = false;
    planeSnapSettings.center = false;
    planeSnapEngine.setSettings(planeSnapSettings);
    passed &= check(planeSnapEngine.snapCandidatesForScene(xzDocument,
                                                           {},
                                                           viewportTransform,
                                                           viewportSize).isEmpty() &&
                        !planeSnapEngine.snapCandidatesForScene(xzDocument,
                                                                {},
                                                                frontTransform,
                                                                viewportSize).isEmpty(),
                    "object snaps must target only curves on the active workplane");

    Document pictureDocument;
    pictureDocument.append(picture);
    const QPointF pictureCenter =
        viewportTransform.worldToScreen(QPointF(7.5, 6.75), viewportSize);
    passed &= check(hitTester.hitTestShape(pictureDocument,
                                           pictureCenter,
                                           viewportTransform,
                                           viewportSize) == 0 &&
                        hitTester.controlPointsForShape(picture).size() == 4,
                    "a placed Picture must be selectable throughout its image area with four frame controls");
    QImage renderedPicture(viewportSize, QImage::Format_ARGB32);
    renderedPicture.fill(QColor(QStringLiteral("#202020")));
    ViewportRenderer pictureRenderer(viewportTransform, hitTester);
    {
        QPainter picturePainter(&renderedPicture);
        pictureRenderer.drawShape(picturePainter,
                                  picture,
                                  viewportSize,
                                  false,
                                  false,
                                  false,
                                  QColor(QStringLiteral("#ffffff")));
    }
    const QPoint picturePixel(qRound(pictureCenter.x()), qRound(pictureCenter.y()));
    passed &= check(renderedPicture.pixelColor(picturePixel) ==
                        picture.pictureImage.pixelColor(0, 0),
                    "the viewport renderer must draw the embedded image inside its placed frame");
    Shape tracingLine{GeometryType::Line,
                      {QPointF(5.0, 6.75), QPointF(10.0, 6.75)},
                      makeDegreeOneNurbs({QPointF(5.0, 6.75), QPointF(10.0, 6.75)})};
    pictureDocument.append(tracingLine);
    passed &= check(hitTester.hitTestShape(pictureDocument,
                                           pictureCenter,
                                           viewportTransform,
                                           viewportSize) == 1,
                    "curves drawn over a Picture must remain selectable instead of being masked by the image face");
    SnapEngine pictureSnapEngine;
    const QVector<SnapCandidate> pictureSnaps = pictureSnapEngine.snapCandidatesForShape(
        picture, viewportTransform, viewportSize);
    passed &= check(std::count_if(pictureSnaps.cbegin(),
                                  pictureSnaps.cend(),
                                  [](const SnapCandidate &candidate) {
                                      return candidate.type == SnapType::Endpoint;
                                  }) == 4 &&
                        std::count_if(pictureSnaps.cbegin(),
                                      pictureSnaps.cend(),
                                      [](const SnapCandidate &candidate) {
                                          return candidate.type == SnapType::Midpoint;
                                      }) == 4,
                    "Picture frame corners and side midpoints must participate in object snaps");
    const DimensionScreenLayout linearDimensionLayout =
        buildDimensionScreenLayout(linearDimension, viewportTransform, viewportSize);
    const DimensionScreenLayout angularDimensionLayout =
        buildDimensionScreenLayout(angularDimension, viewportTransform, viewportSize);
    const DimensionScreenLayout architecturalDimensionLayout =
        buildDimensionScreenLayout(linearDimension,
                                   viewportTransform,
                                   viewportSize,
                                   DimensionFontStyle::Architectural);
    const QFont architecturalFont =
        dimensionAnnotationFont(DimensionFontStyle::Architectural);
    passed &= check(linearDimensionLayout.valid &&
                        linearDimensionLayout.label.endsWith(QStringLiteral(" mm")) &&
                        angularDimensionLayout.valid &&
                        angularDimensionLayout.label == QStringLiteral("90°") &&
                        architecturalDimensionLayout.valid &&
                        architecturalDimensionLayout.label == linearDimensionLayout.label &&
                        architecturalDimensionLayout.labelBounds.width() >
                            linearDimensionLayout.labelBounds.width() &&
                        architecturalFont.family() == QStringLiteral("Architects Daughter"),
                    "dimension layout must display linear lengths and included angles");
    Document dimensionDocument;
    dimensionDocument.append(linearDimension);
    const QPointF dimensionLabelPoint = linearDimensionLayout.labelCenter;
    passed &= check(hitTester.hitTestShape(dimensionDocument,
                                           dimensionLabelPoint,
                                           viewportTransform,
                                           viewportSize) == 0 &&
                        hitTester.controlPointsForShape(linearDimension).isEmpty(),
                    "dimension annotations must be selectable by their labels without exposing annotation anchors as control points");
    SnapEngine dimensionSnapEngine;
    dimensionSnapEngine.setSettings(SnapSettings{true, true, true, true, true,
                                                  true, true, true, true});
    passed &= check(dimensionSnapEngine.snapCandidatesForScene(dimensionDocument,
                                                                {},
                                                                viewportTransform,
                                                                viewportSize)
                            .isEmpty(),
                    "dimension annotation anchors must not become geometry object snaps");
    int hitShapeIndex = -1;
    int hitControlPointIndex = -1;
    passed &= check(hitTester.hitTestSelectedControlPoint(
                        serviceDocument,
                        {0},
                        viewportTransform.worldToScreen(QPointF(0.0, 0.0),
                                                        viewportSize),
                        viewportTransform,
                        viewportSize,
                        &hitShapeIndex,
                        &hitControlPointIndex) &&
                        hitShapeIndex == 0 && hitControlPointIndex == 0,
                    "control-point hit tester must use selected curve control points");

    SnapEngine snapEngine;
    snapEngine.setSettings(SnapSettings{true, true, true, true, true, false, false});
    const SnapResult endpointSnap = snapEngine.findSnapPoint(
        serviceDocument,
        QPointF(0.25, 0.0),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(endpointSnap.type == SnapType::Endpoint &&
                        std::hypot(endpointSnap.point.x(), endpointSnap.point.y()) <= 1.0e-9,
                    "snap engine must select the nearest enabled endpoint");

    Document polygonSnapDocument;
    polygonSnapDocument.append(polygonShape);
    const QVector<SnapCandidate> polygonCandidates = snapEngine.snapCandidatesForShape(
        polygonShape,
        viewportTransform,
        viewportSize);
    int polygonEndpointCount = 0;
    int polygonMidpointCount = 0;
    for (const SnapCandidate &candidate : polygonCandidates) {
        polygonEndpointCount += candidate.type == SnapType::Endpoint ? 1 : 0;
        polygonMidpointCount += candidate.type == SnapType::Midpoint ? 1 : 0;
    }
    SnapEngine polygonEndpointEngine;
    polygonEndpointEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false});
    const SnapResult polygonEndpointSnap = polygonEndpointEngine.findSnapPoint(
        polygonSnapDocument,
        polygonShape.points.first() + QPointF(0.1, 0.1),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(polygonEndpointCount == polygonShape.points.size() &&
                        polygonMidpointCount == polygonShape.points.size() &&
                        polygonEndpointSnap.type == SnapType::Endpoint &&
                        pointsAlmostEqual(polygonEndpointSnap.point,
                                          polygonShape.points.first()),
                    "polygon OSnap must expose every corner and side midpoint and snap to its corners");

    Document ellipseSnapDocument;
    ellipseSnapDocument.append(ellipse);
    SnapEngine ellipseSnapEngine;
    ellipseSnapEngine.setSettings(
        SnapSettings{true, true, false, false, true, false, false});
    const SnapResult ellipseEndpointSnap = ellipseSnapEngine.findSnapPoint(
        ellipseSnapDocument,
        QPointF(5.1, 0.1),
        true,
        {},
        viewportTransform,
        viewportSize);
    const SnapResult ellipseCenterSnap = ellipseSnapEngine.findSnapPoint(
        ellipseSnapDocument,
        QPointF(0.1, 0.1),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(ellipseEndpointSnap.type == SnapType::Endpoint &&
                        pointsAlmostEqual(ellipseEndpointSnap.point, QPointF(5.0, 0.0)) &&
                        ellipseCenterSnap.type == SnapType::Center &&
                        pointsAlmostEqual(ellipseCenterSnap.point, QPointF(0.0, 0.0)),
                    "ellipse OSnap must expose its axis endpoints and construction center");

    Shape nurbsCircleShape{GeometryType::Nurbs,
                           {},
                           circle.nurbs,
                           ArcMode::TwoPoint,
                           0.0,
                           {},
                           {}};
    const QPointF nurbsCircleTangentOrigin(20.0, 7.0);
    const QVector<SnapCandidate> nurbsCircleTangents = snapEngine.tangentCandidatesForShape(
        nurbsCircleShape,
        nurbsCircleTangentOrigin,
        viewportTransform,
        viewportSize);
    bool nurbsTangenciesAreValid = nurbsCircleTangents.size() == 2;
    for (const SnapCandidate &candidate : nurbsCircleTangents) {
        const QPointF radial = candidate.point;
        const QPointF lineDirection = nurbsCircleTangentOrigin - candidate.point;
        const qreal normalizedDot = std::abs(QPointF::dotProduct(radial, lineDirection)) /
                                    (10.0 * std::hypot(lineDirection.x(), lineDirection.y()));
        nurbsTangenciesAreValid &= candidate.type == SnapType::Tangent &&
                                   std::abs(std::hypot(radial.x(), radial.y()) - 10.0) <=
                                       1.0e-7 &&
                                   normalizedDot <= 1.0e-7;
    }
    passed &= check(nurbsTangenciesAreValid,
                    "NURBS tangencies must lie on the curve with the line perpendicular to its derivative");

    const QPointF editedCircleCenter(30.0, 40.0);
    const QPointF editedCircleEdge(35.0, 40.0);
    const Shape editedCircleShape{GeometryType::Circle,
                                  {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                                  makeCircleNurbs({editedCircleCenter, editedCircleEdge}),
                                  ArcMode::TwoPoint,
                                  0.0,
                                  {},
                                  {}};
    const QPointF editedCircleOrigin(50.0, 47.0);
    const QVector<SnapCandidate> editedCircleTangents = snapEngine.tangentCandidatesForShape(
        editedCircleShape,
        editedCircleOrigin,
        viewportTransform,
        viewportSize);
    bool editedCircleTangenciesUseVisibleCurve = editedCircleTangents.size() == 2;
    for (const SnapCandidate &candidate : editedCircleTangents) {
        const QPointF radial = candidate.point - editedCircleCenter;
        const QPointF tangent = editedCircleOrigin - candidate.point;
        const qreal radius = std::hypot(radial.x(), radial.y());
        const qreal tangentLength = std::hypot(tangent.x(), tangent.y());
        editedCircleTangenciesUseVisibleCurve &=
            candidate.type == SnapType::Tangent &&
            std::abs(radius - 5.0) <= 1.0e-7 && tangentLength > 1.0e-12 &&
            std::abs(QPointF::dotProduct(radial, tangent)) /
                    (radius * tangentLength) <=
                1.0e-7;
    }
    passed &= check(editedCircleTangenciesUseVisibleCurve,
                    "edited circles must calculate tangencies from stored NURBS, not stale construction points");

    const Shape editedArcShape{GeometryType::Arc,
                               {QPointF(0.0, 0.0),
                                QPointF(10.0, 0.0),
                                QPointF(0.0, 10.0)},
                               editedCircleShape.nurbs,
                               ArcMode::TwoPoint,
                               0.0,
                               {},
                               {}};
    const QVector<SnapCandidate> editedArcTangents = snapEngine.tangentCandidatesForShape(
        editedArcShape,
        editedCircleOrigin,
        viewportTransform,
        viewportSize);
    bool editedArcTangenciesUseVisibleCurve = editedArcTangents.size() == 2;
    for (const SnapCandidate &candidate : editedArcTangents) {
        const QPointF radial = candidate.point - editedCircleCenter;
        const QPointF tangent = editedCircleOrigin - candidate.point;
        const qreal radius = std::hypot(radial.x(), radial.y());
        const qreal tangentLength = std::hypot(tangent.x(), tangent.y());
        editedArcTangenciesUseVisibleCurve &=
            candidate.type == SnapType::Tangent &&
            std::abs(radius - 5.0) <= 1.0e-7 && tangentLength > 1.0e-12 &&
            std::abs(QPointF::dotProduct(radial, tangent)) /
                    (radius * tangentLength) <=
                1.0e-7;
    }
    passed &= check(editedArcTangenciesUseVisibleCurve,
                    "edited arcs must calculate tangencies from stored NURBS, not stale construction points");

    const QVector<QPointF> archControlPoints{QPointF(0.0, 0.0),
                                              QPointF(0.0, 100.0),
                                              QPointF(100.0, 100.0),
                                              QPointF(100.0, 0.0)};
    const Shape::NurbsCurve2D archCurve = makeBezierNurbs(archControlPoints);
    const Shape archShape{GeometryType::Bezier,
                          archControlPoints,
                          archCurve,
                          ArcMode::TwoPoint,
                          0.0,
                          {},
                          {}};
    const QPointF archOrigin(50.0, 130.0);
    const QVector<SnapCandidate> archTangents = snapEngine.tangentCandidatesForShape(
        archShape, archOrigin, viewportTransform, viewportSize);
    qreal archStartParameter = 0.0;
    qreal archEndParameter = 0.0;
    bool freeformTangenciesAreValid = !archTangents.isEmpty() &&
        nurbsParameterDomain(archCurve, &archStartParameter, &archEndParameter);
    for (const SnapCandidate &candidate : archTangents) {
        qreal low = archStartParameter;
        qreal high = archEndParameter;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const qreal middle = (low + high) * 0.5;
            QPointF point;
            if (!evaluateNurbsPoint(archCurve, middle, &point)) {
                freeformTangenciesAreValid = false;
                break;
            }
            if (point.x() < candidate.point.x()) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const qreal parameter = (low + high) * 0.5;
        QPointF point;
        QPointF derivative;
        if (!evaluateNurbsPoint(archCurve, parameter, &point) ||
            !evaluateNurbsDerivative(archCurve, parameter, &derivative)) {
            freeformTangenciesAreValid = false;
            continue;
        }
        const QPointF chord = point - archOrigin;
        const qreal denominator = std::hypot(chord.x(), chord.y()) *
                                  std::hypot(derivative.x(), derivative.y());
        const qreal tangentResidual = denominator <= 1.0e-12
                                          ? 1.0
                                          : std::abs(crossProduct(chord, derivative)) /
                                                denominator;
        freeformTangenciesAreValid &= candidate.type == SnapType::Tangent &&
                                      std::hypot(point.x() - candidate.point.x(),
                                                 point.y() - candidate.point.y()) <= 1.0e-6 &&
                                      tangentResidual <= 1.0e-7;
    }
    passed &= check(freeformTangenciesAreValid,
                    "freeform Bezier tangent lines must match the curve's analytic derivative");

    SnapSettings nearOnlySettings{true, false, false, false, false, false, false, true};
    SnapEngine nearSnapEngine;
    nearSnapEngine.setSettings(nearOnlySettings);
    const QPointF nearLineCursor = viewportTransform.screenToWorld(
        lineScreenPoint + QPointF(0.0, 5.0), viewportSize);
    const SnapResult nearLineSnap = nearSnapEngine.findSnapPoint(serviceDocument,
                                                                 nearLineCursor,
                                                                 true,
                                                                 {},
                                                                 viewportTransform,
                                                                 viewportSize);
    passed &= check(nearLineSnap.type == SnapType::Near &&
                        std::hypot(nearLineSnap.point.x() - 5.0,
                                   nearLineSnap.point.y()) <= 1.0e-6,
                    "Near OSnap must find the closest point along line geometry");

    Document centerAndNearDocument;
    Shape centerAndNearCircle;
    centerAndNearCircle.geometryType = GeometryType::Circle;
    centerAndNearCircle.points = {QPointF(0.0, 0.0), QPointF(5.0, 0.0)};
    centerAndNearDocument.append(centerAndNearCircle);
    Shape nearbyLine;
    nearbyLine.geometryType = GeometryType::Line;
    nearbyLine.points = {QPointF(-10.0, 1.0), QPointF(10.0, 1.0)};
    nearbyLine.nurbs = makeDegreeOneNurbs(nearbyLine.points);
    centerAndNearDocument.append(nearbyLine);
    SnapEngine centerAndNearSnapEngine;
    centerAndNearSnapEngine.setSettings(
        SnapSettings{true, false, false, false, true, false, false, true, false});
    const SnapResult centerPreferredOverNear = centerAndNearSnapEngine.findSnapPoint(
        centerAndNearDocument,
        QPointF(0.0, 1.0),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(centerPreferredOverNear.type == SnapType::Center &&
                        pointsAlmostEqual(centerPreferredOverNear.point,
                                          QPointF(0.0, 0.0)),
                    "an enabled Center OSnap must take priority over a closer Near result");

    Document endpointAndNearDragDocument;
    Shape endpointAndNearDragSource;
    endpointAndNearDragSource.geometryType = GeometryType::Point;
    endpointAndNearDragSource.points = {QPointF(5.0, 0.0)};
    endpointAndNearDragDocument.append(endpointAndNearDragSource);
    Shape endpointAndNearDragTarget;
    endpointAndNearDragTarget.geometryType = GeometryType::Line;
    endpointAndNearDragTarget.points = {QPointF(0.0, 4.0), QPointF(10.0, 4.0)};
    endpointAndNearDragTarget.nurbs =
        makeDegreeOneNurbs(endpointAndNearDragTarget.points);
    endpointAndNearDragDocument.append(endpointAndNearDragTarget);
    SnapEngine endpointAndNearDragEngine;
    endpointAndNearDragEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false, true, false});
    const DragSnapResult endpointPreferredOverNear =
        endpointAndNearDragEngine.findDragSnap(
            endpointAndNearDragDocument, {0}, viewportTransform, viewportSize);
    const DragSnapResult endpointRecheckedDuringNearLock =
        endpointAndNearDragEngine.findDragSnap(
            endpointAndNearDragDocument,
            {0},
            viewportTransform,
            viewportSize,
            false,
            false);
    const QPointF dragNearPoint(5.0, 4.0);
    const qreal dragNearDistance = std::hypot(
        viewportTransform.worldToScreen(dragNearPoint, viewportSize).x() -
            viewportTransform.worldToScreen(QPointF(5.0, 0.0), viewportSize).x(),
        viewportTransform.worldToScreen(dragNearPoint, viewportSize).y() -
            viewportTransform.worldToScreen(QPointF(5.0, 0.0), viewportSize).y());
    const qreal dragEndpointDistance = std::hypot(
        viewportTransform.worldToScreen(QPointF(0.0, 4.0), viewportSize).x() -
            viewportTransform.worldToScreen(QPointF(5.0, 0.0), viewportSize).x(),
        viewportTransform.worldToScreen(QPointF(0.0, 4.0), viewportSize).y() -
            viewportTransform.worldToScreen(QPointF(5.0, 0.0), viewportSize).y());
    passed &= check(endpointPreferredOverNear.type == SnapType::Endpoint &&
                        (pointsAlmostEqual(endpointPreferredOverNear.targetPoint,
                                           QPointF(0.0, 4.0)) ||
                         pointsAlmostEqual(endpointPreferredOverNear.targetPoint,
                                           QPointF(10.0, 4.0))) &&
                        dragNearDistance < dragEndpointDistance &&
                        dragEndpointDistance < 12.0 &&
                        endpointRecheckedDuringNearLock.type == SnapType::Endpoint,
                    "object dragging must prefer Endpoint over a closer Near result and be able to recheck specific snaps while Near is latched");

    Document nearDragDocument;
    Shape nearDragSource;
    nearDragSource.geometryType = GeometryType::Line;
    nearDragSource.points = {QPointF(0.0, 15.0), QPointF(5.0, 15.0)};
    nearDragSource.nurbs = makeDegreeOneNurbs(nearDragSource.points);
    nearDragDocument.append(nearDragSource);
    Shape firstNearRail;
    firstNearRail.geometryType = GeometryType::Line;
    firstNearRail.points = {QPointF(-50.0, 0.0), QPointF(50.0, 0.0)};
    firstNearRail.nurbs = makeDegreeOneNurbs(firstNearRail.points);
    nearDragDocument.append(firstNearRail);
    Shape activeNearRail;
    activeNearRail.geometryType = GeometryType::Line;
    activeNearRail.points = {QPointF(-50.0, 10.0), QPointF(50.0, 10.0)};
    activeNearRail.nurbs = makeDegreeOneNurbs(activeNearRail.points);
    nearDragDocument.append(activeNearRail);
    SnapEngine nearDragEngine;
    nearDragEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false, true, false});
    const DragSnapResult acquiredNearRail = nearDragEngine.findDragSnap(
        nearDragDocument, {0}, viewportTransform, viewportSize);
    const QPointF acquiredNearRailScreen =
        viewportTransform.worldToScreen(acquiredNearRail.targetPoint, viewportSize);
    const QPointF farAlongNearRail = viewportTransform.screenToWorld(
        acquiredNearRailScreen + QPointF(30.0, 0.0), viewportSize);
    const DragSnapResult trackedNearRail = nearDragEngine.trackNearDragSnap(
        nearDragDocument,
        {0},
        farAlongNearRail,
        acquiredNearRail.targetShapeIndex,
        acquiredNearRail.targetComponentIndex,
        viewportTransform,
        viewportSize,
        12.0);
    const QPointF outsideNearRail = viewportTransform.screenToWorld(
        acquiredNearRailScreen + QPointF(0.0, 13.0), viewportSize);
    const DragSnapResult releasedNearRail = nearDragEngine.trackNearDragSnap(
        nearDragDocument,
        {0},
        outsideNearRail,
        acquiredNearRail.targetShapeIndex,
        acquiredNearRail.targetComponentIndex,
        viewportTransform,
        viewportSize,
        12.0);
    passed &= check(acquiredNearRail.type == SnapType::Near &&
                        acquiredNearRail.targetShapeIndex == 2 &&
                        trackedNearRail.type == SnapType::Near &&
                        trackedNearRail.targetShapeIndex == 2 &&
                        std::hypot(trackedNearRail.targetPoint.x() - farAlongNearRail.x(),
                                   trackedNearRail.targetPoint.y() - 10.0) <= 1.0e-6 &&
                        !releasedNearRail.isValid(),
                    "Near drag tracking must follow the same rail regardless of travel distance and release outside its screen tolerance");

    const QPointF polygonEdgeMidpoint =
        (polygonShape.points[0] + polygonShape.points[1]) * 0.5;
    const SnapResult nearPolygonSnap = nearSnapEngine.findSnapPoint(
        polygonSnapDocument,
        polygonEdgeMidpoint,
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(nearPolygonSnap.type == SnapType::Near &&
                        pointsAlmostEqual(nearPolygonSnap.point, polygonEdgeMidpoint),
                    "Near OSnap must project onto polygon sides");

    SnapEngine controlPointOsnapEngine;
    controlPointOsnapEngine.setSettings(
        SnapSettings{true, false, false, false, false, false, false, false, true});
    Document controlPointOsnapDocument;
    controlPointOsnapDocument.append(archShape);
    const SnapResult bezierControlPointSnap = controlPointOsnapEngine.findSnapPoint(
        controlPointOsnapDocument,
        archControlPoints[1] + QPointF(2.0, 1.0),
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(bezierControlPointSnap.type == SnapType::ControlPoint &&
                        bezierControlPointSnap.point == archControlPoints[1],
                    "Control Points OSnap must target the stored NURBS control vertices");

    controlPointOsnapEngine.setSettings(SnapSettings{true,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false,
                                                     false});
    const SnapResult disabledControlPointSnap = controlPointOsnapEngine.findSnapPoint(
        controlPointOsnapDocument,
        archControlPoints[1],
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(!disabledControlPointSnap.isValid(),
                    "Control Points OSnap must not produce candidates when its mode is off");

    Document controlPointSnapDocument;
    Shape movingControlPointLine = lineShape;
    movingControlPointLine.points = {QPointF(17.0, 50.0), QPointF(100.0, 100.0)};
    movingControlPointLine.nurbs = makeDegreeOneNurbs(movingControlPointLine.points);
    controlPointSnapDocument.append(movingControlPointLine);
    controlPointSnapDocument.append(
        Shape{GeometryType::Rectangle,
              {QPointF(0.0, 0.0), QPointF(100.0, 50.0)},
              {},
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    SnapEngine controlPointSnapEngine;
    controlPointSnapEngine.setSettings(
        SnapSettings{false, false, false, false, false, false, false, false, true});
    const DragSnapResult rectangleCornerSnap =
        controlPointSnapEngine.findControlPointSnap(controlPointSnapDocument,
                                                    0,
                                                    0,
                                                    QPointF(17.0, 50.0),
                                                    viewportTransform,
                                                    viewportSize);
    passed &= check(rectangleCornerSnap.type == SnapType::ControlPoint &&
                        rectangleCornerSnap.targetPoint == QPointF(0.0, 50.0),
                    "control-point dragging must snap to enabled control-point targets even when OSnap is disabled");

    controlPointSnapEngine.setSettings(
        SnapSettings{false, false, false, false, false, false, false, false, false});
    const DragSnapResult disabledControlPointDragSnap =
        controlPointSnapEngine.findControlPointSnap(controlPointSnapDocument,
                                                    0,
                                                    0,
                                                    QPointF(17.0, 50.0),
                                                    viewportTransform,
                                                    viewportSize);
    passed &= check(!disabledControlPointDragSnap.isValid(),
                    "control-point dragging must not snap to control points when that OSnap mode is off");

    SnapEngine endpointControlPointSnapEngine;
    endpointControlPointSnapEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false});
    const DragSnapResult endpointModeCornerSnap =
        endpointControlPointSnapEngine.findControlPointSnap(controlPointSnapDocument,
                                                            0,
                                                            0,
                                                            QPointF(17.0, 50.0),
                                                            viewportTransform,
                                                            viewportSize);
    passed &= check(endpointModeCornerSnap.type == SnapType::Endpoint &&
                        endpointModeCornerSnap.targetPoint == QPointF(0.0, 50.0),
                    "Endpoint OSnap must identify rectangle corners as endpoints while dragging a control point");

    const QPointF fixedLineEndpoint(20.0, 0.0);
    const QPointF circleTangentPoint(5.0, 8.660254037844386);
    const QPointF draggedLineEndpoint = circleTangentPoint + QPointF(1.0, 0.0);
    Document tangentControlPointDocument;
    tangentControlPointDocument.append(
        Shape{GeometryType::Line,
              {fixedLineEndpoint, draggedLineEndpoint},
              makeDegreeOneNurbs({fixedLineEndpoint, draggedLineEndpoint}),
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    tangentControlPointDocument.append(circle);
    SnapEngine tangentControlPointSnapEngine;
    tangentControlPointSnapEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, true, false, false});
    const DragSnapResult draggedEndpointTangentSnap =
        tangentControlPointSnapEngine.findControlPointSnap(
            tangentControlPointDocument,
            0,
            1,
            draggedLineEndpoint,
            viewportTransform,
            viewportSize);
    passed &= check(draggedEndpointTangentSnap.type == SnapType::Tangent &&
                        std::hypot(draggedEndpointTangentSnap.targetPoint.x() -
                                       circleTangentPoint.x(),
                                   draggedEndpointTangentSnap.targetPoint.y() -
                                       circleTangentPoint.y()) <= 1.0e-6,
                    "dragged line endpoints must snap to curve tangencies when Tangent OSnap is enabled");
    const DragSnapResult movedLineTangentSnap =
        tangentControlPointSnapEngine.findDragSnap(tangentControlPointDocument,
                                                   {0},
                                                   viewportTransform,
                                                   viewportSize);
    passed &= check(movedLineTangentSnap.type == SnapType::Tangent &&
                        movedLineTangentSnap.sourcePoint == draggedLineEndpoint &&
                        std::hypot(movedLineTangentSnap.targetPoint.x() -
                                       circleTangentPoint.x(),
                                   movedLineTangentSnap.targetPoint.y() -
                                       circleTangentPoint.y()) <= 1.0e-6,
                    "moving a line must snap its endpoint to a curve tangent when Endpoint and Tangent OSnaps are enabled");

    const NurbsCurve2D nurbsOnlyBezier = makeBezierNurbs(
        {QPointF(50.0, 50.0), QPointF(60.0, 50.0),
         QPointF(70.0, 50.0), QPointF(80.0, 50.0)});
    const NurbsCurve2D nearBezierCurve = makeBezierNurbs(
        {QPointF(0.0, 0.0), QPointF(0.0, 10.0),
         QPointF(10.0, 10.0), QPointF(10.0, 0.0)});
    Shape projectionBezier{GeometryType::Bezier,
                           {},
                           nearBezierCurve,
                           ArcMode::TwoPoint,
                           0.0,
                           {},
                           {}};
    QPointF perpendicularBezierPoint;
    const bool foundBezierPerpendicular = snapEngine.perpendicularPointForShape(
        projectionBezier,
        QPointF(5.0, 12.5),
        viewportTransform,
        viewportSize,
        &perpendicularBezierPoint);
    passed &= check(foundBezierPerpendicular &&
                        std::hypot(perpendicularBezierPoint.x() - 5.0,
                                   perpendicularBezierPoint.y() - 7.5) <= 1.0e-6,
                    "perpendicular projection must evaluate the stored NURBS curve");
    Document nearBezierDocument;
    nearBezierDocument.append(Shape{GeometryType::Bezier,
                                    {},
                                    nearBezierCurve,
                                    ArcMode::TwoPoint,
                                    0.0,
                                    {},
                                    {}});
    const QPointF nearBezierPoint(5.0, 7.5);
    const QPointF nearBezierCursor = viewportTransform.screenToWorld(
        viewportTransform.worldToScreen(nearBezierPoint, viewportSize),
        viewportSize);
    const SnapResult nearBezierSnap = nearSnapEngine.findSnapPoint(
        nearBezierDocument,
        nearBezierCursor,
        true,
        {},
        viewportTransform,
        viewportSize);
    passed &= check(nearBezierSnap.type == SnapType::Near &&
                        std::hypot(nearBezierSnap.point.x() - nearBezierPoint.x(),
                                   nearBezierSnap.point.y() - nearBezierPoint.y()) <= 1.0e-6,
                    "Near OSnap must find the closest evaluated point on stored NURBS curves");

    Document nurbsEndpointDocument;
    Shape nurbsEndpointSource = movingControlPointLine;
    nurbsEndpointSource.points = {QPointF(52.0, 50.0), QPointF(100.0, 100.0)};
    nurbsEndpointSource.nurbs = makeDegreeOneNurbs(nurbsEndpointSource.points);
    nurbsEndpointDocument.append(nurbsEndpointSource);
    nurbsEndpointDocument.append(Shape{GeometryType::Bezier,
                                       {},
                                       nurbsOnlyBezier,
                                       ArcMode::TwoPoint,
                                       0.0,
                                       {},
                                       {}});
    const DragSnapResult nurbsEndpointSnap =
        endpointControlPointSnapEngine.findControlPointSnap(nurbsEndpointDocument,
                                                            0,
                                                            0,
                                                            QPointF(52.0, 50.0),
                                                            viewportTransform,
                                                            viewportSize);
    passed &= check(nurbsEndpointSnap.type == SnapType::Endpoint &&
                        nurbsEndpointSnap.targetPoint == QPointF(50.0, 50.0),
                    "Endpoint OSnap must find evaluated endpoints on NURBS-only curves");

    Document reverseDragSnapDocument;
    reverseDragSnapDocument.append(
        Shape{GeometryType::Rectangle,
              {QPointF(50.0, 52.0), QPointF(100.0, 100.0)},
              {},
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    reverseDragSnapDocument.append(Shape{GeometryType::Bezier,
                                         {},
                                         nurbsOnlyBezier,
                                         ArcMode::TwoPoint,
                                         0.0,
                                         {},
                                         {}});
    const DragSnapResult reverseDirectionSnap =
        endpointControlPointSnapEngine.findDragSnap(reverseDragSnapDocument,
                                                    {1},
                                                    viewportTransform,
                                                    viewportSize);
    passed &= check(reverseDirectionSnap.type == SnapType::Endpoint &&
                        reverseDirectionSnap.sourcePoint == QPointF(50.0, 50.0) &&
                        reverseDirectionSnap.targetPoint == QPointF(50.0, 52.0),
                    "Bezier endpoints must remain snap sources when dragging the curve toward a rectangle");

    Document toolDocument;
    SelectionModel toolSelection;
    History toolHistory(toolDocument);
    ViewportTransform toolTransform;
    CurveSampler toolSampler;
    CurveHitTester toolHitTester;
    SnapEngine toolSnapEngine;
    ToolContext toolContext(toolDocument,
                            toolSelection,
                            toolHistory,
                            toolTransform,
                            toolSampler,
                            toolHitTester,
                            toolSnapEngine);
    QVector<Shape> committedToolShapes;
    ToolId finishedTool = ToolId::Select;
    int previewUpdateCount = 0;
    toolContext.setShapeFactory(
        [](ToolId tool,
           const QVector<QPointF> &points,
           ArcMode arcMode,
           qreal arcSweep,
           Shape *shape) {
            if (shape == nullptr || points.isEmpty()) {
                return false;
            }
            *shape = Shape{geometryTypeForTool(tool),
                           points,
                           Shape::NurbsCurve2D{},
                           arcMode,
                           arcSweep,
                           {},
                           {}};
            if (tool == ToolId::Line) {
                shape->nurbs = makeDegreeOneNurbs(points);
            } else if (isCircleConstructionTool(tool)) {
                QVector<QPointF> definition = points;
                if (tool == ToolId::CircleDiameter &&
                    !makeCircleDefinitionFromDiameter(points[0], points[1], &definition)) {
                    return false;
                }
                if (tool == ToolId::CircleThreePoint &&
                    !makeCircleDefinitionFromThreePoints(
                        points[0], points[1], points[2], &definition)) {
                    return false;
                }
                shape->points = definition;
                shape->nurbs = makeCircleNurbs(definition);
                return validateNurbsCurve(shape->nurbs);
            }
            return true;
        });
    toolContext.setShapeCommitter(
        [&committedToolShapes](ToolId, const Shape &shape) {
            committedToolShapes.append(shape);
            return true;
        });
    toolContext.setToolFinisher([&finishedTool](ToolId tool) {
        finishedTool = tool;
    });
    toolContext.setPreviewPublisher([&previewUpdateCount](const ToolPreview &) {
        ++previewUpdateCount;
    });

    ToolRegistry toolRegistry;
    passed &= check(toolRegistry.find(ToolId::Select) != nullptr &&
                        toolRegistry.find(ToolId::Point) != nullptr &&
                        toolRegistry.find(ToolId::Line) != nullptr &&
                        toolRegistry.find(ToolId::TangentFromCurve) != nullptr &&
                        toolRegistry.find(ToolId::PerpendicularFromCurve) != nullptr &&
                        toolRegistry.find(ToolId::Arc) != nullptr &&
                        toolRegistry.find(ToolId::Rectangle) != nullptr &&
                        toolRegistry.find(ToolId::RectangleFromCenter) != nullptr &&
                        toolRegistry.find(ToolId::RectangleThreePoint) != nullptr &&
                        toolRegistry.find(ToolId::PolygonCenterCorner) != nullptr &&
                        toolRegistry.find(ToolId::PolygonCenterTangent) != nullptr &&
                        toolRegistry.find(ToolId::PolygonCornerCorner) != nullptr &&
                        toolRegistry.find(ToolId::PolygonEdge) != nullptr &&
                        toolRegistry.find(ToolId::Circle) != nullptr &&
                        toolRegistry.find(ToolId::CircleDiameter) != nullptr &&
                        toolRegistry.find(ToolId::CircleThreePoint) != nullptr &&
                        toolRegistry.find(ToolId::CircleTangentTwo) != nullptr &&
                        toolRegistry.find(ToolId::CircleTangentThree) != nullptr &&
                        toolRegistry.find(ToolId::LinearDimension) != nullptr &&
                        toolRegistry.find(ToolId::AngularDimension) != nullptr &&
                        toolRegistry.find(ToolId::Ellipse) != nullptr &&
                        toolRegistry.find(ToolId::EllipseFromEndpoints) != nullptr &&
                        toolRegistry.find(ToolId::EllipseFromCorners) != nullptr &&
                        toolRegistry.find(ToolId::EllipseFromFoci) != nullptr &&
                        toolRegistry.find(ToolId::Bezier) != nullptr &&
                        toolRegistry.find(ToolId::Nurbs) != nullptr &&
                        toolRegistry.find(ToolId::Rotate) != nullptr &&
                        toolRegistry.find(ToolId::Mirror) != nullptr &&
                        toolRegistry.find(ToolId::Trim) != nullptr &&
                        toolRegistry.find(ToolId::Erase) != nullptr,
                    "tool registry must expose the named interaction tool modules");

    PointTool pointTool;
    pointTool.begin(toolContext);
    ToolInput pointInput;
    pointInput.button = Qt::LeftButton;
    pointInput.worldPosition = QPointF(3.0, 4.0);
    passed &= check(pointTool.handleMousePress(pointInput, toolContext) &&
                        committedToolShapes.size() == 1 &&
                        committedToolShapes.back().geometryType == GeometryType::Point &&
                        committedToolShapes.back().points == QVector<QPointF>{QPointF(3.0, 4.0)} &&
                        finishedTool == ToolId::Select,
                    "point tool must commit through ToolContext and finish to Select");

    LineTool lineTool;
    lineTool.begin(toolContext);
    ToolInput lineInput;
    lineInput.button = Qt::LeftButton;
    lineInput.worldPosition = QPointF(0.0, 0.0);
    lineTool.handleMousePress(lineInput, toolContext);
    lineInput.worldPosition = QPointF(10.0, 0.0);
    lineTool.handleMousePress(lineInput, toolContext);
    lineInput.button = Qt::RightButton;
    passed &= check(lineTool.handleMousePress(lineInput, toolContext) &&
                        committedToolShapes.size() == 2 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        validateNurbsCurve(committedToolShapes.back().nurbs) &&
                        previewUpdateCount >= 3 &&
                        finishedTool == ToolId::Select,
                    "line tool must retain connected points until right-click commit");

    const Shape tangentTestCircle{GeometryType::Circle,
                                  {QPointF(0.0, 0.0), QPointF(10.0, 0.0)},
                                  makeCircleNurbs({QPointF(0.0, 0.0), QPointF(10.0, 0.0)}),
                                  ArcMode::TwoPoint,
                                  0.0,
                                  {},
                                  {}};
    const ObjectId tangentCircleId = toolDocument.append(tangentTestCircle);
    constexpr QPointF tangentEndpoint(20.0, 0.0);
    const ObjectId tangentTargetId = toolDocument.append(
        Shape{GeometryType::Line,
              {tangentEndpoint, QPointF(30.0, 0.0)},
              makeDegreeOneNurbs({tangentEndpoint, QPointF(30.0, 0.0)}),
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    constexpr QSize tangentViewportSize(640, 480);
    const auto tangentInputAt = [&](const QPointF &worldPoint) {
        ToolInput input;
        input.screenPosition = toolTransform.worldToScreen(worldPoint,
                                                           tangentViewportSize);
        input.rawWorldPosition = worldPoint;
        input.worldPosition = worldPoint;
        input.viewportSize = tangentViewportSize;
        input.button = Qt::LeftButton;
        return input;
    };

    TangentFromCurveTool tangentTool;
    tangentTool.begin(toolContext);
    const ToolInput tangentCurvePick = tangentInputAt(QPointF(10.0, 0.0));
    const bool tangentCurvePicked = tangentTool.handleMousePress(tangentCurvePick,
                                                                 toolContext);
    const ToolInput insideCircleCursor = tangentInputAt(QPointF(5.0, 0.0));
    tangentTool.handleMouseMove(insideCircleCursor, toolContext);
    const bool tangentUnavailableInside = tangentTool.preview().points.isEmpty();

    const QPointF rawTangentCursor(19.5, 0.25);
    SnapEngine tangentEndpointSnapEngine;
    tangentEndpointSnapEngine.setSettings(
        SnapSettings{true, true, false, false, false, false, false, false});
    const SnapResult tangentEndpointSnap = tangentEndpointSnapEngine.findSnapPoint(
        toolDocument,
        rawTangentCursor,
        true,
        {},
        toolTransform,
        tangentViewportSize);
    ToolInput tangentCursor = tangentInputAt(tangentEndpointSnap.point);
    tangentCursor.screenPosition = toolTransform.worldToScreen(rawTangentCursor,
                                                              tangentViewportSize);
    tangentCursor.rawWorldPosition = rawTangentCursor;
    tangentTool.handleMouseMove(tangentCursor, toolContext);
    const ToolPreview tangentPreview = tangentTool.preview();
    const bool tangentPreviewIsValid = tangentPreview.points.size() == 1 &&
        std::abs(QPointF::dotProduct(tangentPreview.points.first(),
                                     tangentPreview.points.first()) - 100.0) <= 1.0e-4 &&
        std::abs(QPointF::dotProduct(tangentPreview.points.first(),
                                     tangentEndpoint - tangentPreview.points.first())) <= 1.0e-4;
    const bool tangentLineCommitted = tangentTool.handleMousePress(tangentCursor,
                                                                   toolContext);
    passed &= check(tangentCurvePicked &&
                        toolSelection.primaryObjectId() == tangentCircleId &&
                        tangentTargetId.isValid() &&
                        tangentUnavailableInside &&
                        tangentEndpointSnap.type == SnapType::Endpoint &&
                        tangentEndpointSnap.point == tangentEndpoint &&
                        tangentPreviewIsValid &&
                        tangentLineCommitted &&
                        committedToolShapes.size() == 3 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        committedToolShapes.back().points.first() == tangentPreview.points.first() &&
                        committedToolShapes.back().points.back() == tangentEndpoint &&
                        validateNurbsCurve(committedToolShapes.back().nurbs) &&
                        finishedTool == ToolId::Select,
                    "tangent-from-curve tool must preview and commit a tangent line from a selected curve");

    const ObjectId archObjectId = toolDocument.append(archShape);
    QPointF archPickPoint;
    const bool archPickPointEvaluated = evaluateNurbsPoint(archCurve,
                                                           0.25,
                                                           &archPickPoint);
    TangentFromCurveTool freeformTangentTool;
    freeformTangentTool.begin(toolContext);
    const bool freeformCurvePicked = archPickPointEvaluated &&
        freeformTangentTool.handleMousePress(tangentInputAt(archPickPoint), toolContext);
    const ToolInput archEndpointInput = tangentInputAt(archOrigin);
    freeformTangentTool.handleMouseMove(archEndpointInput, toolContext);
    const ToolPreview freeformPreview = freeformTangentTool.preview();
    bool committedFreeformTangentIsValid = freeformPreview.points.size() == 1;
    if (committedFreeformTangentIsValid) {
        qreal low = archStartParameter;
        qreal high = archEndParameter;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const qreal middle = (low + high) * 0.5;
            QPointF point;
            evaluateNurbsPoint(archCurve, middle, &point);
            if (point.x() < freeformPreview.points.first().x()) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const qreal parameter = (low + high) * 0.5;
        QPointF point;
        QPointF derivative;
        if (!evaluateNurbsPoint(archCurve, parameter, &point) ||
            !evaluateNurbsDerivative(archCurve, parameter, &derivative)) {
            committedFreeformTangentIsValid = false;
        } else {
            const QPointF chord = archOrigin - point;
            const qreal denominator = std::hypot(chord.x(), chord.y()) *
                                      std::hypot(derivative.x(), derivative.y());
            committedFreeformTangentIsValid = denominator > 1.0e-12 &&
                std::abs(crossProduct(chord, derivative)) / denominator <= 1.0e-7;
        }
    }
    const bool freeformLineCommitted = freeformTangentTool.handleMousePress(
        archEndpointInput, toolContext);
    passed &= check(archObjectId.isValid() && freeformCurvePicked &&
                        toolSelection.primaryObjectId() == archObjectId,
                    "tangent tool must select the requested freeform curve");
    passed &= check(committedFreeformTangentIsValid,
                    "freeform tangent preview must align with the curve derivative");
    passed &= check(freeformLineCommitted && committedToolShapes.size() == 4 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        committedToolShapes.back().points.first() ==
                            freeformPreview.points.first() &&
                        committedToolShapes.back().points.back() == archOrigin &&
                        validateNurbsCurve(committedToolShapes.back().nurbs),
                    "tangent tool must commit a line aligned with a freeform curve derivative");

    PerpendicularFromCurveTool perpendicularTool;
    perpendicularTool.begin(toolContext);
    const bool perpendicularCurvePicked = perpendicularTool.handleMousePress(
        tangentInputAt(QPointF(10.0, 0.0)), toolContext);
    const QPointF perpendicularEndpoint(20.0, 7.0);
    const ToolInput perpendicularInput = tangentInputAt(perpendicularEndpoint);
    perpendicularTool.handleMouseMove(perpendicularInput, toolContext);
    const ToolPreview perpendicularPreview = perpendicularTool.preview();
    bool perpendicularPreviewIsValid = perpendicularPreview.points.size() == 1;
    if (perpendicularPreviewIsValid) {
        const QPointF foot = perpendicularPreview.points.first();
        const QPointF radial = foot;
        const QPointF lineDirection = perpendicularEndpoint - foot;
        const qreal denominator = std::hypot(radial.x(), radial.y()) *
                                  std::hypot(lineDirection.x(), lineDirection.y());
        perpendicularPreviewIsValid = denominator > 1.0e-12 &&
            std::abs(crossProduct(radial, lineDirection)) / denominator <= 1.0e-7 &&
            std::abs(std::hypot(radial.x(), radial.y()) - 10.0) <= 1.0e-7;
    }
    const bool perpendicularLineCommitted = perpendicularTool.handleMousePress(
        perpendicularInput, toolContext);
    passed &= check(perpendicularCurvePicked &&
                        toolSelection.primaryObjectId() == tangentCircleId,
                    "perpendicular-from-curve tool must select the requested curve");
    passed &= check(perpendicularPreviewIsValid,
                    "perpendicular-from-curve tool must preview a normal line from the selected curve");
    passed &= check(perpendicularLineCommitted && committedToolShapes.size() == 5 &&
                        committedToolShapes.back().geometryType == GeometryType::Line &&
                        committedToolShapes.back().points.first() ==
                            perpendicularPreview.points.first() &&
                        committedToolShapes.back().points.back() == perpendicularEndpoint &&
                        validateNurbsCurve(committedToolShapes.back().nurbs) &&
                        finishedTool == ToolId::Select,
                    "perpendicular-from-curve tool must commit the previewed line as NURBS geometry");

    DimensionTool linearDimensionTool(ToolId::LinearDimension);
    linearDimensionTool.begin(toolContext);
    ToolInput dimensionInput;
    dimensionInput.button = Qt::LeftButton;
    dimensionInput.worldPosition = QPointF(0.0, 0.0);
    linearDimensionTool.handleMousePress(dimensionInput, toolContext);
    dimensionInput.worldPosition = QPointF(12.0, 0.0);
    linearDimensionTool.handleMousePress(dimensionInput, toolContext);
    ToolInput linearPlacementInput;
    linearPlacementInput.worldPosition = QPointF(6.0, 4.0);
    linearDimensionTool.handleMouseMove(linearPlacementInput, toolContext);
    const ToolPreview linearPreview = linearDimensionTool.preview();
    dimensionInput.worldPosition = QPointF(6.0, 4.0);
    const bool linearDimensionCommitted =
        linearDimensionTool.handleMousePress(dimensionInput, toolContext);
    passed &= check(linearPreview.hasShape &&
                        linearPreview.shape.geometryType == GeometryType::LinearDimension &&
                        linearDimensionCommitted && committedToolShapes.size() == 6 &&
                        committedToolShapes.back().geometryType == GeometryType::LinearDimension &&
                        committedToolShapes.back().points ==
                            QVector<QPointF>{QPointF(0.0, 0.0),
                                             QPointF(12.0, 0.0),
                                             QPointF(6.0, 4.0)},
                    "linear dimension tool must preview and commit a three-click measurement");

    DimensionTool angularDimensionTool(ToolId::AngularDimension);
    angularDimensionTool.begin(toolContext);
    dimensionInput.worldPosition = QPointF(0.0, 0.0);
    angularDimensionTool.handleMousePress(dimensionInput, toolContext);
    dimensionInput.worldPosition = QPointF(10.0, 0.0);
    angularDimensionTool.handleMousePress(dimensionInput, toolContext);
    ToolInput angularRayInput;
    angularRayInput.worldPosition = QPointF(0.0, 10.0);
    angularDimensionTool.handleMouseMove(angularRayInput, toolContext);
    const ToolPreview angularPreview = angularDimensionTool.preview();
    dimensionInput.worldPosition = QPointF(0.0, 10.0);
    const bool angularDimensionCommitted =
        angularDimensionTool.handleMousePress(dimensionInput, toolContext);
    passed &= check(angularPreview.hasShape &&
                        angularPreview.shape.geometryType == GeometryType::AngularDimension &&
                        angularDimensionCommitted && committedToolShapes.size() == 7 &&
                        committedToolShapes.back().geometryType == GeometryType::AngularDimension &&
                        finishedTool == ToolId::Select,
                    "angular dimension tool must preview and commit a vertex with two rays");

    Document circleToolDocument;
    SelectionModel circleToolSelection;
    History circleToolHistory(circleToolDocument);
    ViewportTransform circleToolTransform;
    CurveSampler circleToolSampler;
    CurveHitTester circleToolHitTester;
    SnapEngine circleToolSnapEngine;
    ToolContext circleToolContext(circleToolDocument,
                                  circleToolSelection,
                                  circleToolHistory,
                                  circleToolTransform,
                                  circleToolSampler,
                                  circleToolHitTester,
                                  circleToolSnapEngine);
    QVector<Shape> committedCircleTools;
    circleToolContext.setShapeFactory(
        [](ToolId tool,
           const QVector<QPointF> &points,
           ArcMode arcMode,
           qreal arcSweep,
           Shape *shape) {
            if (shape == nullptr || points.size() < requiredPoints(tool)) {
                return false;
            }
            QVector<QPointF> definition = points;
            if (tool == ToolId::CircleDiameter &&
                !makeCircleDefinitionFromDiameter(points[0], points[1], &definition)) {
                return false;
            }
            if (tool == ToolId::CircleThreePoint &&
                !makeCircleDefinitionFromThreePoints(
                    points[0], points[1], points[2], &definition)) {
                return false;
            }
            *shape = Shape{GeometryType::Circle,
                           definition,
                           makeCircleNurbs(definition),
                           arcMode,
                           arcSweep,
                           {},
                           {}};
            return validateNurbsCurve(shape->nurbs);
        });
    circleToolContext.setShapeCommitter(
        [&committedCircleTools](ToolId, const Shape &shape) {
            committedCircleTools.append(shape);
            return true;
        });
    circleToolContext.setToolFinisher([](ToolId) {});

    const auto circleInputAt = [&](const QPointF &worldPoint) {
        ToolInput input;
        input.screenPosition = circleToolTransform.worldToScreen(worldPoint,
                                                                 tangentViewportSize);
        input.rawWorldPosition = worldPoint;
        input.worldPosition = worldPoint;
        input.viewportSize = tangentViewportSize;
        input.button = Qt::LeftButton;
        return input;
    };

    CircleTool diameterCircleTool(ToolId::CircleDiameter);
    diameterCircleTool.begin(circleToolContext);
    diameterCircleTool.handleMousePress(circleInputAt(QPointF(-10.0, 0.0)),
                                        circleToolContext);
    const bool diameterCircleCommitted = diameterCircleTool.handleMousePress(
        circleInputAt(QPointF(10.0, 0.0)), circleToolContext);
    const bool diameterCircleIsCorrect = diameterCircleCommitted &&
        committedCircleTools.size() == 1 &&
        committedCircleTools.back().geometryType == GeometryType::Circle &&
        committedCircleTools.back().points.size() == 2 &&
        committedCircleTools.back().points[0] == QPointF(0.0, 0.0) &&
        std::abs(std::hypot(committedCircleTools.back().points[1].x(),
                            committedCircleTools.back().points[1].y()) -
                 10.0) <= 1.0e-9 &&
        validateNurbsCurve(committedCircleTools.back().nurbs);

    CircleTool threePointCircleTool(ToolId::CircleThreePoint);
    threePointCircleTool.begin(circleToolContext);
    threePointCircleTool.handleMousePress(circleInputAt(QPointF(1.0, 0.0)),
                                          circleToolContext);
    threePointCircleTool.handleMousePress(circleInputAt(QPointF(0.0, 1.0)),
                                          circleToolContext);
    const bool threePointCircleCommitted = threePointCircleTool.handleMousePress(
        circleInputAt(QPointF(-1.0, 0.0)), circleToolContext);
    const bool threePointCircleIsCorrect = threePointCircleCommitted &&
        committedCircleTools.size() == 2 &&
        committedCircleTools.back().geometryType == GeometryType::Circle &&
        std::hypot(committedCircleTools.back().points[0].x(),
                   committedCircleTools.back().points[0].y()) <= 1.0e-9 &&
        std::abs(std::hypot(committedCircleTools.back().points[1].x(),
                            committedCircleTools.back().points[1].y()) -
                 1.0) <= 1.0e-9 &&
        validateNurbsCurve(committedCircleTools.back().nurbs);
    passed &= check(diameterCircleIsCorrect && threePointCircleIsCorrect,
                    "2-point and 3-point circle tools must commit exact rational NURBS circles");

    const auto appendCircleTargetLine = [&circleToolDocument](const QPointF &first,
                                                              const QPointF &second) {
        const QVector<QPointF> points{first, second};
        return circleToolDocument.append(Shape{GeometryType::Line,
                                               points,
                                               makeDegreeOneNurbs(points),
                                               ArcMode::TwoPoint,
                                               0.0,
                                               {},
                                               {}});
    };
    const ObjectId lowerTangentLine = appendCircleTargetLine(QPointF(-100.0, 0.0),
                                                             QPointF(100.0, 0.0));
    const ObjectId upperTangentLine = appendCircleTargetLine(QPointF(-100.0, 10.0),
                                                             QPointF(100.0, 10.0));
    CircleTangentTool twoCurveCircleTool(ToolId::CircleTangentTwo);
    twoCurveCircleTool.begin(circleToolContext);
    twoCurveCircleTool.handleMousePress(circleInputAt(QPointF(40.0, 0.0)),
                                        circleToolContext);
    twoCurveCircleTool.handleMousePress(circleInputAt(QPointF(40.0, 10.0)),
                                        circleToolContext);
    const ToolInput twoCurveSeed = circleInputAt(QPointF(10.0, 5.0));
    twoCurveCircleTool.handleMouseMove(twoCurveSeed, circleToolContext);
    const ToolPreview twoCurveCirclePreview = twoCurveCircleTool.preview();
    const bool twoCurveCirclePreviewIsCorrect = twoCurveCirclePreview.hasShape &&
        twoCurveCirclePreview.shape.geometryType == GeometryType::Circle &&
        std::abs(twoCurveCirclePreview.shape.points.first().y() - 5.0) <= 1.0e-5 &&
        std::abs(std::hypot(twoCurveCirclePreview.shape.points[1].x() -
                                twoCurveCirclePreview.shape.points[0].x(),
                            twoCurveCirclePreview.shape.points[1].y() -
                                twoCurveCirclePreview.shape.points[0].y()) -
                 5.0) <= 1.0e-5;
    const bool twoCurveCircleCommitted = twoCurveCircleTool.handleMousePress(
        twoCurveSeed, circleToolContext);

    const QPointF concentricCircleCenter(250.0, 100.0);
    const ObjectId innerTangentCircle = circleToolDocument.append(
        Shape{GeometryType::Circle,
              {concentricCircleCenter, concentricCircleCenter + QPointF(10.0, 0.0)},
              makeCircleNurbs({concentricCircleCenter,
                               concentricCircleCenter + QPointF(10.0, 0.0)}),
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    const ObjectId outerTangentCircle = circleToolDocument.append(
        Shape{GeometryType::Circle,
              {concentricCircleCenter, concentricCircleCenter + QPointF(20.0, 0.0)},
              makeCircleNurbs({concentricCircleCenter,
                               concentricCircleCenter + QPointF(20.0, 0.0)}),
              ArcMode::TwoPoint,
              0.0,
              {},
              {}});
    CircleTangentTool twoCircleCircleTool(ToolId::CircleTangentTwo);
    twoCircleCircleTool.begin(circleToolContext);
    twoCircleCircleTool.handleMousePress(circleInputAt(QPointF(260.0, 100.0)),
                                         circleToolContext);
    twoCircleCircleTool.handleMousePress(circleInputAt(QPointF(270.0, 100.0)),
                                         circleToolContext);
    const ToolInput twoCircleSeed = circleInputAt(QPointF(265.0, 100.0));
    twoCircleCircleTool.handleMouseMove(twoCircleSeed, circleToolContext);
    const ToolPreview twoCircleCirclePreview = twoCircleCircleTool.preview();
    const bool twoCircleCirclePreviewIsCorrect = twoCircleCirclePreview.hasShape &&
        std::abs(twoCircleCirclePreview.shape.points.first().x() - 265.0) <= 0.02 &&
        std::abs(twoCircleCirclePreview.shape.points.first().y() - 100.0) <= 0.02 &&
        std::abs(std::hypot(twoCircleCirclePreview.shape.points[1].x() -
                                twoCircleCirclePreview.shape.points[0].x(),
                            twoCircleCirclePreview.shape.points[1].y() -
                                twoCircleCirclePreview.shape.points[0].y()) -
                 5.0) <= 0.02;
    const bool twoCircleCircleCommitted = twoCircleCircleTool.handleMousePress(
        twoCircleSeed, circleToolContext);

    const ObjectId triangleBottom = appendCircleTargetLine(QPointF(0.0, 100.0),
                                                           QPointF(200.0, 100.0));
    const ObjectId triangleLeft = appendCircleTargetLine(QPointF(100.0, 0.0),
                                                         QPointF(100.0, 200.0));
    const ObjectId triangleDiagonal = appendCircleTargetLine(QPointF(0.0, 220.0),
                                                             QPointF(220.0, 0.0));
    CircleTangentTool threeCurveCircleTool(ToolId::CircleTangentThree);
    threeCurveCircleTool.begin(circleToolContext);
    threeCurveCircleTool.handleMousePress(circleInputAt(QPointF(50.0, 100.0)),
                                          circleToolContext);
    threeCurveCircleTool.handleMousePress(circleInputAt(QPointF(100.0, 50.0)),
                                          circleToolContext);
    threeCurveCircleTool.handleMousePress(circleInputAt(QPointF(20.0, 200.0)),
                                          circleToolContext);
    const ToolInput threeCurveSeed = circleInputAt(QPointF(106.0, 106.0));
    threeCurveCircleTool.handleMouseMove(threeCurveSeed, circleToolContext);
    const ToolPreview threeCurveCirclePreview = threeCurveCircleTool.preview();
    bool threeCurveCirclePreviewIsCorrect = threeCurveCirclePreview.hasShape &&
        threeCurveCirclePreview.shape.geometryType == GeometryType::Circle &&
        validateNurbsCurve(threeCurveCirclePreview.shape.nurbs);
    if (threeCurveCirclePreviewIsCorrect) {
        const QPointF center = threeCurveCirclePreview.shape.points.first();
        const QPointF edge = threeCurveCirclePreview.shape.points[1];
        const qreal radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
        constexpr qreal inverseSqrtTwo = 0.70710678118654752440;
        threeCurveCirclePreviewIsCorrect =
            std::abs(center.y() - 100.0 - radius) <= 0.02 &&
            std::abs(center.x() - 100.0 - radius) <= 0.02 &&
            std::abs((220.0 - center.x() - center.y()) * inverseSqrtTwo - radius) <= 0.02;
    }
    ToolInput cycleTangentSolutionInput;
    cycleTangentSolutionInput.key = Qt::Key_Tab;
    const bool tangentSolutionTabHandled = threeCurveCircleTool.handleKey(
        cycleTangentSolutionInput, circleToolContext);
    const ToolPreview cycledThreeCurveCirclePreview = threeCurveCircleTool.preview();
    const bool tangentConfigurationAdvanced =
        cycledThreeCurveCirclePreview.statusText.contains(
            QStringLiteral("configuration 1 of 8"), Qt::CaseInsensitive);
    ToolInput reverseTangentSolutionInput;
    reverseTangentSolutionInput.key = Qt::Key_Backtab;
    reverseTangentSolutionInput.modifiers = Qt::ShiftModifier;
    const bool reverseTangentSolutionHandled = threeCurveCircleTool.handleKey(
        reverseTangentSolutionInput, circleToolContext);
    const ToolPreview reversedThreeCurveCirclePreview = threeCurveCircleTool.preview();
    const bool tangentSolutionReversed = reversedThreeCurveCirclePreview.hasShape &&
        std::hypot(reversedThreeCurveCirclePreview.shape.points.first().x() -
                       threeCurveCirclePreview.shape.points.first().x(),
                   reversedThreeCurveCirclePreview.shape.points.first().y() -
                       threeCurveCirclePreview.shape.points.first().y()) <= 0.02;
    const bool threeCurveCircleCommitted = threeCurveCircleTool.handleMousePress(
        threeCurveSeed, circleToolContext);
    passed &= check(lowerTangentLine.isValid() && upperTangentLine.isValid() &&
                        innerTangentCircle.isValid() && outerTangentCircle.isValid() &&
                        triangleBottom.isValid() && triangleLeft.isValid() &&
                        triangleDiagonal.isValid() &&
                        twoCurveCirclePreviewIsCorrect && twoCurveCircleCommitted &&
                        twoCircleCirclePreviewIsCorrect && twoCircleCircleCommitted &&
                        threeCurveCirclePreviewIsCorrect && tangentSolutionTabHandled &&
                        tangentConfigurationAdvanced && reverseTangentSolutionHandled &&
                        tangentSolutionReversed && threeCurveCircleCommitted &&
                        committedCircleTools.size() == 5 &&
                        committedCircleTools[2].geometryType == GeometryType::Circle &&
                        committedCircleTools[3].geometryType == GeometryType::Circle &&
                        committedCircleTools[4].geometryType == GeometryType::Circle &&
                        validateNurbsCurve(committedCircleTools[2].nurbs) &&
                        validateNurbsCurve(committedCircleTools[3].nurbs) &&
                        validateNurbsCurve(committedCircleTools[4].nurbs),
                    "2-curve and 3-curve tangent-circle tools must preview and commit tangent NURBS circles");

    const auto appendExactCircleTarget = [&circleToolDocument](const QPointF &center) {
        const QVector<QPointF> definition{center, center + QPointF(2.0, 0.0)};
        return circleToolDocument.append(Shape{GeometryType::Circle,
                                               definition,
                                               makeCircleNurbs(definition),
                                               ArcMode::TwoPoint,
                                               0.0,
                                               {},
                                               {}});
    };
    const QVector<QPointF> targetCenters{
        QPointF(-20.0, -100.0),
        QPointF(20.0, -100.0),
        QPointF(0.0, -65.35898384862245),
    };
    bool exactTargetsAdded = true;
    for (const QPointF &center : targetCenters) {
        exactTargetsAdded &= appendExactCircleTarget(center).isValid();
    }

    CircleTangentTool eightSolutionTool(ToolId::CircleTangentThree);
    eightSolutionTool.begin(circleToolContext);
    eightSolutionTool.handleMousePress(circleInputAt(QPointF(-22.0, -100.0)),
                                       circleToolContext);
    eightSolutionTool.handleMousePress(circleInputAt(QPointF(22.0, -100.0)),
                                       circleToolContext);
    eightSolutionTool.handleMousePress(circleInputAt(QPointF(0.0, -63.35898384862245)),
                                       circleToolContext);
    eightSolutionTool.handleMouseMove(circleInputAt(QPointF(0.0, -88.45)),
                                      circleToolContext);

    ToolInput nextTangentSolutionInput;
    nextTangentSolutionInput.key = Qt::Key_Tab;
    QVector<QPointF> solutionCenters;
    QVector<qreal> solutionRadii;
    bool allEightTangentSolutionsValid = true;
    for (int index = 0; index < 8; ++index) {
        if (index > 0) {
            allEightTangentSolutionsValid &= eightSolutionTool.handleKey(
                nextTangentSolutionInput, circleToolContext);
        }
        const ToolPreview solution = eightSolutionTool.preview();
        if (!solution.hasShape || solution.shape.points.size() < 2 ||
            !validateNurbsCurve(solution.shape.nurbs)) {
            allEightTangentSolutionsValid = false;
            continue;
        }
        const QPointF center = solution.shape.points[0];
        const QPointF edge = solution.shape.points[1] - center;
        const qreal radius = std::hypot(edge.x(), edge.y());
        for (const QPointF &targetCenter : targetCenters) {
            const qreal separation = std::hypot(center.x() - targetCenter.x(),
                                                center.y() - targetCenter.y());
            const qreal externalResidual = std::abs(separation - radius - 2.0);
            const qreal internalResidual =
                std::abs(separation - std::abs(radius - 2.0));
            allEightTangentSolutionsValid &=
                std::min(externalResidual, internalResidual) <= 1.0e-5;
        }
        for (int prior = 0; prior < solutionCenters.size(); ++prior) {
            allEightTangentSolutionsValid &=
                std::hypot(center.x() - solutionCenters[prior].x(),
                           center.y() - solutionCenters[prior].y()) > 1.0e-4 ||
                std::abs(radius - solutionRadii[prior]) > 1.0e-4;
        }
        solutionCenters.append(center);
        solutionRadii.append(radius);
    }
    const bool wrapsAfterEight = eightSolutionTool.handleKey(
        nextTangentSolutionInput, circleToolContext);
    const ToolPreview wrappedSolution = eightSolutionTool.preview();
    const bool cyclesAllEight = allEightTangentSolutionsValid &&
        solutionCenters.size() == 8 && wrapsAfterEight &&
        wrappedSolution.hasShape &&
        std::hypot(wrappedSolution.shape.points[0].x() - solutionCenters[0].x(),
                   wrappedSolution.shape.points[0].y() - solutionCenters[0].y()) <= 1.0e-5 &&
        std::abs(std::hypot(wrappedSolution.shape.points[1].x() -
                                wrappedSolution.shape.points[0].x(),
                            wrappedSolution.shape.points[1].y() -
                                wrappedSolution.shape.points[0].y()) -
                 solutionRadii[0]) <= 1.0e-5;
    const bool selectedNextSolution = eightSolutionTool.handleKey(
        nextTangentSolutionInput, circleToolContext);
    const ToolPreview selectedSolution = eightSolutionTool.preview();
    const int committedBeforeSelection = committedCircleTools.size();
    const bool selectedSolutionCommitted = eightSolutionTool.handleMousePress(
        circleInputAt(QPointF(0.0, -88.45)), circleToolContext);
    const bool committedSelectedSolution = selectedNextSolution &&
        selectedSolution.hasShape && selectedSolutionCommitted &&
        committedCircleTools.size() == committedBeforeSelection + 1 &&
        validateNurbsCurve(committedCircleTools.back().nurbs) &&
        std::hypot(committedCircleTools.back().points[0].x() -
                       selectedSolution.shape.points[0].x(),
                   committedCircleTools.back().points[0].y() -
                       selectedSolution.shape.points[0].y()) <= 1.0e-5;
    passed &= check(exactTargetsAdded && cyclesAllEight &&
                        committedSelectedSolution,
                    "three separate circles must cycle and commit all eight tangent-circle solutions");

    constexpr qreal pi = 3.14159265358979323846;
    const auto appendQuarterCircleArc = [&circleToolDocument](
                                            const QPointF &center,
                                            qreal startAngle) {
        constexpr qreal localPi = 3.14159265358979323846;
        const qreal middleAngle = startAngle + localPi / 4.0;
        const qreal endAngle = startAngle + localPi / 2.0;
        const qreal middleWeight = std::cos(localPi / 4.0);
        const auto pointAt = [center](qreal angle, qreal radius) {
            return center + QPointF(radius * std::cos(angle),
                                    radius * std::sin(angle));
        };
        Shape::NurbsCurve2D arc;
        arc.dimension = 2;
        arc.degree = 2;
        arc.order = 3;
        arc.rational = true;
        arc.controlPoints = {pointAt(startAngle, 2.0),
                             pointAt(middleAngle, 2.0 / middleWeight),
                             pointAt(endAngle, 2.0)};
        arc.weights = {1.0, middleWeight, 1.0};
        arc.knots = {0.0, 0.0, 1.0, 1.0};
        return circleToolDocument.append(Shape{GeometryType::Arc,
                                               {arc.controlPoints.first(),
                                                arc.controlPoints.last(),
                                                pointAt(middleAngle, 2.0)},
                                               arc,
                                               ArcMode::TwoPoint,
                                               0.0,
                                               {},
                                               {}});
    };
    const QPointF firstArcCenter(-20.0, -180.0);
    const QPointF secondArcCenter(20.0, -180.0);
    const QPointF thirdArcCenter(0.0, -145.35898384862245);
    const bool circularArcsAdded =
        appendQuarterCircleArc(firstArcCenter, 0.0).isValid() &&
        appendQuarterCircleArc(secondArcCenter, pi / 2.0).isValid() &&
        appendQuarterCircleArc(thirdArcCenter, 5.0 * pi / 4.0).isValid();
    CircleTangentTool arcTangentCircleTool(ToolId::CircleTangentThree);
    arcTangentCircleTool.begin(circleToolContext);
    arcTangentCircleTool.handleMousePress(
        circleInputAt(firstArcCenter + QPointF(std::sqrt(2.0), std::sqrt(2.0))),
        circleToolContext);
    arcTangentCircleTool.handleMousePress(
        circleInputAt(secondArcCenter + QPointF(-std::sqrt(2.0), std::sqrt(2.0))),
        circleToolContext);
    arcTangentCircleTool.handleMousePress(
        circleInputAt(thirdArcCenter + QPointF(0.0, -2.0)),
        circleToolContext);
    arcTangentCircleTool.handleMouseMove(
        circleInputAt(QPointF(0.0, -168.453)), circleToolContext);
    const ToolPreview arcTangentPreview = arcTangentCircleTool.preview();
    const bool arcsUseVisibleTangencies = circularArcsAdded &&
        arcTangentPreview.hasShape &&
        arcTangentPreview.statusText.contains(QStringLiteral("Tangent solution")) &&
        std::hypot(arcTangentPreview.shape.points[0].x(),
                   arcTangentPreview.shape.points[0].y() + 168.4529946162075) <= 0.05 &&
        validateNurbsCurve(arcTangentPreview.shape.nurbs);
    if (!arcsUseVisibleTangencies) {
        qWarning() << "arc tangent diagnostic" << circularArcsAdded
                   << arcTangentPreview.hasShape << arcTangentPreview.statusText
                   << arcTangentPreview.shape.points;
    }
    passed &= check(arcsUseVisibleTangencies,
                    "three circular arcs must use only tangent contacts on their visible spans");

    return passed ? 0 : 1;
}
