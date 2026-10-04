#include "core/geometry/nurbs_surface_evaluator.h"
#include "core/geometry/nurbs_surface_tessellator.h"
#include "services/sampling/surface_tessellation_cache.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

using namespace classiCAD;

QVector<double> reducedClampedKnots(int controlPointCount, int degree)
{
    QVector<double> fullKnots;
    fullKnots.reserve(controlPointCount + degree + 1);
    for (int index = 0; index <= degree; ++index) {
        fullKnots.append(0.0);
    }
    const int interiorCount = controlPointCount - degree - 1;
    for (int index = 1; index <= interiorCount; ++index) {
        fullKnots.append(static_cast<double>(index) /
                         static_cast<double>(interiorCount + 1));
    }
    for (int index = 0; index <= degree; ++index) {
        fullKnots.append(1.0);
    }
    return fullKnots.mid(1, fullKnots.size() - 2);
}

NurbsSurface3D makeSurface(int controlPointCount)
{
    NurbsSurface3D surface;
    surface.degreeU = 3;
    surface.degreeV = 3;
    surface.orderU = 4;
    surface.orderV = 4;
    surface.controlVertexCountU = controlPointCount;
    surface.controlVertexCountV = controlPointCount;
    surface.knotsU = reducedClampedKnots(controlPointCount, surface.degreeU);
    surface.knotsV = reducedClampedKnots(controlPointCount, surface.degreeV);
    surface.rational = true;
    surface.controlPoints.reserve(controlPointCount * controlPointCount);
    surface.weights.reserve(controlPointCount * controlPointCount);
    for (int uIndex = 0; uIndex < controlPointCount; ++uIndex) {
        const double u = static_cast<double>(uIndex) /
                         static_cast<double>(controlPointCount - 1);
        for (int vIndex = 0; vIndex < controlPointCount; ++vIndex) {
            const double v = static_cast<double>(vIndex) /
                             static_cast<double>(controlPointCount - 1);
            surface.controlPoints.append(
                {u, v, 0.2 * std::sin(u * 6.0) * std::cos(v * 5.0)});
            surface.weights.append(0.75 + 0.5 *
                (static_cast<double>((uIndex + 2 * vIndex) % 11) / 10.0));
        }
    }
    return surface;
}

template <typename Operation>
double medianMilliseconds(int rounds, Operation operation)
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

void benchmarkSurface(int controlPointCount)
{
    const NurbsSurface3D surface = makeSurface(controlPointCount);
    constexpr int evaluationCount = 1200;
    constexpr int rounds = 5;
    qreal coordinateChecksum = 0.0;

    const double checkedMilliseconds = medianMilliseconds(rounds, [&]() {
        qreal checksum = 0.0;
        for (int index = 0; index < evaluationCount; ++index) {
            Point3D point;
            const qreal u = static_cast<qreal>((index * 37) % 997) / 997.0;
            const qreal v = static_cast<qreal>((index * 71) % 991) / 991.0;
            if (evaluateNurbsSurfacePoint(surface, u, v, &point)) {
                checksum += point.x + point.y + point.z;
            }
        }
        coordinateChecksum += checksum;
    });

    PreparedNurbsSurfaceEvaluator evaluator;
    if (!evaluator.prepare(surface)) {
        std::cerr << "Failed to prepare " << controlPointCount
                  << "x" << controlPointCount << " benchmark surface\n";
        return;
    }
    const double preparedMilliseconds = medianMilliseconds(rounds, [&]() {
        qreal checksum = 0.0;
        for (int index = 0; index < evaluationCount; ++index) {
            Point3D point;
            const qreal u = static_cast<qreal>((index * 37) % 997) / 997.0;
            const qreal v = static_cast<qreal>((index * 71) % 991) / 991.0;
            if (evaluator.evaluate(u, v, &point)) {
                checksum += point.x + point.y + point.z;
            }
        }
        coordinateChecksum += checksum;
    });

    const double tessellationMilliseconds = medianMilliseconds(3, [&]() {
        PreparedNurbsSurfaceTessellation tessellation;
        if (tessellation.prepare(surface)) {
            coordinateChecksum += tessellation.vertices().size();
        }
    });

    SurfaceTessellationCache cache;
    constexpr ObjectId objectId = ObjectId::fromValue(1);
    const auto cached = cache.acquire(objectId, 1, surface);
    if (cached.isNull()) {
        std::cerr << "Failed to prepare cached benchmark surface\n";
        return;
    }
    const double cacheLookupMilliseconds = medianMilliseconds(rounds, [&]() {
        for (int index = 0; index < evaluationCount; ++index) {
            const auto result = cache.acquire(objectId, 1, surface);
            coordinateChecksum += result->vertices().size() * 1.0e-12;
        }
    });

    std::cout << controlPointCount << "x" << controlPointCount
              << " CV surface, " << evaluationCount << " evaluations: checked="
              << checkedMilliseconds << " ms, prepared=" << preparedMilliseconds
              << " ms, ratio=" << checkedMilliseconds / preparedMilliseconds
              << "x; tessellation=" << tessellationMilliseconds
              << " ms; " << evaluationCount << " cache lookups="
              << cacheLookupMilliseconds << " ms; checksum="
              << coordinateChecksum << '\n';
}

} // namespace

int main()
{
    for (const int controlPointCount : {4, 12, 24}) {
        benchmarkSurface(controlPointCount);
    }
    return 0;
}
