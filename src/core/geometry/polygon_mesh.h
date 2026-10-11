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

// Authoritative local-space polygon mesh data imported from Blender. Rendering
// triangulation and GPU buffers are derived data, never stored here.
struct PolygonMesh3D {
    QVector<Point3D> vertices;
    QVector<QVector<int>> faces;
    QVector<int> faceMaterialIndices;
    QVector<QString> materialSlotIds;
    // Read-only migration aid for v8 inline material records; never written by
    // the current serializer.
    QVector<PolygonMeshMaterialSlot> materialSlots;
    QVector<MeshUvMap> uvMaps;
    QVector<MeshColorAttribute> colorAttributes;
    QString activeUvMapName;
    QString activeColorAttributeName;
};

bool validatePolygonMesh(const PolygonMesh3D &mesh, QString *error = nullptr);
QVector<std::array<int, 3>> triangulatePolygonLoop(
    const QVector<Point3D> &loopVertices);

} // namespace classiCAD
