#pragma once

#include "core/document/object_id.h"
#include "core/geometry/nurbs_surface_tessellator.h"

#include <QHash>
#include <QSharedPointer>

namespace classiCAD {

// Reuses immutable surface display geometry while its document object
// revision stays unchanged. Entries are bounded so removed objects do not
// leave an unbounded history of surface meshes behind.
class SurfaceTessellationCache final {
public:
    QSharedPointer<const PreparedNurbsSurfaceTessellation> acquire(
        ObjectId objectId,
        quint64 geometryRevision,
        const NurbsSurface3D &surface) const;

    void clear();
    int size() const;

private:
    struct Entry {
        quint64 geometryRevision = 0;
        quint64 lastUse = 0;
        QSharedPointer<const PreparedNurbsSurfaceTessellation> tessellation;
    };

    mutable QHash<quint64, Entry> entries_;
    mutable quint64 useClock_ = 0;
};

} // namespace classiCAD
