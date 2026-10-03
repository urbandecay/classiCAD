#include <QApplication>
#include <QKeyEvent>
#define private public
#define protected public
#include "../src/ui/viewport_widget.cpp"
#undef protected
#undef private

using namespace classiCAD;

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ViewportWidget view;
    view.resize(640, 480);
    int failures = 0;
    // A bent degree-1 spline overlaps two straight curves along its long
    // leg. Join must fuse that leg and retain the bend in one selected object.
    for (const bool reversed : {false, true}) {
        Shape bent;
        bent.geometryType = GeometryType::Nurbs;
        bent.nurbs = makeDegreeOneNurbs(
            {QPointF(0, 0), QPointF(100, 0), QPointF(100, -10)});
        bent.points = bent.nurbs.controlPoints;
        bent.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
        Shape firstOverlap;
        firstOverlap.geometryType = GeometryType::Line;
        firstOverlap.workPlaneFrame = bent.workPlaneFrame;
        firstOverlap.workPlaneFrame.origin = {10, 5, 0};
        firstOverlap.nurbs = makeDegreeOneNurbs(
            {QPointF(-60, -5), QPointF(30, -5)});
        firstOverlap.points = firstOverlap.nurbs.controlPoints;
        Shape secondOverlap = bent;
        secondOverlap.geometryType = GeometryType::Line;
        secondOverlap.nurbs = makeDegreeOneNurbs(
            {QPointF(20, 0), QPointF(80, 0)});
        if (reversed) {
            firstOverlap.nurbs = view.reversedNurbsCurve(firstOverlap.nurbs);
            secondOverlap.nurbs = view.reversedNurbsCurve(secondOverlap.nurbs);
        }
        secondOverlap.points = secondOverlap.nurbs.controlPoints;
        view.shapes_ = {bent, firstOverlap, secondOverlap};
        view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0),
                                      view.shapes_.objectIdAt(1),
                                      view.shapes_.objectIdAt(2)};
        view.selectedShapeIndex_ = view.selectedShapeIndices_.last();
        view.beginJoinMode();
        bool joinedCorrectly = !view.joinActive_ && view.shapes_.size() == 1 &&
                               view.selectedShapeIndices_.size() == 1;
        if (joinedCorrectly) {
            const Shape &joined = view.shapes_[0];
            joinedCorrectly = joined.geometryType == GeometryType::PolyCurve &&
                               joined.components.size() == 2;
            qreal length = 0;
            for (const auto &component : joined.components) {
                QPointF start;
                QPointF end;
                joinedCorrectly = joinedCorrectly && validateNurbsCurve(component, nullptr) &&
                    view.nurbsCurveEndpoints(component, &start, &end);
                length += std::hypot(end.x() - start.x(), end.y() - start.y());
            }
            joinedCorrectly = joinedCorrectly && std::abs(length - 160.0) < 1.0e-7;
        }
        if (!joinedCorrectly) {
            qWarning() << "Join must fuse overlapping spans of a bent spline" << reversed;
            ++failures;
        }
    }
    // A drawn line and the union of overlapping lines have the same two CVs,
    // but Join stores its result as GeometryType::Nurbs. Both must accept a
    // body click and drag, including when their control points are displayed.
    {
        view.viewportTransform_.resetView();
        view.viewportTransform_.setViewPreset(ViewportViewPreset::Top);
        view.viewportTransform_.setPerspectiveEnabled(false);
        view.setOsnapEnabled(false);
        view.controlPointsVisible_ = true;
        Shape first;
        first.geometryType = GeometryType::Line;
        first.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
        first.nurbs = makeDegreeOneNurbs({QPointF(-40, 0), QPointF(20, 0)});
        first.points = first.nurbs.controlPoints;
        Shape second = first;
        second.nurbs = makeDegreeOneNurbs({QPointF(-10, 0), QPointF(60, 0)});
        second.points = second.nurbs.controlPoints;
        Shape drawn = first;
        drawn.nurbs = makeDegreeOneNurbs({QPointF(-40, -30), QPointF(60, -30)});
        drawn.points = drawn.nurbs.controlPoints;
        view.shapes_ = {first, second, drawn};
        const ObjectId drawnId = view.shapes_.objectIdAt(2);
        view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0),
                                      view.shapes_.objectIdAt(1)};
        view.selectedShapeIndex_ = view.selectedShapeIndices_.last();
        view.beginJoinMode();
        const ObjectId joinedId = view.selectedShapeIndex_;
        const auto mouse = [&](QEvent::Type type, const QPointF &position,
                               Qt::MouseButton button, Qt::MouseButtons buttons) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
#else
            QMouseEvent event(type, position, button, buttons, Qt::NoModifier);
#endif
            QApplication::sendEvent(&view, &event);
        };
        for (const ObjectId id : {joinedId, drawnId}) {
            const int index = view.objectIndex(id);
            if (index < 0) {
                qWarning() << "Join drag fixture lost its curve";
                ++failures;
                continue;
            }
            const QVector<QPointF> before = view.shapes_[index].nurbs.controlPoints;
            view.clearSelection();
            const QPointF body = view.viewportTransform_.workPlaneToScreen(
                (before.first() + before.last()) * 0.5, view.size(),
                shapeWorkPlaneFrame(view.shapes_[index]));
            mouse(QEvent::MouseButtonPress, body, Qt::LeftButton, Qt::LeftButton);
            const bool grabbed = view.draggingSelected_ && view.selectedShapeIndex_ == id;
            mouse(QEvent::MouseMove, body + QPointF(35, 20), Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, body + QPointF(35, 20), Qt::LeftButton, Qt::NoButton);
            const QVector<QPointF> after = view.shapes_[view.objectIndex(id)].nurbs.controlPoints;
            const QPointF startDelta = after.first() - before.first();
            const QPointF endDelta = after.last() - before.last();
            if (!grabbed || std::hypot(startDelta.x(), startDelta.y()) < 1.0e-5 ||
                std::hypot(startDelta.x() - endDelta.x(),
                           startDelta.y() - endDelta.y()) > 1.0e-7) {
                qWarning() << "Body drag must move both drawn and joined two-CV curves" << id.value();
                ++failures;
            }
        }
    }
    if (app.arguments().contains(QStringLiteral("--join-only"))) {
        qInfo() << "Join overlap failures:" << failures;
        return failures ? 1 : 0;
    }
    // Trim previews contain screen coordinates. Navigating after a hover
    // must refresh both the displayed preview and the click target, while
    // the actual cut stays in the curve's original local frame.
    for (int navigation = 0; navigation < 8; ++navigation) {
        view.resize(640, 480);
        view.viewportTransform_.resetView();
        view.viewportTransform_.setViewPreset(ViewportViewPreset::Top);
        view.viewportTransform_.setPerspectiveEnabled(true);
        view.viewportTransform_.zoom() = 0.8;
        view.viewportTransform_.setCameraPreferences(ViewportCameraPreferences{});
        const WorkPlaneFrame frame = makeWorkPlaneFrameFromNormal(
            {15.0, -10.0, 5.0}, {0.1, 0.2, 1.0});
        Shape source;
        source.geometryType = GeometryType::Line;
        source.nurbs = makeDegreeOneNurbs({QPointF(-80, 0), QPointF(80, 0)});
        source.points = source.nurbs.controlPoints;
        source.workPlaneFrame = frame;
        Shape cutter = source;
        cutter.nurbs = makeDegreeOneNurbs({QPointF(0, -80), QPointF(0, 80)});
        cutter.points = cutter.nurbs.controlPoints;
        view.shapes_ = {source, cutter};
        const ObjectId sourceId = view.shapes_.objectIdAt(0);
        view.selectedShapeIndices_ = {sourceId};
        view.selectedShapeIndex_ = sourceId;
        view.setTool(Tool::Trim);
        view.eraseGeometryCachePrepared_ = false;
        view.trimHoverPositionValid_ = false;
        const auto hoverPosition = [&]() {
            return view.viewportTransform_.workPlaneToScreen(
                QPointF(-40, 0), view.size(), frame);
        };
        view.updateTrimHover(hoverPosition());
        if (view.eraseCandidateShapeIndices_.size() != 1) {
            qWarning() << "Navigation trim fixture did not find its source" << navigation;
            ++failures;
            continue;
        }
        switch (navigation) {
        case 0: view.viewportTransform_.orbitByPixels(QPointF(45, -20)); break;
        case 1: view.viewportTransform_.zoomAt(QPointF(330, 245), 1.7, view.size()); break;
        case 2: view.viewportTransform_.panByPixels(QPointF(35, -20), view.size()); break;
        case 3: view.viewportTransform_.setPerspectiveEnabled(false); break;
        case 4: view.resize(800, 600); break;
        case 5: {
            auto preferences = view.viewportTransform_.cameraPreferences();
            preferences.focalLengthMillimeters = 65;
            view.viewportTransform_.setCameraPreferences(preferences);
            break;
        }
        case 6: {
            auto preferences = view.viewportTransform_.cameraPreferences();
            preferences.clipEnd = 2000;
            view.viewportTransform_.setCameraPreferences(preferences);
            break;
        }
        case 7: view.viewportTransform_.setViewPreset(ViewportViewPreset::Isometric); break;
        }
        if (navigation % 2 == 0) {
            // This is also called at the start of painting, so navigation
            // cannot leave an old orange preview visible without mouse motion.
            view.invalidateEraseGeometryCacheForView();
            if (view.eraseGeometryCachePrepared_ ||
                !view.eraseCandidateShapeIndices_.isEmpty() ||
                !view.eraseTargetCurveCaches_.isEmpty()) {
                qWarning() << "Navigation left a stale trim overlay" << navigation;
                ++failures;
            }
        }
        view.updateTrimHover(hoverPosition());
        bool aligned = view.eraseCandidateShapeIndices_.contains(sourceId) &&
                       view.trimHoverComponentIndex_ == 0;
        for (const auto &cache : view.eraseTargetCurveCaches_) {
            for (int sample = 0; sample < cache.sampled.parameters.size(); ++sample) {
                QPointF local;
                view.evaluateNurbsPoint(cache.curve, cache.sampled.parameters[sample], &local);
                const QPointF expected = view.viewportTransform_.workPlaneToScreen(
                    local, view.size(), frame);
                aligned &= QLineF(expected, cache.sampled.screenPoints[sample]).length() < 1.0e-7;
            }
            aligned &= !cache.previewIntervals.isEmpty();
        }
        if (!aligned) {
            qWarning() << "Trim preview missed the curve after navigation" << navigation;
            ++failures;
        }
        view.trimAtScreenPosition(hoverPosition());
        const int sourceIndex = view.objectIndex(sourceId);
        if (sourceIndex < 0 || view.shapes_.size() != 2 ||
            !workPlaneFramesMatch(shapeWorkPlaneFrame(view.shapes_[sourceIndex]), frame) ||
            view.shapes_[sourceIndex].nurbs.controlPoints.isEmpty() ||
            std::abs(view.shapes_[sourceIndex].nurbs.controlPoints.first().x()) > 1.0e-5 ||
            std::abs(view.shapes_[sourceIndex].nurbs.controlPoints.last().x() - 80) > 1.0e-5) {
            qWarning() << "Trim click failed to remove the left half after navigation" << navigation;
            ++failures;
        }
    }
    view.resize(640, 480);
    view.viewportTransform_.resetView();
    view.viewportTransform_.setCameraPreferences(ViewportCameraPreferences{});
    view.setTool(Tool::Select);

    // Rotate the circle's storage seam through every quadrant. The cut
    // must depend on the crossing line, never the circle construction point.
    for (int angle = 0; angle < 360; angle += 15) {
        const double radians = angle * 3.141592653589793 / 180.0;
        const auto circle = makeCircleNurbs({QPointF(0, 0),
            QPointF(100 * std::cos(radians), 100 * std::sin(radians))});
        const auto line = makeDegreeOneNurbs({QPointF(-150, 0), QPointF(150, 0)});
        view.shapes_ = {Shape{GeometryType::Circle, {}, circle, ArcMode::TwoPoint, 0, {}, {}},
                        Shape{GeometryType::Line, {}, line, ArcMode::TwoPoint, 0, {}, {}}};
        view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0),
                                      view.shapes_.objectIdAt(1)};
        view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
        view.prepareEraseGeometryCache();
        QVector<Shape> result;
        if (!view.trimShapeAtEraserStroke(view.shapes_[0],
                {view.worldToScreen(QPointF(0, -100))}, &result, 0,
                &view.eraseTargetCurveCaches_) || result.size() != 1) {
            ++failures;
            continue;
        }
        // Every point retained must be in the upper semicircle. This checks
        // the actual committed curve, including any fragment across the seam.
        for (const auto &curve : result.first().components) {
            const auto knots = view.expandedKnotVector(curve);
            const double a = knots[curve.degree], b = knots[curve.controlPoints.size()];
            for (int i = 0; i <= 100; ++i) {
                QPointF point;
                if (!view.evaluateNurbsPoint(curve, a + (b-a)*i/100, &point) ||
                    point.y() < -0.1 || std::abs(std::hypot(point.x(), point.y())-100) > 1e-6) {
                    qWarning() << "Bad retained geometry at seam angle" << angle << point;
                    ++failures;
                    break;
                }
            }
        }
    }

    const Shape::NurbsCurve2D firstPolyCurvePart =
        makeDegreeOneNurbs({QPointF(-100, 0), QPointF(100, 0)});
    const Shape::NurbsCurve2D secondPolyCurvePart =
        makeDegreeOneNurbs({QPointF(-100, 100), QPointF(100, 100)});
    Shape joinedCurves{GeometryType::PolyCurve,
                       view.polyCurvePoints({firstPolyCurvePart, secondPolyCurvePart}),
                       {},
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {firstPolyCurvePart, secondPolyCurvePart}};
    view.shapes_ = {joinedCurves};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    view.eraseGeometryCachePrepared_ = false;
    view.trimHoverPositionValid_ = false;
    view.prepareEraseGeometryCache();
    view.updateTrimHover(view.worldToScreen(QPointF(0, 0)));
    bool previewTargetsNearestComponent = view.trimHoverComponentIndex_ == 0;
    for (const EraseCurveSampleCache &cache : view.eraseTargetCurveCaches_) {
        if (cache.componentIndex == 0) {
            previewTargetsNearestComponent &= !cache.previewIntervals.isEmpty();
        } else if (cache.componentIndex == 1) {
            previewTargetsNearestComponent &= cache.previewIntervals.isEmpty();
        }
    }
    if (!previewTargetsNearestComponent) {
        qWarning() << "Point trim preview must target only the nearest PolyCurve component";
        ++failures;
    }

    view.applyEraseCandidates(nullptr, view.trimHoverComponentIndex_);
    if (view.shapes_.size() != 1 ||
        view.shapes_[0].geometryType != GeometryType::PolyCurve ||
        view.shapes_[0].components.size() != 1 ||
        view.shapes_[0].components[0].controlPoints !=
            secondPolyCurvePart.controlPoints) {
        qWarning() << "Point trim must leave other PolyCurve components untouched";
        ++failures;
    }

    const Shape sourceCircle{GeometryType::Circle,
                             {},
                             makeCircleNurbs({QPointF(0, 0), QPointF(100, 0)}),
                             ArcMode::TwoPoint,
                             0.0,
                             {},
                             {}};
    const Shape crossingLine{GeometryType::Line,
                             {},
                             makeDegreeOneNurbs({QPointF(-150, 0), QPointF(150, 0)}),
                             ArcMode::TwoPoint,
                             0.0,
                             {},
                             {}};
    view.shapes_ = {sourceCircle, crossingLine};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0),
                                  view.shapes_.objectIdAt(1)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    view.prepareEraseGeometryCache();
    QVector<Shape> erasedCircle;
    const bool erasedToUpperArc = view.trimShapeAtEraserStroke(
        view.shapes_[0],
        {view.worldToScreen(QPointF(0, -100))},
        &erasedCircle,
        0,
        &view.eraseTargetCurveCaches_);
    if (!erasedToUpperArc || erasedCircle.size() != 1 ||
        erasedCircle.first().geometryType != GeometryType::PolyCurve ||
        erasedCircle.first().components.isEmpty()) {
        qWarning() << "Erasing a circle half must produce its trimmed NURBS components";
        ++failures;
    } else {
        Document tangentDocument;
        tangentDocument.append(erasedCircle.first());
        SnapEngine tangentEngine;
        tangentEngine.setSettings(
            SnapSettings{true, false, false, false, false, false, true});
        const QPointF tangentOrigin(0, 200);
        const QVector<SnapCandidate> candidates = tangentEngine.tangentCandidates(
            tangentDocument,
            tangentOrigin,
            view.viewportTransform_,
            view.size());
        const QPointF expectedTangent(86.6025403784, 50.0);
        bool tangentFound = false;
        QPointF tangentPoint;
        for (const SnapCandidate &candidate : candidates) {
            if (std::hypot(candidate.point.x() - expectedTangent.x(),
                           candidate.point.y() - expectedTangent.y()) <= 0.1) {
                tangentFound = true;
                tangentPoint = candidate.point;
                break;
            }
        }
        const SnapResult snap = tangentEngine.findSnapPoint(
            tangentDocument,
            tangentPoint,
            true,
            {tangentOrigin},
            view.viewportTransform_,
            view.size());
        if (!tangentFound || snap.type != SnapType::Tangent) {
            qWarning() << "Tangent OSnap must find the tangent on an erased circular NURBS arc"
                       << candidates.size() << tangentPoint
                       << static_cast<int>(snap.type);
            ++failures;
        }
    }

    const auto circleWithSeam = [](const QPointF &center,
                                   qreal radius,
                                   qreal seamAngle) {
        const QPointF edge(center.x() + radius * std::cos(seamAngle),
                           center.y() + radius * std::sin(seamAngle));
        return Shape{GeometryType::Circle,
                     {center, edge},
                     makeCircleNurbs({center, edge}),
                     ArcMode::TwoPoint,
                     0.0,
                     {},
                     {}};
    };
    view.shapes_ = {circleWithSeam(QPointF(0, 0), 60.0, 0.31),
                    circleWithSeam(QPointF(-120, 0), 60.0, 0.83),
                    circleWithSeam(QPointF(120, 0), 60.0, 1.27)};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    view.prepareEraseGeometryCache();
    const EraseCurveSampleCache *tangentCircleCache = nullptr;
    for (const EraseCurveSampleCache &cache : view.eraseTargetCurveCaches_) {
        if (cache.shapeIndex == 0 && cache.componentIndex == 0) {
            tangentCircleCache = &cache;
            break;
        }
    }
    const QVector<qreal> tangentContacts = tangentCircleCache != nullptr
                                               ? tangentCircleCache->intersectionParameters
                                               : QVector<qreal>{};
    QVector<QPointF> tangentContactPoints;
    for (const qreal parameter : tangentContacts) {
        QPointF point;
        if (view.evaluateNurbsPoint(view.shapes_[0].nurbs, parameter, &point)) {
            tangentContactPoints.append(point);
        }
    }
    std::sort(tangentContactPoints.begin(), tangentContactPoints.end(),
              [](const QPointF &first, const QPointF &second) {
                  return first.x() < second.x();
              });
    bool foundBothTangencies = tangentContactPoints.size() == 2;
    if (foundBothTangencies) {
        foundBothTangencies =
            std::abs(tangentContactPoints[0].x() + 60.0) <= 0.1 &&
            std::abs(tangentContactPoints[0].y()) <= 0.1 &&
            std::abs(tangentContactPoints[1].x() - 60.0) <= 0.1 &&
            std::abs(tangentContactPoints[1].y()) <= 0.1;
    }
    if (!foundBothTangencies) {
        qWarning() << "Erase must recognize both tangent-only circle contacts"
                   << tangentContactPoints;
        ++failures;
    }

    QVector<Shape> tangentCircleRemainder;
    if (!view.trimShapeAtEraserStroke(
            view.shapes_[0],
            {view.worldToScreen(QPointF(0, 60))},
            &tangentCircleRemainder,
            0,
            &view.eraseTargetCurveCaches_) ||
        tangentCircleRemainder.size() != 1 ||
        tangentCircleRemainder.first().components.isEmpty()) {
        qWarning() << "Erasing between two tangent contacts must preserve the rest of the circle";
        ++failures;
    } else {
        const Shape::NurbsCurve2D &remainder =
            tangentCircleRemainder.first().components.first();
        const QVector<double> knots = view.expandedKnotVector(remainder);
        const qreal start = knots[remainder.degree];
        const qreal end = knots[remainder.controlPoints.size()];
        bool keptLowerArc = true;
        for (int sample = 0; sample <= 100; ++sample) {
            QPointF point;
            const qreal parameter = start + (end - start) * sample / 100.0;
            if (!view.evaluateNurbsPoint(remainder, parameter, &point) ||
                point.y() > 0.1 ||
                std::abs(std::hypot(point.x(), point.y()) - 60.0) > 0.1) {
                keptLowerArc = false;
                break;
            }
        }
        if (!keptLowerArc) {
            qWarning() << "Tangency-bounded erase must retain the lower circular arc";
            ++failures;
        }
    }

    const auto worldPoint = [&view](qreal x, qreal y) {
        return view.screenToWorld(QPointF(x, y));
    };
    Shape arc{GeometryType::Arc,
              {worldPoint(249, 169), worldPoint(391, 311), worldPoint(391, 169)},
              {},
              ArcMode::TwoPoint,
              0.0,
              {},
              {}};
    arc.nurbs = view.makeArcNurbsCurve(arc);
    Shape line{GeometryType::Line,
               {worldPoint(178, 98), worldPoint(462, 382)},
               makeDegreeOneNurbs({worldPoint(178, 98), worldPoint(462, 382)}),
               ArcMode::TwoPoint,
               0.0,
               {},
               {}};
    view.shapes_ = {arc, line};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0),
                                  view.shapes_.objectIdAt(1)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    view.prepareEraseGeometryCache();
    const QVector<QPointF> middleStroke{QPointF(300, 220), QPointF(340, 260)};
    const auto checkMiddleLineSection = [&view, &failures](
        const QVector<QPointF> &stroke, const char *label) {
        if (view.eraseTargetCurveCaches_.size() < 2) {
            ++failures;
            return;
        }
        const auto &lineTarget = view.eraseTargetCurveCaches_[1];
        const auto hit = view.eraserIntervalsForCurve(lineTarget.curve, stroke);
        const auto bounded = view.boundEraseIntervals(
            lineTarget.curve, hit, lineTarget.intersectionParameters);
        if (bounded.size() != 1 ||
            std::abs(bounded.first().start - 0.25) > 1.0e-6 ||
            std::abs(bounded.first().end - 0.75) > 1.0e-6) {
            qWarning() << "Wrong intersection-bounded erase interval" << label
                       << "count" << bounded.size();
            ++failures;
        }
    };
    checkMiddleLineSection(middleStroke, "middle");
    // The brush overlaps the first intersection by a few pixels, but the
    // stroke is centered in the middle section. The neighboring section must
    // remain intact.
    checkMiddleLineSection({QPointF(255, 175), QPointF(300, 220)},
                           "near intersection");

    // Simulate the tiny gap left by a previous sampled circle cut. The arc
    // lies entirely on one side of the line, so strict crossing misses it.
    for (auto &point : view.shapes_[0].nurbs.controlPoints) {
        point += QPointF(0.02, 0.02);
    }
    view.prepareEraseGeometryCache();
    checkMiddleLineSection(middleStroke, "trimmed endpoint contact");
    QVector<Shape> remaining;
    if (!view.trimShapeAtEraserStroke(view.shapes_[1], middleStroke,
                                     &remaining, 1, &view.eraseTargetCurveCaches_) ||
        remaining.size() != 2 ||
        remaining[0].geometryType != GeometryType::Line ||
        remaining[1].geometryType != GeometryType::Line ||
        !view.isValidNurbsCurve(remaining[0].nurbs) ||
        !view.isValidNurbsCurve(remaining[1].nurbs)) {
        qWarning() << "Middle cut must produce two independently selectable line tails";
        ++failures;
    }

    const ObjectId originalLineId = view.shapes_.objectIdAt(1);
    const LayerId originalLineLayer = view.document_.object(originalLineId)->layerId;
    view.history_.clear();
    view.eraseCandidateShapeIndices_ = {originalLineId};
    view.eraseStrokeScreenPath_ = middleStroke;
    view.applyEraseCandidates();
    if (view.shapes_.size() != 3 ||
        view.shapes_.objectIdAt(1) != originalLineId ||
        view.shapes_.objectIdAt(2) == originalLineId ||
        view.document_.object(view.shapes_.objectIdAt(2))->layerId != originalLineLayer ||
        view.shapes_[1].geometryType != GeometryType::Line ||
        view.shapes_[2].geometryType != GeometryType::Line) {
        qWarning() << "Trim must insert the second line tail as a separate same-layer object";
        ++failures;
    }
    view.undo();
    if (view.shapes_.size() != 2 || view.shapes_.objectIdAt(1) != originalLineId) {
        qWarning() << "Undo must restore the original object after a multi-piece trim";
        ++failures;
    }

    Shape joined{GeometryType::PolyCurve,
                 view.polyCurvePoints({arc.nurbs, line.nurbs}),
                 Shape::NurbsCurve2D{},
                 ArcMode::TwoPoint,
                 0.0,
                 {},
                 {arc.nurbs, line.nurbs}};
    view.shapes_ = {joined};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    if (view.explodeSelectedShapes() != 2 ||
        view.shapes_.size() != 2 ||
        view.shapes_[0].geometryType != GeometryType::PolyCurve ||
        view.shapes_[1].geometryType != GeometryType::PolyCurve ||
        view.shapes_[0].components.size() != 1 ||
        view.shapes_[1].components.size() != 1 ||
        view.selectedShapeIndices_.size() != 2) {
        qWarning() << "Explode must create separate selected component shapes";
        ++failures;
    }

    Shape rotateFirst{GeometryType::Line,
                      {QPointF(1, 0), QPointF(2, 0)},
                      makeDegreeOneNurbs({QPointF(1, 0), QPointF(2, 0)}),
                      ArcMode::TwoPoint,
                      0.0,
                      {},
                      {}};
    Shape rotateSecond{GeometryType::Line,
                       {QPointF(0, 1), QPointF(0, 2)},
                       makeDegreeOneNurbs({QPointF(0, 1), QPointF(0, 2)}),
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
    view.shapes_ = {rotateFirst, rotateSecond};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0),
                                  view.shapes_.objectIdAt(1)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(1);
    const bool rotateStarted = view.beginRotate();
    view.handleRotatePoint(QPointF(0, 0));
    view.handleRotatePoint(QPointF(1, 0));
    view.handleRotatePoint(QPointF(0, 1));
    const auto near = [](qreal first, qreal second) {
        return std::abs(first - second) <= 1.0e-9;
    };
    if (!rotateStarted || view.activeTool_ != Tool::Select ||
        view.shapes_.size() != 2 ||
        !near(view.shapes_[0].nurbs.controlPoints[0].x(), 0.0) ||
        !near(view.shapes_[0].nurbs.controlPoints[0].y(), 1.0) ||
        !near(view.shapes_[1].nurbs.controlPoints[0].x(), -1.0) ||
        !near(view.shapes_[1].nurbs.controlPoints[0].y(), 0.0)) {
        qWarning() << "Rotate must apply one angle to every selected shape";
        ++failures;
    }

    Shape scaleLine{GeometryType::Line,
                    {QPointF(0, 0), QPointF(2, 3)},
                    makeDegreeOneNurbs({QPointF(0, 0), QPointF(2, 3)}),
                    ArcMode::TwoPoint,
                    0.0,
                    {},
                    {}};
    view.shapes_ = {scaleLine};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    const bool scaleOneDStarted = view.beginScale(ScaleMode::OneD);
    view.handleScalePoint(QPointF(0, 0));
    view.handleScalePoint(QPointF(2, 0));
    view.updateScalePreview(QPointF(4, 0));
    const bool scalePreviewIsNonDestructive =
        near(view.shapes_[0].nurbs.controlPoints[1].x(), 2.0) &&
        near(view.shapes_[0].nurbs.controlPoints[1].y(), 3.0);
    view.handleScalePoint(QPointF(4, 0));
    if (!scaleOneDStarted || !scalePreviewIsNonDestructive ||
        view.activeTool_ != Tool::Select ||
        !near(view.shapes_[0].nurbs.controlPoints[1].x(), 4.0) ||
        !near(view.shapes_[0].nurbs.controlPoints[1].y(), 3.0)) {
        qWarning() << "Scale 1D must preview without mutation and stretch only along its picked axis";
        ++failures;
    }

    scaleLine = Shape{GeometryType::Line,
                      {QPointF(1, 1), QPointF(2, 1)},
                      makeDegreeOneNurbs({QPointF(1, 1), QPointF(2, 1)}),
                      ArcMode::TwoPoint,
                      0.0,
                      {},
                      {}};
    view.shapes_ = {scaleLine};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    const bool scaleTwoDStarted = view.beginScale(ScaleMode::TwoD);
    view.handleScalePoint(QPointF(0, 0));
    view.handleScalePoint(QPointF(1, 0));
    view.handleScalePoint(QPointF(0, 2));
    if (!scaleTwoDStarted || view.activeTool_ != Tool::Select ||
        !near(view.shapes_[0].nurbs.controlPoints[0].x(), 2.0) ||
        !near(view.shapes_[0].nurbs.controlPoints[0].y(), 2.0) ||
        !near(view.shapes_[0].nurbs.controlPoints[1].x(), 4.0) ||
        !near(view.shapes_[0].nurbs.controlPoints[1].y(), 2.0)) {
        qWarning() << "Scale 2D must resize equally in X and Y around its base point";
        ++failures;
    }

    view.shapes_ = {scaleLine};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    const bool scaleTypedOneDStarted = view.beginScale(ScaleMode::OneD);
    view.handleScalePoint(QPointF(0, 0));
    QKeyEvent typedFactor(QEvent::KeyPress,
                          Qt::Key_2,
                          Qt::NoModifier,
                          QStringLiteral("2"));
    QKeyEvent confirmFactor(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    view.keyPressEvent(&typedFactor);
    view.keyPressEvent(&confirmFactor);
    view.handleScalePoint(QPointF(1, 0));
    if (!scaleTypedOneDStarted || view.activeTool_ != Tool::Select ||
        !near(view.shapes_[0].nurbs.controlPoints[1].x(), 4.0) ||
        !near(view.shapes_[0].nurbs.controlPoints[1].y(), 1.0)) {
        qWarning() << "Scale 1D must accept a typed factor and a picked scale direction";
        ++failures;
    }

    Shape mirrorSource{GeometryType::Line,
                       {QPointF(1, 2), QPointF(3, 2)},
                       makeDegreeOneNurbs({QPointF(1, 2), QPointF(3, 2)}),
                       ArcMode::TwoPoint,
                       0.0,
                       {},
                       {}};
    view.shapes_ = {mirrorSource};
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    view.setSnapModes(true, false, false, false, false, false, false, false);
    view.setOsnapEnabled(true);
    const bool endpointMirrorStarted = view.beginMirror();
    const QPointF snappedAxisStart = view.constrainLinePoint(QPointF(1.1, 2.0));
    if (!endpointMirrorStarted ||
        std::hypot(snappedAxisStart.x() - 1.0,
                   snappedAxisStart.y() - 2.0) > 1.0e-9) {
        qWarning() << "Mirror axis must use endpoint snapping";
        ++failures;
    }
    view.cancelMirror();

    view.setOsnapEnabled(false);
    view.setOrthoEnabled(true);
    view.selectedShapeIndices_ = {view.shapes_.objectIdAt(0)};
    view.selectedShapeIndex_ = view.shapes_.objectIdAt(0);
    const bool mirrorStarted = view.beginMirror();
    view.handleMirrorPoint(QPointF(0, 0));
    const QPointF orthoAxisEnd = view.constrainLinePoint(QPointF(1, 4));
    view.handleMirrorPoint(orthoAxisEnd);
    if (!mirrorStarted || view.activeTool_ != Tool::Select ||
        view.shapes_.size() != 2 ||
        !near(view.shapes_[1].nurbs.controlPoints[0].x(), -1.0) ||
        !near(view.shapes_[1].nurbs.controlPoints[0].y(), 2.0) ||
        !near(view.shapes_[1].nurbs.controlPoints[1].x(), -3.0) ||
        !near(view.shapes_[1].nurbs.controlPoints[1].y(), 2.0)) {
        qWarning() << "Mirror must preserve the source and reflect the selected NURBS across an ortho axis";
        ++failures;
    }
    view.setOrthoEnabled(false);

    view.setTool(Tool::Select);
    view.draggingSelected_ = true;
    view.dragAxisLock_ = DragAxisLock::None;
    QKeyEvent xPress(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
    view.keyPressEvent(&xPress);
    const QPointF xLockedDelta = view.constrainDragDelta(QPointF(5.0, 7.0));
    QKeyEvent yPress(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier);
    view.keyPressEvent(&yPress);
    const QPointF yLockedDelta = view.constrainDragDelta(QPointF(5.0, 7.0));
    QKeyEvent yRelease(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier);
    view.keyPressEvent(&yRelease);
    const QPointF unlockedDelta = view.constrainDragDelta(QPointF(5.0, 7.0));
    view.draggingSelected_ = false;
    if (xLockedDelta != QPointF(5.0, 0.0) ||
        yLockedDelta != QPointF(0.0, 7.0) ||
        unlockedDelta != QPointF(5.0, 7.0)) {
        qWarning() << "X and Y must constrain selected-object movement independently of Ortho";
        ++failures;
    }

    view.draggingSelected_ = false;
    view.dragAxisLock_ = DragAxisLock::None;
    QKeyEvent gPress(QEvent::KeyPress, Qt::Key_G, Qt::NoModifier);
    view.keyPressEvent(&gPress);
    const bool grabStarted = view.grabActive_ && view.draggingSelected_;
    QKeyEvent grabXPress(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
    view.keyPressEvent(&grabXPress);
    const bool grabAxisLocked = view.grabActive_ && view.dragAxisLock_ == DragAxisLock::X;
    QKeyEvent grabEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    view.keyPressEvent(&grabEscape);
    if (!grabStarted || !grabAxisLocked || view.grabActive_ || view.draggingSelected_) {
        qWarning() << "G must start a cancelable grab with X/Y axis locking";
        ++failures;
    }

    qInfo() << "Trim seam failures:" << failures;
    return failures ? 1 : 0;
}
