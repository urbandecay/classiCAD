#include "surface_tessellation_cache.h"

#include <algorithm>

namespace classiCAD {
namespace {

constexpr int kMaximumCachedSurfaces = 128;

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

QSharedPointer<const PreparedNurbsSurfaceTessellation>
SurfaceTessellationCache::acquire(ObjectId objectId,
                                  quint64 geometryRevision,
                                  const NurbsSurface3D &surface) const
{
    if (!objectId.isValid() || geometryRevision == 0) {
        return buildTessellation(surface);
    }

    auto found = entries_.find(objectId.value());
    if (found != entries_.end() &&
        found->geometryRevision == geometryRevision &&
        !found->tessellation.isNull()) {
        found->lastUse = ++useClock_;
        return found->tessellation;
    }

    const auto tessellation = buildTessellation(surface);
    if (tessellation.isNull()) {
        entries_.remove(objectId.value());
        return {};
    }
    entries_.insert(objectId.value(),
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

void SurfaceTessellationCache::clear()
{
    entries_.clear();
    useClock_ = 0;
}

int SurfaceTessellationCache::size() const
{
    return entries_.size();
}

} // namespace classiCAD
