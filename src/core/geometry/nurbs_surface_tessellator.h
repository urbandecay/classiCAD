#pragma once

#include "nurbs_surface.h"

#include <array>

namespace classiCAD {

// Display and query mesh derived from an exact NURBS surface. The source CV
// net and UV trim curves remain authoritative; this mesh only approximates
// their currently visible result.
class PreparedNurbsSurfaceTessellation final {
public:
    struct Polyline {
        QVector<Point3D> points;
    };

    using Triangle = std::array<int, 3>;

    struct Options {
        int gridCount = 48;
        int trimBoundaryDepth = 4;
        int isocurveCount = 8;
        int isocurveSamples = 128;
        int trimSamples = 256;
    };

    bool prepare(const NurbsSurface3D &surface);
    bool prepare(const NurbsSurface3D &surface, const Options &options);
    bool isValid() const;
    PreparedNurbsSurfaceTessellation translated(const Point3D &offset) const;
    const QVector<Point3D> &vertices() const;
    const QVector<Triangle> &triangles() const;
    const QVector<Polyline> &wireframe() const;

private:
    QVector<Point3D> vertices_;
    QVector<Triangle> triangles_;
    QVector<Polyline> wireframe_;
    bool valid_ = false;
};

} // namespace classiCAD
