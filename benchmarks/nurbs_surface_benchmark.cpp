#include "core/geometry/nurbs_surface_evaluator.h"
#include "core/geometry/curve_construction.h"
#include "core/geometry/nurbs_surface_factory.h"
#include "core/geometry/nurbs_solid.h"
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

void benchmarkMovingCylinder()
{
    NurbsSurface3D cap;
    NurbsExtrusionSolid3D solid;
    if (!makeNurbsPlanarFillSurface(makeCircleNurbs({{0, 0}, {3, 0}}),
                                   makeWorkPlaneFrame(WorkPlane::XY), &cap) ||
        !makeNurbsExtrusionSolid(cap, {0, 0, 5}, &solid)) {
        std::cerr << "Failed to create cylinder benchmark\n";
        return;
    }
    const auto faces = nurbsSolidFaces(solid);
    constexpr int frames = 12;
    double checksum = 0;
    const auto moved = [](NurbsSurface3D face, int frame) {
        for (auto &point : face.controlPoints) {
            point.x += frame * 0.137;
            point.y -= frame * 0.053;
        }
        return face;
    };
    const double rebuilt = medianMilliseconds(3, [&]() {
        for (int frame = 1; frame <= frames; ++frame) {
            for (const auto &face : faces) {
                PreparedNurbsSurfaceTessellation tessellation;
                tessellation.prepare(moved(face, frame));
                checksum += tessellation.vertices().size();
            }
        }
    });
    SurfaceTessellationCache cache;
    for (int i = 0; i < faces.size(); ++i) {
        cache.acquire(ObjectId::fromValue(1), 1, faces[i], i);
    }
    const double dragged = medianMilliseconds(5, [&]() {
        for (int frame = 1; frame <= frames; ++frame) {
            for (int i = 0; i < faces.size(); ++i) {
                const auto mesh = cache.acquire(ObjectId::fromValue(1),
                    frame + 2, moved(faces[i], frame), i);
                checksum += mesh->vertices().size();
            }
        }
    });
    const double preview = medianMilliseconds(5, [&]() {
        for (int frame = 1; frame <= frames; ++frame) {
            for (const auto &face : faces) {
                const auto mesh = cache.acquire(ObjectId::invalid(), 0,
                                                moved(face, frame));
                checksum += mesh->vertices().size();
            }
        }
    });
    std::cout << "Cylinder, " << frames << " movement frames: rebuild="
              << rebuilt << " ms; drag=" << dragged << " ms; duplicate preview="
              << preview << " ms; checksum=" << checksum << '\n';
}

NurbsSurface3D makeBenchmarkTrimmedFace()
{
    const auto boundary = makeDegreeOneNurbs({
        {-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}, {-1.0, -1.0}});
    NurbsSurface3D surface;
    if (!makeNurbsPlanarFillSurface(boundary,
                                   makeWorkPlaneFrame(WorkPlane::XY),
                                   &surface)) {
        return {};
    }
    return surface;
}

NurbsSurface3D deformBenchmarkFace(const NurbsSurface3D &source, int frame)
{
    NurbsSurface3D result = source;
    // Move one corner control point as a Grab drag would. The trim stays fixed
    // in UV, while the authoritative patch deforms underneath it.
    result.controlPoints[3].z += frame * 0.0025;
    result.controlPoints[3].x += frame * 0.0007;
    return result;
}

bool samePoint(const Point3D &a, const Point3D &b, double tolerance)
{
    return std::abs(a.x - b.x) <= tolerance &&
           std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

bool sameTessellation(const PreparedNurbsSurfaceTessellation &a,
                      const PreparedNurbsSurfaceTessellation &b)
{
    constexpr double tolerance = 1.0e-10;
    if (a.vertices().size() != b.vertices().size() ||
        a.triangles() != b.triangles() ||
        a.wireframe().size() != b.wireframe().size()) {
        return false;
    }
    for (int i = 0; i < a.vertices().size(); ++i) {
        if (!samePoint(a.vertices()[i], b.vertices()[i], tolerance)) {
            return false;
        }
    }
    for (int lineIndex = 0; lineIndex < a.wireframe().size(); ++lineIndex) {
        const auto &first = a.wireframe()[lineIndex];
        const auto &second = b.wireframe()[lineIndex];
        if (first.points.size() != second.points.size()) {
            return false;
        }
        for (int pointIndex = 0; pointIndex < first.points.size(); ++pointIndex) {
            if (!samePoint(first.points[pointIndex], second.points[pointIndex],
                           tolerance)) {
                return false;
            }
        }
    }
    return true;
}

bool benchmarkMovingTrimmedFace()
{
    const NurbsSurface3D base = makeBenchmarkTrimmedFace();
    if (!validateNurbsSurface(base)) {
        std::cerr << "Failed to create trimmed planar face benchmark\n";
        return false;
    }

    constexpr ObjectId objectId = ObjectId::fromValue(42);
    SurfaceTessellationCache cache;
    if (cache.acquire(objectId, 1, base).isNull()) {
        std::cerr << "Failed to prepare trimmed planar face cache\n";
        return false;
    }
    // The first deformation changes the affine face into a curved patch and
    // establishes its generic tessellation. Later frames measure the Blender-
    // style precomputed-basis update path.
    if (cache.acquire(objectId, 2, deformBenchmarkFace(base, 1)).isNull()) {
        std::cerr << "Failed to initialize deformed trimmed face cache\n";
        return false;
    }

    const NurbsSurface3D checkSurface = deformBenchmarkFace(base, 8);
    SurfaceTessellationCache::AcquisitionStats checkStats;
    const auto cachedCheck = cache.acquire(objectId, 9, checkSurface, 0,
                                           &checkStats);
    PreparedNurbsSurfaceTessellation rebuiltCheck;
    if (cachedCheck.isNull() ||
        checkStats.path != SurfaceTessellationCache::AcquisitionStats::Path::TopologyHit ||
        !rebuiltCheck.prepare(checkSurface) ||
        !sameTessellation(*cachedCheck, rebuiltCheck)) {
        std::cerr << "Prepared-basis face update did not match a fresh rebuild\n";
        return false;
    }

    constexpr int frames = 60;
    double checksum = 0.0;
    const double rebuildMilliseconds = medianMilliseconds(5, [&]() {
        for (int frame = 1; frame <= frames; ++frame) {
            PreparedNurbsSurfaceTessellation tessellation;
            if (tessellation.prepare(deformBenchmarkFace(base, frame))) {
                checksum += tessellation.vertices().size();
            }
        }
    });

    // Reproduce the previous cache-update algorithm for an apples-to-apples
    // comparison: prepare the surface evaluator, then reevaluate every saved
    // mesh and wireframe UV sample on each drag frame.
    const double previousEvaluatorMilliseconds = medianMilliseconds(5, [&]() {
        for (int frame = 1; frame <= frames; ++frame) {
            const NurbsSurface3D moved = deformBenchmarkFace(base, frame);
            PreparedNurbsSurfaceEvaluator evaluator;
            if (!evaluator.prepare(moved)) {
                continue;
            }
            for (const QPointF &parameter : cachedCheck->vertexParameters()) {
                Point3D point;
                if (evaluator.evaluate(parameter.x(), parameter.y(), &point)) {
                    checksum += point.z;
                }
            }
            for (const auto &line : cachedCheck->wireframe()) {
                for (const QPointF &parameter : line.parameters) {
                    Point3D point;
                    if (evaluator.evaluate(parameter.x(), parameter.y(), &point)) {
                        checksum += point.z;
                    }
                }
            }
        }
    });

    quint64 revision = 100;
    const double cachedMilliseconds = medianMilliseconds(7, [&]() {
        for (int frame = 1; frame <= frames; ++frame) {
            const auto tessellation = cache.acquire(
                objectId, revision++, deformBenchmarkFace(base, frame));
            if (!tessellation.isNull()) {
                checksum += tessellation->vertices().size();
            }
        }
    });
    std::cout << "Trimmed single face, one CV moving over " << frames
              << " frames: full rebuild=" << rebuildMilliseconds << " ms; "
              << "previous per-sample evaluator=" << previousEvaluatorMilliseconds
              << " ms; prepared Blender-style U/V basis cache="
              << cachedMilliseconds << " ms; update speedup="
              << previousEvaluatorMilliseconds / cachedMilliseconds
              << "x (" << rebuildMilliseconds / cachedMilliseconds
              << "x vs full rebuild); fresh-rebuild geometry match=passed; checksum="
              << checksum << '\n';

    const NurbsSurface3D rationalBase = makeSurface(12);
    SurfaceTessellationCache rationalCache;
    const auto rationalOriginal = rationalCache.acquire(objectId, 1,
                                                        rationalBase);
    NurbsSurface3D rationalMoved = rationalBase;
    rationalMoved.controlPoints[0].z += 0.37;
    SurfaceTessellationCache::AcquisitionStats rationalStats;
    const auto rationalUpdated = rationalCache.acquire(objectId, 2,
        rationalMoved, 0, &rationalStats);
    PreparedNurbsSurfaceTessellation rationalRebuilt;
    if (rationalOriginal.isNull() || rationalUpdated.isNull() ||
        rationalStats.path != SurfaceTessellationCache::AcquisitionStats::Path::TopologyHit ||
        !rationalRebuilt.prepare(rationalMoved) ||
        !sameTessellation(*rationalUpdated, rationalRebuilt)) {
        std::cerr << "Prepared-basis rational surface update did not match a fresh rebuild\n";
        return false;
    }
    std::cout << "Rational multi-span 12x12 patch deformation: "
              << "fresh-rebuild geometry match=passed\n";
    return true;
}

} // namespace

int main()
{
    for (const int controlPointCount : {4, 12, 24}) {
        benchmarkSurface(controlPointCount);
    }
    benchmarkMovingCylinder();
    return benchmarkMovingTrimmedFace() ? 0 : 1;
}
