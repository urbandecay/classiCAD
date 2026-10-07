#include "surface_tessellation_cache.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr int kMaximumCachedSurfaces = 128;

bool sameSurfaceParameterization(const NurbsSurface3D &a,
                                 const NurbsSurface3D &b)
{
    if (a.dimension != b.dimension || a.degreeU != b.degreeU ||
        a.degreeV != b.degreeV || a.orderU != b.orderU ||
        a.orderV != b.orderV || a.rational != b.rational ||
        a.controlVertexCountU != b.controlVertexCountU ||
        a.controlVertexCountV != b.controlVertexCountV ||
        a.controlPoints.size() != b.controlPoints.size() ||
        a.weights != b.weights || a.knotsU != b.knotsU ||
        a.knotsV != b.knotsV || a.trimLoops.size() != b.trimLoops.size()) {
        return false;
    }
    for (int i = 0; i < a.trimLoops.size(); ++i) {
        const auto &first = a.trimLoops[i];
        const auto &second = b.trimLoops[i];
        if (first.isHole != second.isHole ||
            first.curve.dimension != second.curve.dimension ||
            first.curve.degree != second.curve.degree ||
            first.curve.order != second.curve.order ||
            first.curve.rational != second.curve.rational ||
            first.curve.controlPoints != second.curve.controlPoints ||
            first.curve.weights != second.curve.weights ||
            first.curve.knots != second.curve.knots) {
            return false;
        }
    }
    return true;
}

bool surfaceTranslationOffset(const NurbsSurface3D &source,
                       const NurbsSurface3D &target, Point3D *offset)
{
    if (!sameSurfaceParameterization(source, target) ||
        source.controlPoints.isEmpty()) {
        return false;
    }
    const Point3D &a = source.controlPoints.first();
    const Point3D &b = target.controlPoints.first();
    *offset = {b.x - a.x, b.y - a.y, b.z - a.z};
    const auto matches = [](double original, double moved, double delta) {
        const double expected = original + delta;
        // Live incremental drags accumulate rounding in each CV. Allow a
        // small multiple of machine precision so that rounding does not turn
        // a rigid move into a full mesh rebuild. Exact model data is untouched.
        const double tolerance = 256 * std::numeric_limits<double>::epsilon() *
            std::max({1.0, std::abs(original), std::abs(moved), std::abs(delta)});
        return std::isfinite(expected) && std::isfinite(moved) &&
               std::abs(expected - moved) <= tolerance;
    };
    for (int i = 0; i < source.controlPoints.size(); ++i) {
        const auto &p = source.controlPoints[i];
        const auto &q = target.controlPoints[i];
        if (!matches(p.x, q.x, offset->x) || !matches(p.y, q.y, offset->y) ||
            !matches(p.z, q.z, offset->z)) {
            return false;
        }
    }
    return true;
}

QSharedPointer<const PreparedNurbsSurfaceTessellation> buildTessellation(
    const NurbsSurface3D &surface,
    SurfaceTessellationCache::AcquisitionStats *stats)
{
    auto tessellation = QSharedPointer<PreparedNurbsSurfaceTessellation>::create();
    QElapsedTimer timer;
    if (stats != nullptr) {
        timer.start();
    }
    const bool prepared = tessellation->prepare(
        surface, PreparedNurbsSurfaceTessellation::Options{},
        stats != nullptr ? &stats->preparation : nullptr);
    if (stats != nullptr) {
        stats->prepareMicroseconds = timer.nsecsElapsed() / 1000;
    }
    if (!prepared) {
        return {};
    }
    return tessellation;
}

} // namespace

bool SurfaceTessellationCache::translationOffset(
    const NurbsSurface3D &source, const NurbsSurface3D &target, Point3D *offset)
{
    return offset != nullptr && surfaceTranslationOffset(source, target, offset);
}

QSharedPointer<const PreparedNurbsSurfaceTessellation>
SurfaceTessellationCache::acquire(ObjectId objectId,
                                  quint64 geometryRevision,
                                  const NurbsSurface3D &surface,
                                  int faceIndex,
                                  AcquisitionStats *stats) const
{
    if (stats != nullptr) {
        *stats = {};
    }
    if (!objectId.isValid() || geometryRevision == 0) {
        return acquireTranslated(surface, stats);
    }

    const auto key = qMakePair(objectId.value(), faceIndex);
    auto found = entries_.find(key);
    if (found != entries_.end() &&
        found->geometryRevision == geometryRevision &&
        !found->tessellation.isNull()) {
        found->lastUse = ++useClock_;
        if (stats != nullptr) {
            stats->path = AcquisitionStats::Path::RevisionHit;
        }
        return found->tessellation;
    }

    QSharedPointer<const PreparedNurbsSurfaceTessellation> tessellation;
    if (found != entries_.end() && !found->tessellation.isNull()) {
        Point3D translation;
        if (translationOffset(found->surface, surface, &translation)) {
            tessellation = translation.x == 0.0 && translation.y == 0.0 &&
                                   translation.z == 0.0
                               ? found->tessellation
                               : QSharedPointer<PreparedNurbsSurfaceTessellation>::create(
                                     found->tessellation->translated(translation));
            if (stats != nullptr) {
                stats->path = AcquisitionStats::Path::TranslationHit;
            }
        } else if (sameSurfaceParameterization(found->surface, surface) &&
                   found->tessellation->strategy() ==
                       PreparedNurbsSurfaceTessellation::Strategy::GenericGrid) {
            QElapsedTimer updateTimer;
            if (stats != nullptr) {
                updateTimer.start();
            }
            PreparedNurbsSurfaceTessellation updated;
            if (found->tessellation->updateControlPointPositions(surface,
                                                                 &updated)) {
                tessellation =
                    QSharedPointer<PreparedNurbsSurfaceTessellation>::create(
                        std::move(updated));
                if (stats != nullptr) {
                    stats->path = AcquisitionStats::Path::TopologyHit;
                    stats->topologyUpdateMicroseconds =
                        updateTimer.nsecsElapsed() / 1000;
                }
            }
        }
    }
    if (tessellation.isNull()) {
        tessellation = acquireTranslated(surface, stats);
    }
    if (tessellation.isNull()) {
        entries_.remove(key);
        if (stats != nullptr) {
            stats->path = AcquisitionStats::Path::Failed;
        }
        return {};
    }
    entries_.insert(key,
                    Entry{geometryRevision, ++useClock_, surface, tessellation});
    if (entries_.size() > kMaximumCachedSurfaces) {
        auto leastRecentlyUsed = entries_.begin();
        for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
            if (iterator->lastUse < leastRecentlyUsed->lastUse) {
                leastRecentlyUsed = iterator;
            }
        }
        entries_.erase(leastRecentlyUsed);
    }
    return tessellation;
}

QSharedPointer<const PreparedNurbsSurfaceTessellation>
SurfaceTessellationCache::acquireTranslated(
    const NurbsSurface3D &surface,
    AcquisitionStats *stats) const
{
    for (int i = translationEntries_.size() - 1; i >= 0; --i) {
        Point3D offset;
        const auto &entry = translationEntries_[i];
        if (!translationOffset(entry.surface, surface, &offset)) {
            continue;
        }
        const auto tessellation = entry.tessellation;
        if (i != translationEntries_.size() - 1) {
            const auto recent = translationEntries_.takeAt(i);
            translationEntries_.append(recent);
        }
        if (stats != nullptr) {
            stats->path = AcquisitionStats::Path::TranslationHit;
        }
        if (offset.x == 0 && offset.y == 0 && offset.z == 0) {
            return tessellation;
        }
        return QSharedPointer<PreparedNurbsSurfaceTessellation>::create(
            tessellation->translated(offset));
    }
    const auto tessellation = buildTessellation(surface, stats);
    if (!tessellation.isNull()) {
        if (stats != nullptr) {
            stats->path = AcquisitionStats::Path::Rebuilt;
        }
        if (translationEntries_.size() >= kMaximumCachedSurfaces) {
            translationEntries_.removeFirst();
        }
        translationEntries_.append({surface, tessellation});
    } else if (stats != nullptr) {
        stats->path = AcquisitionStats::Path::Failed;
    }
    return tessellation;
}

void SurfaceTessellationCache::clear()
{
    entries_.clear();
    translationEntries_.clear();
    useClock_ = 0;
}

int SurfaceTessellationCache::size() const
{
    return entries_.size();
}

} // namespace classiCAD
