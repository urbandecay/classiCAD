#include "core/document/document.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/hit_testing/projected_curve_bounds.h"
#include "services/hit_testing/selection_box_query.h"
#include "services/sampling/curve_sampler.h"
#include "services/snapping/snap_engine.h"
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
    Document snapScene;
    QVector<int> movingIndices;
    for (int i = 0; i < 256; ++i) {
        Shape point;
        point.geometryType = GeometryType::Point;
        point.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
        point.points = {{double(i - 128) * 4, 0}};
        snapScene.append(point);
        movingIndices.append(i);
    }
    SnapEngine snapEngine;
    SnapSettings snapSettings;
    snapSettings.enabled = true;
    snapSettings.endpoint = true;
    snapSettings.midpoint = snapSettings.center = snapSettings.intersection = false;
    snapSettings.perpendicular = snapSettings.tangent = snapSettings.near = false;
    snapSettings.controlPoint = false;
    snapEngine.setSettings(snapSettings);
    passed &= check(!snapEngine.findDragSnap(snapScene, movingIndices, transform,
                                             viewportSize).isValid(),
                    "moving the whole scene must have no stationary snap target");
    Shape targetPoint;
    targetPoint.geometryType = GeometryType::Point;
    targetPoint.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    const QPointF snapDelta = transform.screenToWorld(cursor + QPointF(3, 0), viewportSize);
    targetPoint.points = {snapDelta};
    snapScene.append(targetPoint);
    const auto dragSnap = snapEngine.findDragSnap(snapScene, movingIndices, transform,
                                                 viewportSize);
    passed &= check(dragSnap.isValid() && dragSnap.targetShapeIndex == 256 &&
                        std::abs(dragSnap.sourcePoint.x()) < 1.0e-9 &&
                        std::abs(dragSnap.translation.x() - snapDelta.x()) < 1.0e-9,
                    "large-selection spatial buckets must retain the nearest endpoint snap");
    snapScene.mutateGeometry(snapScene.objectIdAt(256), [](Shape &point) {
        point.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY, 5);
        return true;
    });
    const auto spatialSnap = snapEngine.findDragSnap(snapScene, movingIndices, transform,
                                                    viewportSize);
    passed &= check(spatialSnap.isValid() && spatialSnap.hasWorldTranslation &&
                        std::abs(spatialSnap.worldTranslation.x - snapDelta.x()) < 1.0e-9 &&
                        std::abs(spatialSnap.worldTranslation.z - 5) < 1.0e-9,
                    "large-selection spatial buckets must preserve the target's world depth");
    return passed ? 0 : 1;
}
