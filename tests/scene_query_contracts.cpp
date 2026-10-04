#include "core/document/document.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/hit_testing/projected_curve_bounds.h"
#include "services/hit_testing/selection_box_query.h"
#include "services/sampling/curve_sampler.h"
#include "services/viewport/viewport_transform.h"

#include <QDebug>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

Shape lineShape(qreal y)
{
    Shape shape;
    shape.geometryType = GeometryType::Line;
    shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    shape.nurbs = makeDegreeOneNurbs({{-80.0, y}, {80.0, y}});
    shape.points = {{-80.0, y}, {80.0, y}};
    return shape;
}

} // namespace

int main()
{
    constexpr QSize viewportSize{1280, 720};
    ViewportTransform transform;
    transform.setViewPreset(ViewportViewPreset::Top);
    transform.zoom() = 12.0;
    const QPointF cursor(viewportSize.width() * 0.5,
                         viewportSize.height() * 0.5);

    Shape centerCurve = lineShape(0.0);
    Shape distantCurve = lineShape(80.0);
    QRectF hull;
    bool passed = check(projectedNurbsControlHullBounds(
                            centerCurve.nurbs,
                            shapeWorkPlaneFrame(centerCurve),
                            transform,
                            viewportSize,
                            &hull),
                        "a valid planar NURBS control hull should project") &&
                  check(screenBoundsOverlap(hull,
                                            QRectF(cursor - QPointF(1.0, 1.0),
                                                   QSizeF(2.0, 2.0))),
                        "the projected control hull should cover the curve center");

    Document document;
    document.append(distantCurve);
    document.append(lineShape(-90.0));
    CurveHitTester hitTester;
    passed &= check(hitTester.hitTestShape(document,
                                          cursor,
                                          transform,
                                          viewportSize,
                                          true) == -1,
                    "control-hull rejection must preserve a miss when all curves are distant");
    document.append(centerCurve);
    passed &= check(hitTester.hitTestShape(document,
                                          cursor,
                                          transform,
                                          viewportSize,
                                          true) == 2,
                    "the broad phase must keep a curve whose control hull covers the cursor");
    passed &= check(hitTester.hitTestShapeOnAnyWorkPlane(document,
                                                         cursor,
                                                         transform,
                                                         viewportSize) == 2,
                    "workplane inference must preserve hits through its control-hull filter");

    ViewportTransform perspectiveTransform = transform;
    perspectiveTransform.setPerspectiveEnabled(true);
    QRectF perspectiveHull;
    Document perspectiveDocument;
    perspectiveDocument.append(centerCurve);
    passed &= check(projectedNurbsControlHullBounds(
                            centerCurve.nurbs,
                            shapeWorkPlaneFrame(centerCurve),
                            perspectiveTransform,
                            viewportSize,
                            &perspectiveHull) &&
                        screenBoundsOverlap(
                            perspectiveHull,
                            QRectF(cursor - QPointF(1.0, 1.0),
                                   QSizeF(2.0, 2.0))) &&
                        hitTester.hitTestShape(perspectiveDocument,
                                               cursor,
                                               perspectiveTransform,
                                               viewportSize,
                                               true) == 0,
                    "perspective projection must retain a curve inside its projected control hull");

    Shape orientedCurve = lineShape(0.0);
    orientedCurve.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XZ);
    Document orientedDocument;
    orientedDocument.append(orientedCurve);
    passed &= check(hitTester.hitTestShape(orientedDocument,
                                          cursor,
                                          transform,
                                          viewportSize,
                                          true) == 0,
                    "the curve broad phase must project control points through oriented workplanes");

    Shape pointWithLegacyCurveData;
    pointWithLegacyCurveData.geometryType = GeometryType::Point;
    pointWithLegacyCurveData.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    pointWithLegacyCurveData.points = {{0.0, 0.0}};
    pointWithLegacyCurveData.nurbs =
        makeDegreeOneNurbs({{0.0, 80.0}, {1.0, 80.0}});
    Document pointDocument;
    pointDocument.append(pointWithLegacyCurveData);
    passed &= check(hitTester.hitTestShape(pointDocument,
                                          cursor,
                                          transform,
                                          viewportSize,
                                          true) == 0,
                    "curve bounds must not filter a point from unrelated legacy curve data");

    CurveSampler sampler;
    const QRectF centerBox(cursor - QPointF(8.0, 8.0), QSizeF(16.0, 16.0));
    const SelectionBoxGeometryResult crossingResult =
        queryCurveOrPointSelectionBox(centerCurve,
                                      centerBox,
                                      true,
                                      sampler,
                                      transform,
                                      viewportSize);
    const SelectionBoxGeometryResult disjointResult =
        queryCurveOrPointSelectionBox(distantCurve,
                                      centerBox,
                                      true,
                                      sampler,
                                      transform,
                                      viewportSize);
    passed &= check(crossingResult.applies && crossingResult.matches &&
                        disjointResult.applies && !disjointResult.matches,
                    "selection-box bounds should skip disjoint curves and retain crossing curves");

    WorkPlaneFrame clippedFrame = makeWorkPlaneFrame(WorkPlane::XY, 5000.0);
    QRectF clippedBounds;
    passed &= check(!projectedNurbsControlHullBounds(centerCurve.nurbs,
                                                     clippedFrame,
                                                     transform,
                                                     viewportSize,
                                                     &clippedBounds),
                    "clipped control points must fail open to the existing narrow phase");
    return passed ? 0 : 1;
}
