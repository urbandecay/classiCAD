#include "polygon_mesh.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace classiCAD {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

quint64 undirectedEdgeKey(int first, int second)
{
    const quint32 low = static_cast<quint32>(std::min(first, second));
    const quint32 high = static_cast<quint32>(std::max(first, second));
    return (quint64(low) << 32) | quint64(high);
}

} // namespace

bool validatePolygonFaceTriangulation(
    int loopCount, const QVector<std::array<int, 3>> &triangles)
{
    if (loopCount < 3) return triangles.isEmpty();
    if (triangles.size() != loopCount - 2) return false;

    struct EdgeUsage {
        int count = 0;
        int orientation = 0;
    };
    QHash<quint64, EdgeUsage> edgeUsage;
    QSet<quint64> boundaryEdges;
    for (int loop = 0; loop < loopCount; ++loop) {
        boundaryEdges.insert(undirectedEdgeKey(
            loop, (loop + 1) % loopCount));
    }
    QSet<int> usedCorners;
    for (const auto &triangle : triangles) {
        if (triangle[0] < 0 || triangle[0] >= loopCount ||
            triangle[1] < 0 || triangle[1] >= loopCount ||
            triangle[2] < 0 || triangle[2] >= loopCount ||
            triangle[0] == triangle[1] || triangle[1] == triangle[2] ||
            triangle[2] == triangle[0]) {
            return false;
        }
        usedCorners.insert(triangle[0]);
        usedCorners.insert(triangle[1]);
        usedCorners.insert(triangle[2]);
        for (int edge = 0; edge < 3; ++edge) {
            const int first = triangle[static_cast<size_t>(edge)];
            const int second = triangle[static_cast<size_t>((edge + 1) % 3)];
            EdgeUsage &usage = edgeUsage[undirectedEdgeKey(first, second)];
            ++usage.count;
            usage.orientation += first < second ? 1 : -1;
        }
    }
    if (usedCorners.size() != loopCount) return false;

    for (auto edge = edgeUsage.cbegin(); edge != edgeUsage.cend(); ++edge) {
        if (boundaryEdges.contains(edge.key())) {
            const quint32 low = static_cast<quint32>(edge.key() >> 32);
            const quint32 high = static_cast<quint32>(edge.key());
            const int expectedOrientation =
                (static_cast<int>(low) + 1) % loopCount ==
                        static_cast<int>(high)
                    ? 1 : -1;
            if (edge->count != 1 ||
                edge->orientation != expectedOrientation) {
                return false;
            }
        } else if (edge->count != 2 || edge->orientation != 0) {
            return false;
        }
    }
    for (const quint64 boundaryEdge : boundaryEdges) {
        if (!edgeUsage.contains(boundaryEdge)) return false;
    }
    return true;
}

bool validatePolygonMesh(const PolygonMesh3D &mesh, QString *error)
{
    if (mesh.vertices.isEmpty() || mesh.faces.isEmpty()) {
        return fail(error, QStringLiteral("mesh needs vertices and faces"));
    }
    for (const Point3D &point : mesh.vertices) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z)) {
            return fail(error, QStringLiteral("mesh vertex is not finite"));
        }
    }
    int cornerCount = 0;
    for (const QVector<int> &face : mesh.faces) {
        if (face.size() < 3) {
            return fail(error, QStringLiteral("mesh face has fewer than three corners"));
        }
        for (const int vertexIndex : face) {
            if (vertexIndex < 0 || vertexIndex >= mesh.vertices.size()) {
                return fail(error, QStringLiteral("mesh face index is out of range"));
            }
        }
        cornerCount += face.size();
    }
    if (!mesh.faceTriangles.isEmpty()) {
        if (mesh.faceTriangles.size() != mesh.faces.size()) {
            return fail(error,
                        QStringLiteral("mesh face triangulation count does not match faces"));
        }
        for (int faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
            if (!validatePolygonFaceTriangulation(
                    mesh.faces[faceIndex].size(),
                    mesh.faceTriangles[faceIndex])) {
                return fail(error,
                            QStringLiteral("mesh face triangulation is incomplete or invalid"));
            }
        }
    }
    if (!mesh.cornerNormals.isEmpty() && mesh.cornerNormals.size() != cornerCount) {
        return fail(error, QStringLiteral("mesh corner normal count does not match face loops"));
    }
    for (const Point3D &normal : mesh.cornerNormals) {
        if (!std::isfinite(normal.x) || !std::isfinite(normal.y) ||
            !std::isfinite(normal.z) ||
            normal.x * normal.x + normal.y * normal.y + normal.z * normal.z <= 1.0e-24) {
            return fail(error, QStringLiteral("mesh corner normal is invalid"));
        }
    }
    if (mesh.faceMaterialIndices.size() != mesh.faces.size()) {
        return fail(error, QStringLiteral("mesh material index count does not match faces"));
    }
    for (const int materialIndex : mesh.faceMaterialIndices) {
        if (materialIndex < 0 ||
            (mesh.materialSlotIds.isEmpty() && mesh.materialSlots.isEmpty()
                 ? materialIndex != 0
                 : materialIndex >= (mesh.materialSlotIds.isEmpty()
                                         ? mesh.materialSlots.size()
                                         : mesh.materialSlotIds.size()))) {
            return fail(error, QStringLiteral("mesh material index is out of range"));
        }
    }
    for (const PolygonMeshMaterialSlot &slot : mesh.materialSlots) {
        if (slot.baseColorSceneLinear.size() != 4 ||
            !std::isfinite(slot.roughness) || !std::isfinite(slot.metallic) ||
            !std::isfinite(slot.alpha)) {
            return fail(error, QStringLiteral("invalid mesh material slot"));
        }
        for (const float component : slot.baseColorSceneLinear) {
            if (!std::isfinite(component)) {
                return fail(error, QStringLiteral("invalid mesh material color"));
            }
        }
    }

    for (int i = 0; i < mesh.uvMaps.size(); ++i) {
        const MeshUvMap &uvMap = mesh.uvMaps[i];
        if (uvMap.name.isEmpty() || uvMap.cornerValues.size() != cornerCount) {
            return fail(error, QStringLiteral("invalid mesh UV map"));
        }
        for (int j = 0; j < i; ++j) {
            if (mesh.uvMaps[j].name == uvMap.name) {
                return fail(error, QStringLiteral("duplicate mesh UV map name"));
            }
        }
        for (const QPointF &uv : uvMap.cornerValues) {
            if (!std::isfinite(uv.x()) || !std::isfinite(uv.y())) {
                return fail(error, QStringLiteral("mesh UV coordinate is not finite"));
            }
        }
    }

    for (int i = 0; i < mesh.colorAttributes.size(); ++i) {
        const MeshColorAttribute &attribute = mesh.colorAttributes[i];
        if (attribute.name.isEmpty() || attribute.dataType.isEmpty() ||
            attribute.colorSpace.isEmpty() || attribute.componentCount < 1 ||
            attribute.componentCount > 4) {
            return fail(error, QStringLiteral("invalid mesh color attribute metadata"));
        }
        int elementCount = 0;
        switch (attribute.domain) {
        case MeshAttributeDomain::Point:
            elementCount = mesh.vertices.size();
            break;
        case MeshAttributeDomain::Face:
            elementCount = mesh.faces.size();
            break;
        case MeshAttributeDomain::Corner:
            elementCount = cornerCount;
            break;
        default:
            return fail(error, QStringLiteral("unsupported mesh color attribute domain"));
        }
        if (attribute.values.size() != elementCount * attribute.componentCount) {
            return fail(error, QStringLiteral("mesh color attribute value count is invalid"));
        }
        for (int j = 0; j < i; ++j) {
            if (mesh.colorAttributes[j].name == attribute.name) {
                return fail(error, QStringLiteral("duplicate mesh color attribute name"));
            }
        }
        for (const float component : attribute.values) {
            if (!std::isfinite(component)) {
                return fail(error, QStringLiteral("mesh color attribute is not finite"));
            }
        }
    }
    if (!mesh.activeUvMapName.isEmpty()) {
        bool found = false;
        for (const MeshUvMap &uvMap : mesh.uvMaps) found |= uvMap.name == mesh.activeUvMapName;
        if (!found) return fail(error, QStringLiteral("active mesh UV map is missing"));
    }
    if (!mesh.defaultUvMapName.isEmpty()) {
        bool found = false;
        for (const MeshUvMap &uvMap : mesh.uvMaps) found |= uvMap.name == mesh.defaultUvMapName;
        if (!found) return fail(error, QStringLiteral("default mesh UV map is missing"));
    }
    if (!mesh.activeColorAttributeName.isEmpty()) {
        bool found = false;
        for (const MeshColorAttribute &attribute : mesh.colorAttributes)
            found |= attribute.name == mesh.activeColorAttributeName;
        if (!found) return fail(error, QStringLiteral("active mesh color attribute is missing"));
    }
    if (!mesh.defaultColorAttributeName.isEmpty()) {
        bool found = false;
        for (const MeshColorAttribute &attribute : mesh.colorAttributes)
            found |= attribute.name == mesh.defaultColorAttributeName;
        if (!found) return fail(error, QStringLiteral("default mesh color attribute is missing"));
    }
    return true;
}

QVector<std::array<int, 3>> triangulatePolygonLoop(
    const QVector<Point3D> &vertices)
{
    QVector<std::array<int, 3>> result;
    if (vertices.size() < 3) return result;
    Point3D normal{};
    for (int i = 0; i < vertices.size(); ++i) {
        const Point3D &a = vertices[i], &b = vertices[(i + 1) % vertices.size()];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    const qreal ax = std::abs(normal.x), ay = std::abs(normal.y), az = std::abs(normal.z);
    if (std::max(ax, std::max(ay, az)) <= 1.0e-14) return result;
    const int droppedAxis = ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
    QVector<QPointF> points;
    points.reserve(vertices.size());
    for (const Point3D &point : vertices) {
        points.append(droppedAxis == 0 ? QPointF(point.y, point.z)
                     : droppedAxis == 1 ? QPointF(point.x, point.z)
                                        : QPointF(point.x, point.y));
    }
    qreal minX = points.first().x(), maxX = minX;
    qreal minY = points.first().y(), maxY = minY;
    for (const QPointF &point : points) {
        minX = std::min(minX, point.x()); maxX = std::max(maxX, point.x());
        minY = std::min(minY, point.y()); maxY = std::max(maxY, point.y());
    }
    const qreal scale = std::max(maxX - minX, maxY - minY);
    if (!std::isfinite(scale) || scale <= 0.0) return result;
    const QPointF center((minX + maxX) * 0.5, (minY + maxY) * 0.5);
    for (QPointF &point : points) point = (point - center) / scale;
    const auto cross2 = [](const QPointF &a, const QPointF &b, const QPointF &c) {
        return (b.x() - a.x()) * (c.y() - a.y()) -
               (b.y() - a.y()) * (c.x() - a.x());
    };
    qreal signedArea = 0.0;
    for (int i = 0; i < points.size(); ++i) {
        const QPointF &a = points[i], &b = points[(i + 1) % points.size()];
        signedArea += a.x() * b.y() - b.x() * a.y();
    }
    if (std::abs(signedArea) <= 1.0e-14) return result;
    const qreal winding = signedArea > 0.0 ? 1.0 : -1.0;
    QVector<int> remaining;
    remaining.reserve(points.size());
    for (int i = 0; i < points.size(); ++i) remaining.append(i);
    const auto insideTriangle = [&](const QPointF &p, const QPointF &a,
                                    const QPointF &b, const QPointF &c) {
        constexpr qreal epsilon = 1.0e-12;
        return winding * cross2(a, b, p) >= -epsilon &&
               winding * cross2(b, c, p) >= -epsilon &&
               winding * cross2(c, a, p) >= -epsilon;
    };
    int clippedCount = 0;
    while (remaining.size() > 3) {
        bool clipped = false;
        for (int i = 0; i < remaining.size(); ++i) {
            const int prev = remaining[(i + remaining.size() - 1) % remaining.size()];
            const int curr = remaining[i], next = remaining[(i + 1) % remaining.size()];
            const qreal corner = winding * cross2(points[prev], points[curr], points[next]);
            if (corner <= 1.0e-12) {
                if (std::abs(corner) <= 1.0e-12) {
                    remaining.removeAt(i);
                    clipped = true;
                    break;
                }
                continue;
            }
            bool containsVertex = false;
            for (const int candidate : remaining) {
                if (candidate == prev || candidate == curr || candidate == next) continue;
                if (insideTriangle(points[candidate], points[prev], points[curr], points[next])) {
                    containsVertex = true;
                    break;
                }
            }
            if (containsVertex) continue;
            result.append({prev, curr, next});
            remaining.removeAt(i);
            clipped = true;
            break;
        }
        if (!clipped || ++clippedCount > vertices.size()) {
            result.clear();
            return result;
        }
    }
    if (remaining.size() == 3 &&
        std::abs(cross2(points[remaining[0]], points[remaining[1]],
                        points[remaining[2]])) > 1.0e-12) {
        result.append({remaining[0], remaining[1], remaining[2]});
    }
    return result;
}

QVector<std::array<int, 3>> polygonMeshFaceTriangles(
    const PolygonMesh3D &mesh, int faceIndex,
    const QVector<Point3D> &loopVertices)
{
    if (faceIndex >= 0 && faceIndex < mesh.faceTriangles.size()) {
        const QVector<std::array<int, 3>> &triangles =
            mesh.faceTriangles[faceIndex];
        bool usable = loopVertices.size() >= 3 &&
                      triangles.size() == loopVertices.size() - 2;
        for (const auto &triangle : triangles) {
            for (const int corner : triangle) {
                usable = usable && corner >= 0 &&
                         corner < loopVertices.size();
            }
        }
        if (usable) return triangles;
    }
    return triangulatePolygonLoop(loopVertices);
}

} // namespace classiCAD
