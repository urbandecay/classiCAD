#include <QApplication>
#define private public
#define protected public
#define main classicad_app_main
#include "../src/main.cpp"
#undef main
#undef protected
#undef private

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ViewportWidget view;
    view.resize(640, 480);
    int failures = 0;
    // Rotate the circle's storage seam through every quadrant. The cut
    // must depend on the crossing line, never the circle construction point.
    for (int angle = 0; angle < 360; angle += 15) {
        const double radians = angle * 3.141592653589793 / 180.0;
        const auto circle = makeCircleNurbs({QPointF(0, 0),
            QPointF(100 * std::cos(radians), 100 * std::sin(radians))});
        const auto line = makeDegreeOneNurbs({QPointF(-150, 0), QPointF(150, 0)});
        view.shapes_ = {Shape{Tool::Circle, {}, circle, ArcMode::TwoPoint, 0, {}, {}},
                        Shape{Tool::Line, {}, line, ArcMode::TwoPoint, 0, {}, {}}};
        view.selectedShapeIndices_ = {0, 1};
        view.selectedShapeIndex_ = 0;
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
    const auto worldPoint = [&view](qreal x, qreal y) {
        return view.screenToWorld(QPointF(x, y));
    };
    Shape arc{Tool::Arc,
              {worldPoint(249, 169), worldPoint(391, 311), worldPoint(391, 169)},
              {},
              ArcMode::TwoPoint,
              0.0,
              {},
              {}};
    arc.nurbs = view.makeArcNurbsCurve(arc);
    Shape line{Tool::Line,
               {worldPoint(178, 98), worldPoint(462, 382)},
               makeDegreeOneNurbs({worldPoint(178, 98), worldPoint(462, 382)}),
               ArcMode::TwoPoint,
               0.0,
               {},
               {}};
    view.shapes_ = {arc, line};
    view.selectedShapeIndices_ = {0, 1};
    view.selectedShapeIndex_ = 0;
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
        remaining.size() != 1 || remaining.first().components.size() != 2) {
        qWarning() << "Middle cut must preserve both outer line tails";
        ++failures;
    }

    Shape joined{Tool::PolyCurve,
                 view.polyCurvePoints({arc.nurbs, line.nurbs}),
                 Shape::NurbsCurve2D{},
                 ArcMode::TwoPoint,
                 0.0,
                 {},
                 {arc.nurbs, line.nurbs}};
    view.shapes_ = {joined};
    view.selectedShapeIndices_ = {0};
    view.selectedShapeIndex_ = 0;
    if (view.explodeSelectedShapes() != 2 ||
        view.shapes_.size() != 2 ||
        view.shapes_[0].tool != Tool::PolyCurve ||
        view.shapes_[1].tool != Tool::PolyCurve ||
        view.shapes_[0].components.size() != 1 ||
        view.shapes_[1].components.size() != 1 ||
        view.selectedShapeIndices_.size() != 2) {
        qWarning() << "Explode must create separate selected component shapes";
        ++failures;
    }

    qInfo() << "Trim seam failures:" << failures;
    return failures ? 1 : 0;
}
