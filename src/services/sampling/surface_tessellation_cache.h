#pragma once

#include "core/document/object_id.h"
#include "core/geometry/nurbs_surface_tessellator.h"

#include <QHash>
#include <QSharedPointer>

namespace classiCAD {

// Reuses immutable surface display geometry by object revision, translation,
// and parameter topology. Rigid moves shift samples; control-point edits reuse
// UV connectivity and reevaluate its world points. Both caches are bounded.
class SurfaceTessellationCache final {
public:
    struct AcquisitionStats {
        enum class Path {
            None,
            RevisionHit,
            TranslationHit,
            TopologyHit,
            Rebuilt,
            Failed
        };
        Path path = Path::None;
        qint64 prepareMicroseconds = 0;
        qint64 topologyUpdateMicroseconds = 0;
        PreparedNurbsSurfaceTessellation::PreparationStats preparation;
    };

    static bool translationOffset(const NurbsSurface3D &source,
                                  const NurbsSurface3D &target,
                                  Point3D *offset);
    QSharedPointer<const PreparedNurbsSurfaceTessellation> acquire(
        ObjectId objectId,
        quint64 geometryRevision,
        const NurbsSurface3D &surface,
        int faceIndex = 0,
        AcquisitionStats *stats = nullptr) const;

    void clear();
    int size() const;

private:
    struct TranslationEntry {
        NurbsSurface3D surface;
        QSharedPointer<const PreparedNurbsSurfaceTessellation> tessellation;
    };
    QSharedPointer<const PreparedNurbsSurfaceTessellation> acquireTranslated(
        const NurbsSurface3D &surface,
        AcquisitionStats *stats = nullptr) const;
    struct Entry {
        quint64 geometryRevision = 0;
        quint64 lastUse = 0;
        NurbsSurface3D surface;
        QSharedPointer<const PreparedNurbsSurfaceTessellation> tessellation;
    };

    mutable QHash<QPair<quint64, int>, Entry> entries_;
    mutable quint64 useClock_ = 0;
    mutable QVector<TranslationEntry> translationEntries_;
};

} // namespace classiCAD
