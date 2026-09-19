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
    qInfo() << "Trim seam failures:" << failures;
    return failures ? 1 : 0;
}
