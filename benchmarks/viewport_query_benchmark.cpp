#include "core/document/document.h"
#include "core/geometry/curve_construction.h"
#include "services/hit_testing/curve_hit_tester.h"
#include "services/hit_testing/selection_box_query.h"
#include "services/sampling/curve_sampler.h"
#include "services/viewport/viewport_transform.h"

#include <QElapsedTimer>

#include <algorithm>
#include <iostream>
#include <vector>

using namespace classiCAD;

namespace {

double medianMilliseconds(int rounds, const auto &operation)
{
    std::vector<double> durations;
    durations.reserve(rounds);
    for (int round = 0; round < rounds; ++round) {
        QElapsedTimer timer;
        timer.start();
        operation();
        durations.push_back(timer.nsecsElapsed() / 1.0e6);
    }
    std::sort(durations.begin(), durations.end());
    return durations[durations.size() / 2];
}

Document makeScene(int curveCount)
{
    Document document;
    for (int index = 0; index < curveCount; ++index) {
        const qreal y = (static_cast<qreal>(index) - curveCount / 2.0) * 0.2;
        Shape shape;
        shape.geometryType = GeometryType::Nurbs;
        shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
        shape.nurbs = makeDegreeOneNurbs(
            {QPointF(-80.0, y), QPointF(80.0, y)});
        document.append(shape);
    }
    return document;
}

void benchmarkScene(int curveCount)
{
    constexpr QSize viewportSize{1280, 720};
    constexpr int rounds = 9;
    const Document document = makeScene(curveCount);
    ViewportTransform transform;
    transform.setViewPreset(ViewportViewPreset::Top);
    transform.zoom() = 12.0;
    CurveSampler sampler;
    CurveHitTester hitTester;
    quint64 checksum = 0;
    const QPointF cursor(viewportSize.width() * 0.5,
                         viewportSize.height() * 0.5);
    const QRectF selectionBox(cursor - QPointF(70.0, 70.0),
                              cursor + QPointF(70.0, 70.0));

    const double sceneHitTestMilliseconds = medianMilliseconds(rounds, [&]() {
        checksum += static_cast<quint64>(hitTester.hitTestShape(
            document, cursor, transform, viewportSize, true) + 1);
    });

    const double sceneBoxQueryMilliseconds = medianMilliseconds(rounds, [&]() {
        int matches = 0;
        for (const Shape &shape : document) {
            const SelectionBoxGeometryResult result =
                queryCurveOrPointSelectionBox(shape,
                                              selectionBox,
                                              true,
                                              sampler,
                                              transform,
                                              viewportSize);
            matches += result.applies && result.matches;
        }
        checksum += static_cast<quint64>(matches);
    });

    std::cout << "visible curves=" << curveCount
              << " viewport hit-test=" << sceneHitTestMilliseconds << " ms"
              << " selection-box query=" << sceneBoxQueryMilliseconds << " ms"
              << " checksum=" << checksum << '\n';
}

} // namespace

int main()
{
    for (const int curveCount : {64, 256, 1024, 4096}) {
        benchmarkScene(curveCount);
    }
    return 0;
}
