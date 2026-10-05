#include "surface_tessellation_cache.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

constexpr int kMaximumCachedSurfaces = 128;

bool surfaceTranslationOffset(const NurbsSurface3D &source,
                       const NurbsSurface3D &target, Point3D *offset)
{
    if (source.dimension != target.dimension || source.degreeU != target.degreeU ||
        source.degreeV != target.degreeV || source.orderU != target.orderU ||
        source.orderV != target.orderV || source.rational != target.rational ||
        source.controlVertexCountU != target.controlVertexCountU ||
        source.controlVertexCountV != target.controlVertexCountV ||
        source.weights != target.weights || source.knotsU != target.knotsU ||
        source.knotsV != target.knotsV || source.controlPoints.isEmpty() ||
        source.controlPoints.size() != target.controlPoints.size() ||
        source.trimLoops.size() != target.trimLoops.size()) {
        return false;
    }
    for (int i = 0; i < source.trimLoops.size(); ++i) {
        const auto &a = source.trimLoops[i];
        const auto &b = target.trimLoops[i];
        if (a.isHole != b.isHole || a.curve.dimension != b.curve.dimension ||
            a.curve.degree != b.curve.degree || a.curve.order != b.curve.order ||
            a.curve.rational != b.curve.rational ||
            a.curve.controlPoints != b.curve.controlPoints ||
            a.curve.weights != b.curve.weights || a.curve.knots != b.curve.knots) {
            return false;
        }
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
    const NurbsSurface3D &surface)
{
    auto tessellation = QSharedPointer<PreparedNurbsSurfaceTessellation>::create();
    if (!tessellation->prepare(surface)) {
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
                                  int faceIndex) const
{
    if (!objectId.isValid() || geometryRevision == 0) {
        return acquireTranslated(surface);
    }

    const auto key = qMakePair(objectId.value(), faceIndex);
    auto found = entries_.find(key);
    if (found != entries_.end() &&
        found->geometryRevision == geometryRevision &&
        !found->tessellation.isNull()) {
        found->lastUse = ++useClock_;
        return found->tessellation;
    }

    const auto tessellation = acquireTranslated(surface);
    if (tessellation.isNull()) {
        entries_.remove(key);
        return {};
    }
    entries_.insert(key,
                    Entry{geometryRevision, ++useClock_, tessellation});
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
SurfaceTessellationCache::acquireTranslated(const NurbsSurface3D &surface) const
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
        if (offset.x == 0 && offset.y == 0 && offset.z == 0) {
            return tessellation;
        }
        return QSharedPointer<PreparedNurbsSurfaceTessellation>::create(
            tessellation->translated(offset));
    }
    const auto tessellation = buildTessellation(surface);
    if (!tessellation.isNull()) {
        if (translationEntries_.size() >= kMaximumCachedSurfaces) {
            translationEntries_.removeFirst();
        }
        translationEntries_.append({surface, tessellation});
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
