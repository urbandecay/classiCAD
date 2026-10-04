#include "core/document/document.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/shape_mapping.h"
#include "services/erase/curve_erase_query.h"

#include <QElapsedTimer>

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using namespace classiCAD;

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
        Shape shape;
        shape.geometryType = GeometryType::Nurbs;
        const qreal y = static_cast<qreal>(index) * 3.0;
        shape.nurbs = makeDegreeOneNurbs({QPointF(0.0, y), QPointF(10.0, y)});
        document.append(shape);
    }
    return document;
}

QVector<EraseCurveSampleCache> makeCurveCache(const Document &document)
{
    QVector<EraseCurveSampleCache> cache;
    cache.reserve(document.size());
    for (int index = 0; index < document.size(); ++index) {
        EraseCurveSampleCache curve;
        curve.shapeIndex = index;
        curve.componentIndex = 0;
        curve.workPlaneFrame = shapeComponentWorkPlaneFrame(document[index], 0);
        curve.curve = document[index].nurbs;
        cache.append(std::move(curve));
    }
    return cache;
}

void resolveCurve(const Document &document,
                  int index,
                  const EraseIntersectionCandidates &candidates,
                  quint64 *checksum)
{
    const Shape &shape = document[index];
    const EraseIntersectionParameterResult result =
        findEraseIntersectionParameters(
            shape.nurbs,
            shapeComponentWorkPlaneFrame(shape, 0),
            document.objectIdAt(index),
            0,
            candidates.curves,
            candidates.points);
    *checksum += static_cast<quint64>(result.parameters.size()) +
                 static_cast<quint64>(result.intersectingObjectIds.size()) +
                 static_cast<quint64>(result.nurbsSeedSolves);
}

void resolveSelectedCurves(const Document &document,
                           int targetCount,
                           const EraseIntersectionCandidates &candidates,
                           quint64 *checksum)
{
    for (int index = 0; index < targetCount; ++index) {
        resolveCurve(document, index, candidates, checksum);
    }
}

void benchmarkScene(int curveCount, int targetCount)
{
    const Document document = makeScene(curveCount);
    const QVector<EraseCurveSampleCache> sceneCache = makeCurveCache(document);
    quint64 checksum = 0;

    const double candidateBuildMilliseconds = medianMilliseconds(7, [&]() {
        const EraseIntersectionCandidates candidates =
            makeEraseIntersectionCandidates(document, &sceneCache);
        checksum += static_cast<quint64>(candidates.curves.size());
    });

    const double reuseCandidatesMilliseconds = medianMilliseconds(7, [&]() {
        const EraseIntersectionCandidates candidates =
            makeEraseIntersectionCandidates(document, &sceneCache);
        resolveSelectedCurves(document, targetCount, candidates, &checksum);
    });

    const double rebuildPerTargetMilliseconds = medianMilliseconds(7, [&]() {
        for (int index = 0; index < targetCount; ++index) {
            const EraseIntersectionCandidates candidates =
                makeEraseIntersectionCandidates(document, &sceneCache);
            resolveCurve(document, index, candidates, &checksum);
        }
    });

    std::cout << "visible curves=" << curveCount
              << " selected targets=" << targetCount
              << " candidate build=" << candidateBuildMilliseconds << " ms"
              << "; reused scene=" << reuseCandidatesMilliseconds << " ms"
              << "; rebuilt per target=" << rebuildPerTargetMilliseconds << " ms"
              << "; ratio=" << rebuildPerTargetMilliseconds /
                                   std::max(1.0e-9,
                                            reuseCandidatesMilliseconds)
              << "x; checksum=" << checksum << '\n';
}

} // namespace

int main()
{
    for (const auto &[curveCount, targetCount] :
         {std::pair{16, 2}, std::pair{64, 8}, std::pair{256, 16}}) {
        benchmarkScene(curveCount, targetCount);
    }
    return 0;
}
