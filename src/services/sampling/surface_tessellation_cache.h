#pragma once

#include "core/document/object_id.h"
#include "core/geometry/nurbs_surface_tessellator.h"

#include <QHash>
#include <QSharedPointer>

namespace classiCAD {

// Reuses immutable surface display geometry by object revision and by exact
// surface shape up to translation. Moving objects and anonymous previews shift
// existing samples without evaluating the surface again. Both caches are bounded.
class SurfaceTessellationCache final {
public:
    QSharedPointer<const PreparedNurbsSurfaceTessellation> acquire(
        ObjectId objectId,
        quint64 geometryRevision,
        const NurbsSurface3D &surface,
        int faceIndex = 0) const;

    void clear();
    int size() const;

private:
    struct TranslationEntry {
        NurbsSurface3D surface;
        QSharedPointer<const PreparedNurbsSurfaceTessellation> tessellation;
    };
    QSharedPointer<const PreparedNurbsSurfaceTessellation> acquireTranslated(
        const NurbsSurface3D &surface) const;
    struct Entry {
        quint64 geometryRevision = 0;
        quint64 lastUse = 0;
        QSharedPointer<const PreparedNurbsSurfaceTessellation> tessellation;
    };

    mutable QHash<QPair<quint64, int>, Entry> entries_;
    mutable quint64 useClock_ = 0;
    mutable QVector<TranslationEntry> translationEntries_;
};

} // namespace classiCAD
