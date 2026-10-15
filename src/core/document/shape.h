#pragma once

#include "core/document/dimension_anchor.h"
#include "core/geometry/arc_mode.h"
#include "core/geometry/geometry_type.h"
#include "core/geometry/nurbs_curve.h"
#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"
#include "core/geometry/polygon_mesh.h"
#include "core/geometry/scalar_volume.h"
#include "core/geometry/work_plane.h"

#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QVector>
#include <QVector4D>

namespace classiCAD {

enum class PictureImageSide : int {
    Both = 0,
    Front = 1,
    Back = 2,
};

enum class PictureImageDepth : int {
    Default = 0,
    Front = 1,
    Back = 2,
};

// Persistent geometry and its local-to-world mapping. Shape owns exact curve
// and surface data; tessellations and screen-space data live in services.
struct Shape {
    GeometryType geometryType = GeometryType::Invalid;
    QVector<QPointF> points;
    using NurbsCurve3D = classiCAD::NurbsCurve3D;
    using NurbsCurve2D = classiCAD::NurbsCurve2D;
    NurbsCurve3D nurbs;

    ArcMode arcMode = ArcMode::TwoPoint;
    qreal arcSweep = 0.0;
    // Interior subdivision locations are markers in the source parameter
    // domain; they do not change the stored NURBS spans or control vertices.
    QVector<double> subdivisionParameters;
    // A joined spline remains a Rhino-style component curve collection. Each
    // component keeps its own degree, weights, knots, and parameter domain.
    QVector<NurbsCurve3D> components;
    // Mixed-plane PolyCurves store one frame per component. Empty is the
    // legacy representation and uses workPlaneFrame for all components.
    QVector<WorkPlaneFrame> componentWorkPlaneFrames;
    // Explicit weld topology, flattened in the control-point order exposed by
    // the geometry type. Zero means the point is not welded; matching nonzero
    // values identify control points connected by the Weld command or by an
    // extrusion that keeps its source boundary attached.
    QVector<quint64> controlPointWeldGroups;
    QVector<DimensionAnchorReference> dimensionAnchors;
    qreal dimensionOffset = 0.0;
    bool dimensionOffsetValid = false;
    // Picture frame corners are stored in points; image bytes are embedded.
    QImage pictureImage;
    QByteArray pictureImageData;
    bool pictureShowPerspective = true;
    bool pictureShowOrthographic = true;
    bool pictureOnlyAxisAligned = false;
    PictureImageSide pictureImageSide = PictureImageSide::Both;
    PictureImageDepth pictureImageDepth = PictureImageDepth::Default;
    QVector4D pictureColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
    bool pictureUseAlphaBlend = true;
    bool picturePremultipliedAlpha = false;
    WorkPlane workPlane = WorkPlane::XY;
    qreal workPlaneOffset = 0.0;
    // When valid, this frame is authoritative for local 2D geometry. An
    // invalid frame keeps older records on their principal workPlane mapping.
    WorkPlaneFrame workPlaneFrame;
    using NurbsSurface3D = classiCAD::NurbsSurface3D;
    NurbsSurface3D nurbsSurface;
    NurbsExtrusionSolid3D nurbsSolid;
    PolygonMesh3D polygonMesh;
    ScalarVolume3D scalarVolume;
};

inline QVector<NurbsSurface3D> shapeSurfaceFaces(const Shape &shape)
{
    if (shape.geometryType == GeometryType::NurbsSolid)
        return nurbsSolidFaces(shape.nurbsSolid);
    if (shape.geometryType == GeometryType::NurbsSurface)
        return {shape.nurbsSurface};
    return {};
}

inline NurbsSurface3D &shapeBaseSurface(Shape &shape)
{
    return shape.geometryType == GeometryType::NurbsSolid
               ? shape.nurbsSolid.baseSurface : shape.nurbsSurface;
}

} // namespace classiCAD
