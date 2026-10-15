#pragma once

#include "nurbs_curve.h"
#include "work_plane.h"

#include <QString>
#include <QVector>
#include <array>

namespace classiCAD {

enum class MeshAttributeDomain : int {
    Point = 0,
    Face = 1,
    Corner = 2,
};

struct MeshUvMap {
    QString name;
    // One coordinate per polygon corner, flattened in face/loop order.
    QVector<QPointF> cornerValues;
};

struct MeshColorAttribute {
    QString name;
    MeshAttributeDomain domain = MeshAttributeDomain::Corner;
    // Blender attribute storage type (for example FLOAT_COLOR or BYTE_COLOR)
    // and interpretation. Keep these strings to preserve future Blender types.
    QString dataType = QStringLiteral("FLOAT_COLOR");
    QString colorSpace = QStringLiteral("scene_linear");
    int componentCount = 4;
    // Linear RGBA/RGB (or scalar replicated by a consumer), flattened by
    // domain element then component. Point, face, and corner domains are kept
    // distinct to preserve Blender's interpolation and seam semantics.
    QVector<float> values;
};

struct PolygonMeshMaterialSlot {
    // Compatibility payload for v8 documents written before the material
    // table existed. New documents use materialSlotIds and DocumentMaterial.
    QString name;
    QString libraryPath;
    QVector<float> baseColorSceneLinear{0.8f, 0.8f, 0.8f, 1.0f};
    float roughness = 0.4f;
    float metallic = 0.0f;
    float alpha = 1.0f;
};

// Authoritative local-space polygon mesh data imported from Blender. Polygon
// loops remain the source geometry; imported faceTriangles is an optional
// derived display cache that preserves Blender's evaluated triangulation.
struct PolygonMesh3D {
    QVector<Point3D> vertices;
    QVector<QVector<int>> faces;
    QVector<QVector<std::array<int, 3>>> faceTriangles;
    // Optional custom split normals, one XYZ vector per face loop in the same
    // flattened order used by UV corner data. Empty means derive normals.
    QVector<Point3D> cornerNormals;
    QVector<int> faceMaterialIndices;
    QVector<QString> materialSlotIds;
    // Read-only migration aid for v8 inline material records; never written by
    // the current serializer.
    QVector<PolygonMeshMaterialSlot> materialSlots;
    QVector<MeshUvMap> uvMaps;
    QVector<MeshColorAttribute> colorAttributes;
    QString activeUvMapName;
    QString defaultUvMapName;
    QString activeColorAttributeName;
    QString defaultColorAttributeName;
};

// Blender Workbench uses the active UV layer, falling back to the default
// (render) UV layer when no active layer is set.
inline const QString &workbenchUvMapName(const PolygonMesh3D &mesh)
{
    return mesh.activeUvMapName.isEmpty() ? mesh.defaultUvMapName
                                          : mesh.activeUvMapName;
}

bool validatePolygonMesh(const PolygonMesh3D &mesh, QString *error = nullptr);
bool validatePolygonFaceTriangulation(
    int loopCount, const QVector<std::array<int, 3>> &triangles);
QVector<std::array<int, 3>> triangulatePolygonLoop(
    const QVector<Point3D> &loopVertices);
QVector<std::array<int, 3>> polygonMeshFaceTriangles(
    const PolygonMesh3D &mesh, int faceIndex,
    const QVector<Point3D> &loopVertices);

} // namespace classiCAD
