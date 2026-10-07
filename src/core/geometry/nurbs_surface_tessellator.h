#pragma once

#include "nurbs_surface.h"

#include <array>
#include <QtGlobal>

namespace classiCAD {

// Display and query mesh derived from an exact NURBS surface. The source CV
// net and UV trim curves remain authoritative; this mesh only approximates
// their currently visible result.
class PreparedNurbsSurfaceTessellation final {
public:
    enum class Strategy { Unprepared, AffinePlane, LinearExtrusion, GenericGrid, Failed };

    struct Polyline {
        QVector<Point3D> points;
        QVector<QPointF> parameters;
    };

    using Triangle = std::array<int, 3>;

    struct Options {
        int gridCount = 48;
        int trimBoundaryDepth = 4;
        int isocurveCount = 8;
        int isocurveSamples = 128;
        int trimSamples = 256;
    };

    struct PreparationStats {
        qint64 evaluatorPrepareMicroseconds = 0;
        qint64 trimPrepareMicroseconds = 0;
        qint64 isocurveMicroseconds = 0;
        qint64 trimBoundaryWireMicroseconds = 0;
        qint64 genericGridMicroseconds = 0;
        quint64 genericGridCellVisits = 0;
        quint64 genericGridSubdivisions = 0;
        quint64 trimBoundarySegmentTests = 0;
    };

    bool prepare(const NurbsSurface3D &surface);
    bool prepare(const NurbsSurface3D &surface, const Options &options);
    bool prepare(const NurbsSurface3D &surface,
                 const Options &options,
                 PreparationStats *stats);
    bool updateControlPointPositions(const NurbsSurface3D &surface,
                                     PreparedNurbsSurfaceTessellation *result) const;
    bool isValid() const;
    Strategy strategy() const;
    PreparedNurbsSurfaceTessellation translated(const Point3D &offset) const;
    const QVector<Point3D> &vertices() const;
    const QVector<Triangle> &triangles() const;
    const QVector<Polyline> &wireframe() const;

private:
    QVector<Point3D> vertices_;
    QVector<QPointF> vertexParameters_;
    QVector<Triangle> triangles_;
    QVector<Polyline> wireframe_;
    bool valid_ = false;
    Strategy strategy_ = Strategy::Unprepared;
};

} // namespace classiCAD
