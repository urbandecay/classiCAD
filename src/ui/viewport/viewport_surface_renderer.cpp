/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_surface_renderer.h"

#include "viewport_cavity_blue_noise.h"
#include "viewport_depth_geometry.h"
#include "blender_grid_renderer.h"
#include "viewport_line_weight.h"
#include "workbench_lighting.h"
#include "core/geometry/polygon_mesh.h"

#include <QDataStream>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QIODevice>
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace classiCAD {
namespace {

constexpr int kWorkbenchShadowMapSize = 1024;

struct PolygonShadowEdgeUse {
    int count = 0;
    int from = -1;
    int to = -1;
};

bool buildClosedPolygonShadowMesh(
    const PolygonMesh3D &mesh,
    const std::array<double, 16> &matrix,
    QVector<QVector3D> *shadowVertices,
    QVector<std::array<int, 3>> *shadowTriangles)
{
    if (shadowVertices == nullptr || shadowTriangles == nullptr ||
        mesh.vertices.size() < 4 || mesh.faces.isEmpty()) {
        return false;
    }

    QVector<QVector3D> vertices;
    vertices.reserve(mesh.vertices.size());
    for (const Point3D &point : mesh.vertices) {
        const double w = matrix[12] * point.x + matrix[13] * point.y +
                         matrix[14] * point.z + matrix[15];
        if (!std::isfinite(w) || std::abs(w) <= 1.0e-12) return false;
        const double x = (matrix[0] * point.x + matrix[1] * point.y +
                          matrix[2] * point.z + matrix[3]) / w;
        const double y = (matrix[4] * point.x + matrix[5] * point.y +
                          matrix[6] * point.z + matrix[7]) / w;
        const double z = (matrix[8] * point.x + matrix[9] * point.y +
                          matrix[10] * point.z + matrix[11]) / w;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            std::abs(x) > std::numeric_limits<float>::max() ||
            std::abs(y) > std::numeric_limits<float>::max() ||
            std::abs(z) > std::numeric_limits<float>::max()) {
            return false;
        }
        vertices.append(QVector3D(static_cast<float>(x),
                                  static_cast<float>(y),
                                  static_cast<float>(z)));
    }

    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
        matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
        matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-18) {
        return false;
    }
    const bool reflected = determinant < 0.0;
    QVector<std::array<int, 3>> triangles;
    QSet<int> usedVertices;
    for (const QVector<int> &face : mesh.faces) {
        if (face.size() < 3) return false;
        QVector<Point3D> localLoop;
        localLoop.reserve(face.size());
        QSet<int> faceIndices;
        for (const int vertexIndex : face) {
            if (vertexIndex < 0 || vertexIndex >= mesh.vertices.size() ||
                faceIndices.contains(vertexIndex)) {
                return false;
            }
            faceIndices.insert(vertexIndex);
            usedVertices.insert(vertexIndex);
            localLoop.append(mesh.vertices[vertexIndex]);
        }
        const QVector<std::array<int, 3>> faceTriangles =
            triangulatePolygonLoop(localLoop);
        if (faceTriangles.isEmpty()) return false;
        for (const auto &faceTriangle : faceTriangles) {
            std::array<int, 3> triangle{
                face[faceTriangle[0]], face[faceTriangle[1]],
                face[faceTriangle[2]]};
            if (reflected) std::swap(triangle[1], triangle[2]);
            const QVector3D a = vertices[triangle[0]];
            const QVector3D b = vertices[triangle[1]];
            const QVector3D c = vertices[triangle[2]];
            const QVector3D cross = QVector3D::crossProduct(b - a, c - a);
            if (!std::isfinite(cross.lengthSquared()) ||
                cross.lengthSquared() <= 1.0e-16f) {
                return false;
            }
            triangles.append(triangle);
        }
    }
    // Loose vertices and incomplete triangulations are ambiguous volume
    // topology, so keep those meshes on the established fallback path.
    if (usedVertices.size() != vertices.size() || triangles.isEmpty()) {
        return false;
    }

    // Edge counts alone admit closed shells pinched together at one vertex.
    // A triangulated closed 2-manifold has one connected cycle in the link
    // around every vertex.
    QVector<QVector<std::array<int, 2>>> vertexLinks(vertices.size());
    for (const auto &triangle : triangles) {
        vertexLinks[triangle[0]].append({triangle[1], triangle[2]});
        vertexLinks[triangle[1]].append({triangle[2], triangle[0]});
        vertexLinks[triangle[2]].append({triangle[0], triangle[1]});
    }
    for (const QVector<std::array<int, 2>> &links : vertexLinks) {
        if (links.isEmpty()) return false;
        QHash<int, QSet<int>> adjacency;
        for (const auto &linkEdge : links) {
            const int first = linkEdge[0];
            const int second = linkEdge[1];
            if (first == second || adjacency[first].contains(second) ||
                adjacency[second].contains(first)) {
                return false;
            }
            adjacency[first].insert(second);
            adjacency[second].insert(first);
        }
        for (auto it = adjacency.cbegin(); it != adjacency.cend(); ++it) {
            if (it.value().size() != 2) return false;
        }
        QSet<int> visited;
        QVector<int> pending{adjacency.constBegin().key()};
        while (!pending.isEmpty()) {
            const int node = pending.takeLast();
            if (visited.contains(node)) continue;
            visited.insert(node);
            for (const int neighbor : adjacency.value(node)) {
                if (!visited.contains(neighbor)) pending.append(neighbor);
            }
        }
        if (visited.size() != adjacency.size()) return false;
    }

    const auto edgeKey = [](int first, int second) {
        const quint32 low = static_cast<quint32>(std::min(first, second));
        const quint32 high = static_cast<quint32>(std::max(first, second));
        return (quint64(low) << 32u) | high;
    };
    QHash<quint64, PolygonShadowEdgeUse> edgeUses;
    QVector<int> parent(vertices.size());
    for (int index = 0; index < parent.size(); ++index) parent[index] = index;
    const auto findRoot = [&parent](int value) {
        int root = value;
        while (parent[root] != root) root = parent[root];
        while (parent[value] != value) {
            const int next = parent[value];
            parent[value] = root;
            value = next;
        }
        return root;
    };
    const auto join = [&parent, &findRoot](int left, int right) {
        const int leftRoot = findRoot(left);
        const int rightRoot = findRoot(right);
        if (leftRoot != rightRoot) parent[rightRoot] = leftRoot;
    };
    for (const auto &triangle : triangles) {
        for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
            const int from = triangle[edgeIndex];
            const int to = triangle[(edgeIndex + 1) % 3];
            if (from == to) return false;
            join(from, to);
            PolygonShadowEdgeUse &use = edgeUses[edgeKey(from, to)];
            if (use.count == 0) {
                use.from = from;
                use.to = to;
            } else if (use.count == 1 && (use.from != to || use.to != from)) {
                return false;
            }
            if (++use.count > 2) return false;
        }
    }
    for (const PolygonShadowEdgeUse &use : edgeUses) {
        if (use.count != 2) return false;
    }

    QHash<int, QVector3D> componentOrigins;
    QHash<int, QVector3D> componentMinimums;
    QHash<int, QVector3D> componentMaximums;
    for (int index = 0; index < vertices.size(); ++index) {
        const int root = findRoot(index);
        const QVector3D &point = vertices[index];
        if (!componentOrigins.contains(root)) {
            componentOrigins.insert(root, point);
            componentMinimums.insert(root, point);
            componentMaximums.insert(root, point);
        } else {
            const QVector3D minimum = componentMinimums[root];
            const QVector3D maximum = componentMaximums[root];
            componentMinimums[root] = QVector3D(
                std::min(minimum.x(), point.x()),
                std::min(minimum.y(), point.y()),
                std::min(minimum.z(), point.z()));
            componentMaximums[root] = QVector3D(
                std::max(maximum.x(), point.x()),
                std::max(maximum.y(), point.y()),
                std::max(maximum.z(), point.z()));
        }
    }
    QHash<int, double> componentVolumes;
    for (const auto &triangle : triangles) {
        const int root = findRoot(triangle[0]);
        const QVector3D origin = componentOrigins.value(root);
        const QVector3D a = vertices[triangle[0]] - origin;
        const QVector3D b = vertices[triangle[1]] - origin;
        const QVector3D c = vertices[triangle[2]] - origin;
        const double volume =
            (static_cast<double>(a.x()) *
                 (static_cast<double>(b.y()) * c.z() -
                  static_cast<double>(b.z()) * c.y()) +
             static_cast<double>(a.y()) *
                 (static_cast<double>(b.z()) * c.x() -
                  static_cast<double>(b.x()) * c.z()) +
             static_cast<double>(a.z()) *
                 (static_cast<double>(b.x()) * c.y() -
                  static_cast<double>(b.y()) * c.x())) / 6.0;
        componentVolumes[root] += volume;
    }
    for (auto it = componentVolumes.cbegin(); it != componentVolumes.cend(); ++it) {
        const QVector3D extent = componentMaximums.value(it.key()) -
                                 componentMinimums.value(it.key());
        const double scale = std::max({static_cast<double>(extent.x()),
                                       static_cast<double>(extent.y()),
                                       static_cast<double>(extent.z())});
        const double volumeTolerance =
            std::max(1.0e-30, scale * scale * scale * 1.0e-12);
        if (!std::isfinite(it.value()) || it.value() <= volumeTolerance) {
            // Inward-oriented, flat, or numerically indeterminate components
            // are left to the conservative depth-map fallback.
            return false;
        }
    }

    *shadowVertices = std::move(vertices);
    *shadowTriangles = std::move(triangles);
    return true;
}

int workbenchTextureInterpolation(const QString &interpolation)
{
    if (interpolation.compare(QStringLiteral("Closest"), Qt::CaseInsensitive) == 0)
        return 1;
    // Workbench uses the GPU's filtered sampler for Linear, Cubic, and Smart.
    return 0;
}

int workbenchTextureExtension(const QString &extension)
{
    if (extension.compare(QStringLiteral("EXTEND"), Qt::CaseInsensitive) == 0)
        return 1;
    if (extension.compare(QStringLiteral("CLIP"), Qt::CaseInsensitive) == 0)
        return 2;
    if (extension.compare(QStringLiteral("MIRROR"), Qt::CaseInsensitive) == 0)
        return 3;
    return 0;
}

void configureWorkbenchImageTexture(GLuint texture,
                                    const WorkbenchMaterialData &material)
{
    if (texture == 0) return;
    const bool closest = workbenchTextureInterpolation(
        material.textureImageInterpolation) == 1;
    const GLint filter = closest ? GL_NEAREST : GL_LINEAR;
    const QString extension = material.textureImageExtension.toUpper();
    const GLint wrap = extension == QStringLiteral("CLIP")
        ? GL_CLAMP_TO_BORDER
        : extension == QStringLiteral("EXTEND") ? GL_CLAMP_TO_EDGE
        : extension == QStringLiteral("MIRROR") ? GL_MIRRORED_REPEAT
                                                 : GL_REPEAT;
    QOpenGLFunctions *functions = QOpenGLContext::currentContext()->functions();
    functions->glBindTexture(GL_TEXTURE_2D, texture);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    if (wrap == GL_CLAMP_TO_BORDER) {
        const GLfloat transparent[4] = {0.f, 0.f, 0.f, 0.f};
        functions->glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
                                    transparent);
    }
}

void configureWorkbenchUdimTexture(GLuint texture,
                                   const WorkbenchMaterialData &material)
{
    if (texture == 0) return;
    const bool closest = workbenchTextureInterpolation(
        material.textureImageInterpolation) == 1;
    const QString extension = material.textureImageExtension.toUpper();
    const GLint wrap = extension == QStringLiteral("CLIP")
        ? GL_CLAMP_TO_BORDER
        : extension == QStringLiteral("EXTEND") ? GL_CLAMP_TO_EDGE
        : extension == QStringLiteral("MIRROR") ? GL_MIRRORED_REPEAT
                                                  : GL_REPEAT;
    QOpenGLFunctions *functions = QOpenGLContext::currentContext()->functions();
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousBinding = 0;
    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    functions->glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousBinding);
    functions->glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    functions->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                                closest ? GL_NEAREST : GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER,
                                closest ? GL_NEAREST : GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,
                                wrap);
    functions->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,
                                wrap);
    if (wrap == GL_CLAMP_TO_BORDER) {
        const GLfloat transparent[4] = {0.f, 0.f, 0.f, 0.f};
        functions->glTexParameterfv(GL_TEXTURE_2D_ARRAY,
                                    GL_TEXTURE_BORDER_COLOR, transparent);
    }
    functions->glBindTexture(GL_TEXTURE_2D_ARRAY,
                              static_cast<GLuint>(previousBinding));
    functions->glActiveTexture(static_cast<GLenum>(previousActiveTexture));
}

QVector3D workbenchShadowDirectionWorld(
    const ViewportShadingSettings &settings)
{
    // Blender swaps Y/Z, then flips X/Z before Workbench shadow-volume
    // extrusion. Keep the same scene-display direction convention here.
    QVector3D direction(-settings.shadowDirection.x(),
                        settings.shadowDirection.z(),
                        -settings.shadowDirection.y());
    if (direction.lengthSquared() <= 1.0e-10f) {
        direction = QVector3D(-0.57735026f, 0.57735026f, -0.57735026f);
    }
    direction.normalize();
    return direction;
}

Point3D normalized(Point3D value, const Point3D &fallback)
{
    const qreal length = std::sqrt(value.x * value.x + value.y * value.y +
                                    value.z * value.z);
    if (!std::isfinite(length) || length <= 1.0e-12) {
        return fallback;
    }
    value.x /= length;
    value.y /= length;
    value.z /= length;
    return value;
}

Point3D cross(const Point3D &a, const Point3D &b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

QVector3D asVector(const Point3D &point)
{
    return {static_cast<float>(point.x),
            static_cast<float>(point.y),
            static_cast<float>(point.z)};
}

QVector3D transformMeshNormal(const std::array<double, 16> &m,
                              const Point3D &normal)
{
    const double a=m[0], b=m[1], c=m[2], d=m[4], e=m[5], f=m[6];
    const double g=m[8], h=m[9], i=m[10];
    const double cof[9] = {e*i-f*h, f*g-d*i, d*h-e*g,
                           c*h-b*i, a*i-c*g, b*g-a*h,
                           b*f-c*e, c*d-a*f, a*e-b*d};
    const double determinant = a*cof[0] + b*cof[1] + c*cof[2];
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-15)
        return {};
    QVector3D result(
        static_cast<float>((cof[0]*normal.x + cof[1]*normal.y + cof[2]*normal.z) / determinant),
        static_cast<float>((cof[3]*normal.x + cof[4]*normal.y + cof[5]*normal.z) / determinant),
        static_cast<float>((cof[6]*normal.x + cof[7]*normal.y + cof[8]*normal.z) / determinant));
    if (result.lengthSquared() <= 1.0e-16f) return {};
    return result.normalized();
}

bool hasNurbsSurfaceGeometry(const ViewportRenderObject &object)
{
    return object.shape.geometryType == GeometryType::NurbsSurface ||
           object.shape.geometryType == GeometryType::NurbsSolid;
}

bool hasPolygonMeshGeometry(const ViewportRenderObject &object)
{
    return object.shape.geometryType == GeometryType::PolygonMesh &&
           validatePolygonMesh(object.shape.polygonMesh);
}

constexpr int kWorkbenchCavityMaxSamples = 512;

quint32 blenderStringHash(const QByteArray &bytes, quint32 seed = 0)
{
    constexpr quint32 multiplier = 0x5bd1e995U;
    const uchar *data = reinterpret_cast<const uchar *>(bytes.constData());
    int remaining = bytes.size();
    quint32 hash = seed ^ static_cast<quint32>(remaining);
    while (remaining >= 4) {
        quint32 chunk = static_cast<quint32>(data[0]) |
                        (static_cast<quint32>(data[1]) << 8) |
                        (static_cast<quint32>(data[2]) << 16) |
                        (static_cast<quint32>(data[3]) << 24);
        chunk *= multiplier;
        chunk ^= chunk >> 24;
        chunk *= multiplier;
        hash *= multiplier;
        hash ^= chunk;
        data += 4;
        remaining -= 4;
    }
    if (remaining == 3) hash ^= static_cast<quint32>(data[2]) << 16;
    if (remaining >= 2) hash ^= static_cast<quint32>(data[1]) << 8;
    if (remaining >= 1) {
        hash ^= data[0];
        hash *= multiplier;
    }
    hash ^= hash >> 13;
    hash *= multiplier;
    hash ^= hash >> 15;
    return hash;
}

QColor workbenchRandomColor(quint64 objectId,
                            const QString &displayName,
                            const QString &libraryPath)
{
    quint32 hash = 0;
    if (!displayName.isEmpty()) {
        hash = blenderStringHash(QByteArrayLiteral("OB") + displayName.toUtf8());
        if (!libraryPath.isEmpty()) {
            hash = blenderStringHash(libraryPath.toUtf8(), hash);
        }
    } else {
        hash = static_cast<quint32>(objectId) ^
               static_cast<quint32>(objectId >> 32);
        hash ^= hash >> 16;
        hash *= 0x7feb352dU;
        hash ^= hash >> 15;
        hash *= 0x846ca68bU;
        hash ^= hash >> 16;
    }
    const qreal hue = static_cast<qreal>(hash) / 4294967296.0;
    return QColor::fromHsvF(hue, 0.5, 0.8);
}

qreal workbenchSourceAlpha(ViewportColorMode colorMode,
                           qreal objectAlpha,
                           const WorkbenchMaterialData &material,
                           const QColor &customColor)
{
    switch (colorMode) {
    case ViewportColorMode::Material:
    case ViewportColorMode::Attribute:
    case ViewportColorMode::Texture:
        return material.alpha;
    case ViewportColorMode::Object:
    case ViewportColorMode::Random:
        return objectAlpha;
    case ViewportColorMode::Custom:
        Q_UNUSED(customColor);
        return workbenchCustomColorSourceAlpha;
    }
    return 1.0;
}

qreal workbenchEffectiveAlpha(qreal sourceAlpha,
                              const ViewportShadingSettings &settings,
                              bool previewOverlay)
{
    if (previewOverlay) {
        return 0.5;
    }
    return std::clamp(sourceAlpha *
                          (settings.xrayEnabled() ? settings.xrayAlpha : 1.0),
                      0.0, 1.0);
}

} // namespace

ViewportSurfaceRenderer::~ViewportSurfaceRenderer()
{
    if (QOpenGLContext::currentContext() == nullptr) {
        return;
    }
    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.destroy();
    }
    if (shadowVolumeVertexBuffer_.isCreated()) {
        shadowVolumeVertexBuffer_.destroy();
    }
    if (shadowVolumeVertexArray_.isCreated()) {
        shadowVolumeVertexArray_.destroy();
    }
    if (outlineVertexArray_.isCreated()) {
        outlineVertexArray_.destroy();
    }
    if (vertexArray_.isCreated()) {
        vertexArray_.destroy();
    }
    if (matcapTexture_ != 0) {
        glDeleteTextures(1, &matcapTexture_);
        matcapTexture_ = 0;
    }
    for (GLuint texture : imageTextures_) {
        if (texture != 0) glDeleteTextures(1, &texture);
    }
    imageTextures_.clear();
    for (const UdimTextureSet &textures : udimImageTextures_) {
        if (textures.imageArray != 0) glDeleteTextures(1, &textures.imageArray);
        if (textures.tileLookup != 0) glDeleteTextures(1, &textures.tileLookup);
    }
    udimImageTextures_.clear();
    if (agxDisplayTexture_ != 0 && agxDisplayTextureTransform_ != 4) {
        glDeleteTextures(1, &agxDisplayTexture_);
    }
    for (GLuint texture : customDisplayTextures_)
        if (texture != 0) glDeleteTextures(1, &texture);
    customDisplayTextures_.clear();
    agxDisplayTexture_ = 0;
    if (cavityJitterTexture_ != 0) {
        glDeleteTextures(1, &cavityJitterTexture_);
        cavityJitterTexture_ = 0;
    }
    if (shadowTexture_ != 0) {
        glDeleteTextures(1, &shadowTexture_);
        shadowTexture_ = 0;
    }
    if (shadowFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &shadowFramebuffer_);
        shadowFramebuffer_ = 0;
    }
    if (objectIdFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &objectIdFramebuffer_);
        objectIdFramebuffer_ = 0;
    }
    if (objectIdDepthStencil_ != 0) {
        glDeleteRenderbuffers(1, &objectIdDepthStencil_);
        objectIdDepthStencil_ = 0;
    }
    if (objectIdTexture_ != 0) {
        glDeleteTextures(1, &objectIdTexture_);
        objectIdTexture_ = 0;
    }
    if (workbenchEffectFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &workbenchEffectFramebuffer_);
        workbenchEffectFramebuffer_ = 0;
    }
    if (frontMergeFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &frontMergeFramebuffer_);
        frontMergeFramebuffer_ = 0;
    }
    if (workbenchOutlineIdTexture_ != 0) {
        glDeleteTextures(1, &workbenchOutlineIdTexture_);
        workbenchOutlineIdTexture_ = 0;
    }
    if (cavityDepthFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &cavityDepthFramebuffer_);
        cavityDepthFramebuffer_ = 0;
    }
    if (cavityDepthTexture_ != 0) {
        glDeleteTextures(1, &cavityDepthTexture_);
        cavityDepthTexture_ = 0;
    }
    if (transparencyFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &transparencyFramebuffer_);
        transparencyFramebuffer_ = 0;
    }
    if (transparencyAccumulationTexture_ != 0) {
        glDeleteTextures(1, &transparencyAccumulationTexture_);
        transparencyAccumulationTexture_ = 0;
    }
    if (transparencyWeightTexture_ != 0) {
        glDeleteTextures(1, &transparencyWeightTexture_);
        transparencyWeightTexture_ = 0;
    }
    if (frontTransparencyFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &frontTransparencyFramebuffer_);
        frontTransparencyFramebuffer_ = 0;
    }
    if (frontTransparencyAccumulationTexture_ != 0) {
        glDeleteTextures(1, &frontTransparencyAccumulationTexture_);
        frontTransparencyAccumulationTexture_ = 0;
    }
    if (frontTransparencyWeightTexture_ != 0) {
        glDeleteTextures(1, &frontTransparencyWeightTexture_);
        frontTransparencyWeightTexture_ = 0;
    }
    program_.removeAllShaders();
    outlineProgram_.removeAllShaders();
    objectIdProgram_.removeAllShaders();
    workbenchEffectProgram_.removeAllShaders();
    shadowProgram_.removeAllShaders();
    transparencyResolveProgram_.removeAllShaders();
    frontMergeProgram_.removeAllShaders();
}

bool ViewportSurfaceRenderer::initialize()
{
    if (initializationAttempted_) {
        return initialized_;
    }
    initializationAttempted_ = true;
    static constexpr const char *vertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec3 aAttributeColor;
        layout(location = 3) in vec2 aUv;
        uniform mat4 uViewProjection;
        uniform vec3 uWorldOffset;
        uniform mat4 uShadowViewProjection;
        out vec3 vNormalView;
        out vec3 vAttributeColor;
        out vec2 vUv;
        out vec3 vWorldPosition;
        out vec4 vShadowPosition;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        void main()
        {
            vec3 position = aPosition + uWorldOffset;
            vNormalView = vec3(dot(aNormal, uViewRight),
                               dot(aNormal, uViewUp),
                               dot(aNormal, uViewFacing));
            vAttributeColor = aAttributeColor;
            vUv = aUv;
            vWorldPosition = position;
            vShadowPosition = uShadowViewProjection * vec4(position, 1.0);
            gl_Position = uViewProjection * vec4(position, 1.0);
        }
    )glsl";
    // The screen-space cavity and normal-curvature equations below adapt
    // Blender 5.2.2 Workbench shaders. Blender Authors; GPL-2.0-or-later.
    static constexpr const char *fragmentShader = R"glsl(
        #version 330 core
        in vec3 vNormalView;
        in vec3 vAttributeColor;
        in vec2 vUv;
        in vec3 vWorldPosition;
        in vec4 vShadowPosition;
        uniform vec4 uBaseColor;
        uniform bool uUseAttributeColor;
        uniform bool uUseTextureColor;
        uniform sampler2D uWorkbenchImage;
        uniform bool uUseUdimImage;
        uniform sampler2DArray uWorkbenchUdimImage;
        uniform sampler2D uWorkbenchUdimLookup;
        uniform int uTextureInterpolation;
        uniform int uTextureExtension;
        uniform bool uTexturePremultiplied;
        uniform bool uTextureAlphaCutoff;
        ivec2 mapUdimCoord(ivec2 p, ivec2 size, out bool valid)
        {
            valid = true;
            for (int axis = 0; axis < 2; ++axis) {
                int n = size[axis];
                int c = p[axis];
                if (uTextureExtension == 0) c = (c % n + n) % n;
                else if (uTextureExtension == 1) c = clamp(c, 0, n - 1);
                else if (uTextureExtension == 2) {
                    if (c < 0 || c >= n) valid = false;
                }
                else {
                    c = (c % (2 * n) + 2 * n) % (2 * n);
                    if (c >= n) c = 2 * n - 1 - c;
                }
                p[axis] = c;
            }
            return p;
        }
        vec4 fetchWorkbenchUdim(sampler2DArray image, vec4 info, ivec2 p)
        {
            ivec2 size = ivec2(round(vec2(textureSize(image, 0).xy) * info.yz));
            bool valid;
            ivec2 mapped = mapUdimCoord(p, size, valid);
            if (!valid) return vec4(0.0);
            return texelFetch(image, ivec3(mapped, int(info.x + 0.5) - 1), 0);
        }
        vec4 sampleWorkbenchUdimTexture(sampler2DArray image,
                                        sampler2D lookup, vec2 uv)
        {
            vec2 tile = floor(uv);
            if (tile.x < 0.0 || tile.x >= 10.0 ||
                tile.y < 0.0 || tile.y >= 100.0) return vec4(1.0, 0.0, 1.0, 1.0);
            vec4 info = texelFetch(lookup, ivec2(tile), 0);
            if (info.x < 0.5) return vec4(1.0, 0.0, 1.0, 1.0);
            ivec2 size = ivec2(round(vec2(textureSize(image, 0).xy) * info.yz));
            vec2 p = fract(uv) * vec2(size) - 0.5;
            ivec2 base = ivec2(floor(p));
            vec2 f = fract(p);
            if (uTextureInterpolation == 1)
                return fetchWorkbenchUdim(image, info,
                    ivec2(floor(fract(uv) * vec2(size))));
            if (uTextureInterpolation != 2) {
                vec4 a = mix(fetchWorkbenchUdim(image, info, base),
                             fetchWorkbenchUdim(image, info, base + ivec2(1, 0)), f.x);
                vec4 b = mix(fetchWorkbenchUdim(image, info, base + ivec2(0, 1)),
                             fetchWorkbenchUdim(image, info, base + ivec2(1, 1)), f.x);
                return mix(a, b, f.y);
            }
            vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
            vec2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
            vec2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
            vec2 w3 = f * f * f / 6.0;
            vec4 result = vec4(0.0);
            for (int y = 0; y < 4; ++y) {
                float wy = y == 0 ? w0.y : y == 1 ? w1.y : y == 2 ? w2.y : w3.y;
                for (int x = 0; x < 4; ++x) {
                    float wx = x == 0 ? w0.x : x == 1 ? w1.x : x == 2 ? w2.x : w3.x;
                    result += fetchWorkbenchUdim(image, info,
                        base + ivec2(x - 1, y - 1)) * wx * wy;
                }
            }
            return result;
        }
        vec4 sampleWorkbenchUdimLinear(vec2 uv)
        {
            return sampleWorkbenchUdimTexture(uWorkbenchUdimImage,
                                               uWorkbenchUdimLookup, uv);
        }
        vec4 sampleWorkbenchUdim(vec2 uv)
        {
            return sampleWorkbenchUdimTexture(uWorkbenchUdimImage,
                                               uWorkbenchUdimLookup, uv);
        }
        vec4 sampleWorkbenchImage(vec2 uv)
        {
            if (uUseUdimImage) return sampleWorkbenchUdim(uv);
            if (uTextureInterpolation == 1) {
                return texture(uWorkbenchImage, uv);
            }
            if (uTextureInterpolation != 2) {
                return texture(uWorkbenchImage, uv);
            }
            vec2 size = vec2(textureSize(uWorkbenchImage, 0));
            vec2 p = uv * size - 0.5;
            vec2 base = floor(p);
            vec2 f = fract(p);
            vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
            vec2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
            vec2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
            vec2 w3 = f * f * f / 6.0;
            vec4 result = vec4(0.0);
            for (int y = 0; y < 4; ++y) {
                float wy = y == 0 ? w0.y : y == 1 ? w1.y : y == 2 ? w2.y : w3.y;
                for (int x = 0; x < 4; ++x) {
                    float wx = x == 0 ? w0.x : x == 1 ? w1.x : x == 2 ? w2.x : w3.x;
                    result += texture(uWorkbenchImage,
                        (base + vec2(float(x - 1), float(y - 1)) + 0.5) / size) * wx * wy;
                }
            }
            return result;
        }
        uniform bool uEditModeSelected;
        uniform vec3 uEditSelectionColor;
        uniform float uEditSelectionMix;
        uniform vec3 uCameraPosition;
        uniform float uOrthographicTransparencyDepthOffset;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        uniform bool uPerspective;
        uniform int uLightingMode;
        uniform sampler2DArray uMatcapTexture;
        uniform bool uMatcapAvailable;
        uniform bool uMatcapFlipX;
        uniform sampler3D uAgxDisplayTexture;
        uniform bool uAgxDisplayAvailable;
        uniform int uAgxDisplayLutSize;
        uniform vec2 uAgxDisplayLutDomain;
        uniform int uAgxDisplayLutShaper;
        uniform float uAgxDisplayLutShaperScale;
        uniform int uColorTransform;
        uniform float uColorExposure;
        uniform float uColorGamma;
        uniform bool uSceneLinearOutput;
        uniform vec3 uLightDirection[4];
        uniform vec3 uLightDiffuse[4];
        uniform vec3 uLightSpecular[4];
        uniform float uLightWrap[4];
        uniform vec3 uAmbientColor;
        uniform float uRoughness;
        uniform float uMetallic;
        uniform bool uUseSpecular;
        uniform sampler2DShadow uShadowMap;
        uniform bool uUseShadows;
        uniform bool uUseShadowMap;
        uniform bool uShadowVolumeShadowed;
        uniform float uShadowIntensity;
        uniform float uShadowBias;
        uniform float uShadowFocus;
        uniform float uShadowShift;
        uniform vec3 uShadowDirectionView;
        uniform bool uUseCavity;
        uniform bool uUseSsao;
        uniform bool uUseCurvature;
        uniform sampler2D uCavityDepth;
        uniform sampler2D uCavityNormal;
        uniform usampler2D uCavityObjectId;
        uniform sampler2D uCavityJitterTexture;
        uniform mat4 uInverseViewProjection;
        uniform vec2 uCavityProjectionScale;
        uniform float uCavityUiScale;
        uniform float uCavityDistance;
        uniform float uCavityAttenuation;
        uniform float uCavityValleyFactor;
        uniform float uCavityRidgeFactor;
        uniform float uCurvatureRidgeControl;
        uniform float uCurvatureValleyControl;
        uniform int uCavitySampleStart;
        uniform int uCavitySampleEnd;
        uniform int uCavitySamplesPerIteration;
        uniform float uCavitySampleCountInv;
        uniform bool uTransparencyAccumulation;
        uniform bool uFaceAwareNormals;
        uniform bool uExcludeFrontCoverage;
        uniform sampler2D uFrontDepthMask;
        out vec4 fragmentColor;
        layout(location = 1) out vec4 transparencyWeight;

        float fastReciprocal(float value)
        {
            return intBitsToFloat(0x7eef370b - floatBitsToInt(value));
        }

        float wrappedLighting(float normalLight, float wrap)
        {
            float wrapPlusOne = wrap + 1.0;
            return clamp((normalLight + wrap) *
                         fastReciprocal(wrapPlusOne * wrapPlusOne),
                         0.0, 1.0);
        }

        vec3 workbenchStudioLighting(vec3 baseColor,
                                     float roughness,
                                     float metallic,
                                     vec3 normal,
                                     vec3 incident)
        {
            vec3 diffuseColor;
            vec3 specularColor;
            if (uUseSpecular) {
                diffuseColor = mix(baseColor, vec3(0.0), metallic);
                specularColor = mix(vec3(0.05), baseColor, metallic);
            } else {
                diffuseColor = baseColor;
                specularColor = vec3(0.0);
            }

            vec3 specularLight = uAmbientColor;
            vec3 diffuseLight = uAmbientColor;
            vec3 reflectedIncident = -reflect(incident, normal);

            if (uUseSpecular) {
                for (int index = 0; index < 4; ++index) {
                    vec3 light = uLightDirection[index];
                    vec3 halfDirection = normalize(light + incident);
                    float wrappedNormalLight = dot(light, reflectedIncident);
                    float specularAngle = clamp(dot(halfDirection, normal),
                                               0.0, 1.0);
                    float specularNormalLight = clamp(dot(light, normal),
                                                      0.0, 1.0);
                    float gloss = (1.0 - roughness) *
                                  (1.0 - uLightWrap[index]);
                    float shininess = exp2(10.0 * gloss + 1.0);
                    float specularAmount = pow(specularAngle, shininess) *
                        specularNormalLight * (shininess * 0.125 + 1.0);
                    float environmentWrap = mix(uLightWrap[index], 1.0,
                                                roughness);
                    float environment = wrappedLighting(wrappedNormalLight,
                                                        environmentWrap);
                    specularAmount = mix(specularAmount, environment,
                                         uLightWrap[index] * uLightWrap[index]);
                    specularLight += specularAmount * uLightSpecular[index];
                }
                float normalView = clamp(dot(normal, incident), 0.0, 1.0);
                float fresnel = exp2(-8.35 * normalView) * (1.0 - roughness);
                specularColor = mix(specularColor, vec3(1.0), fresnel);
            }
            // Blender multiplies this ambient/specular term even when the
            // specular toggle is off; specularColor is then zero.
            specularLight *= specularColor;

            for (int index = 0; index < 4; ++index) {
                float normalLight = dot(uLightDirection[index], normal);
                diffuseLight += wrappedLighting(normalLight, uLightWrap[index]) *
                                uLightDiffuse[index];
            }

            if (uUseSpecular) {
                float specularEnergy = dot(specularColor, vec3(0.33333));
                diffuseLight *= diffuseColor * (1.0 - specularEnergy);
            } else {
                diffuseLight *= diffuseColor;
            }
            return diffuseLight + specularLight;
        }

        vec3 sceneLinearToSrgb(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color * 12.92;
            vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
            return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
        }

        vec3 srgbToDisplayLinear(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color / 12.92;
            vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
            return mix(high, low, lessThanEqual(color, vec3(0.04045)));
        }

        vec3 workbenchDisplayLutCoordinate(vec3 color)
        {
            vec3 bounded = clamp(color, vec3(uAgxDisplayLutDomain.x),
                                 vec3(uAgxDisplayLutDomain.y)) -
                           vec3(uAgxDisplayLutDomain.x);
            float span = max(uAgxDisplayLutDomain.y - uAgxDisplayLutDomain.x,
                             1.0e-20);
            vec3 normalized = bounded / span;
            if (uAgxDisplayLutShaper == 1) {
                float denominator = log2(1.0 + uAgxDisplayLutShaperScale * span);
                normalized = log2(vec3(1.0) +
                                  uAgxDisplayLutShaperScale * bounded) /
                             max(denominator, 1.0e-20);
            }
            return (clamp(normalized, vec3(0.0), vec3(1.0)) *
                    float(uAgxDisplayLutSize - 1) + 0.5) /
                   float(uAgxDisplayLutSize);
        }

        vec3 sceneLinearToAgxSrgb(vec3 color)
        {
            color = max(color, vec3(0.0)) * exp2(uColorExposure);
            vec3 displayColor;
            if (uColorTransform == 1) {
                displayColor = sceneLinearToSrgb(color);
            } else if (uColorTransform == 4 && !uAgxDisplayAvailable) {
                displayColor = color;
            } else if (!uAgxDisplayAvailable) {
                displayColor = sceneLinearToSrgb(color);
            } else {
                vec3 coordinate = workbenchDisplayLutCoordinate(color);
                displayColor = texture(uAgxDisplayTexture, coordinate).rgb;
            }
            return pow(max(displayColor, vec3(0.0)),
                       vec3(1.0 / max(uColorGamma, 0.000001)));
        }

        vec3 cavityNoise(vec2 screenCoordinate, ivec2 pixelSize)
        {
            // Blender samples a repeating 64x64 texture at one texel per
            // viewport pixel. Its RGB values are the rotated direction and
            // sample-radius offset, built from the shared blue-noise LUT.
            vec2 jitterCoordinate = screenCoordinate * vec2(pixelSize) *
                                    (1.0 / 64.0);
            return texture(uCavityJitterTexture, jitterCoordinate).rgb;
        }

        float radicalInverseVdc(uint bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) |
                   ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) |
                   ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) |
                   ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) |
                   ((bits & 0xFF00FF00u) >> 8u);
            return float(bits) * 2.3283064365386963e-10;
        }

        vec3 cavityViewPosition(vec2 screenCoordinate, float depth)
        {
            vec4 worldPosition = uInverseViewProjection *
                vec4(screenCoordinate * 2.0 - 1.0,
                     depth * 2.0 - 1.0, 1.0);
            if (abs(worldPosition.w) <= 1.0e-8) {
                return vec3(0.0);
            }
            worldPosition.xyz /= worldPosition.w;
            vec3 relative = worldPosition.xyz - uCameraPosition;
            return vec3(dot(relative, uViewRight),
                        dot(relative, uViewUp),
                        dot(relative, uViewFacing));
        }

        vec2 workbenchCavity(vec2 screenCoordinate,
                             float depth,
                             vec3 position,
                             vec3 normal)
        {
            if (depth == 1.0 || depth == 0.0) {
                return vec2(0.0);
            }

            vec3 noise = cavityNoise(screenCoordinate,
                                     textureSize(uCavityNormal, 0));
            float homcoord = uPerspective ? max(abs(position.z), 1.0e-6) : 1.0;
            vec2 offset = uCavityProjectionScale *
                          (uCavityDistance / homcoord) * 0.5;
            vec2 rotX = noise.rg;
            vec2 rotY = vec2(-rotX.y, rotX.x);
            float cavities = 0.0;
            float edges = 0.0;
            for (int index = uCavitySampleStart;
                 index < uCavitySampleEnd && index < 512;
                 ++index) {
                float iterationOffset =
                    float(index / uCavitySamplesPerIteration) * 0.499;
                float radius = fract((float(index) + 0.5 + iterationOffset) /
                                     float(uCavitySamplesPerIteration));
                float angle = radicalInverseVdc(uint(index)) *
                              6.28318530718 + iterationOffset;
                vec2 sampleDirection = vec2(cos(angle), sin(angle));
                vec2 jitteredDirection = vec2(
                    dot(sampleDirection, rotX), dot(sampleDirection, rotY));
                jitteredDirection *= radius + noise.b;
                vec2 sampleCoordinate = screenCoordinate +
                                        jitteredDirection * offset;
                if (any(greaterThan(abs(sampleCoordinate - 0.5),
                                    vec2(0.5)))) {
                    continue;
                }

                float sampleDepth = texture(uCavityDepth, sampleCoordinate).r;
                bool isBackground = sampleDepth == 1.0;
                if (isBackground) {
                    sampleDepth = depth;
                }
                vec3 samplePosition = cavityViewPosition(sampleCoordinate,
                                                         sampleDepth);
                if (isBackground) {
                    samplePosition.z -= uCavityDistance;
                }
                vec3 direction = samplePosition - position;
                float lengthToSample = length(direction);
                float cavityDot = dot(direction, normal);
                float edgeDot = -cavityDot;
                float bias = 0.05 * lengthToSample + 0.0001;
                float attenuation = 1.0 /
                    max(lengthToSample *
                            (1.0 + lengthToSample * lengthToSample *
                                       uCavityAttenuation),
                        1.0e-8);
                if (cavityDot > -bias) {
                    cavities += cavityDot * attenuation;
                }
                if (edgeDot > bias) {
                    edges += edgeDot * attenuation;
                }
            }
            cavities *= uCavitySampleCountInv;
            edges *= uCavitySampleCountInv;
            cavities = clamp(cavities * uCavityValleyFactor, 0.0, 1.0);
            edges *= uCavityRidgeFactor;
            return vec2(cavities, edges);
        }

        float curvatureSoftClamp(float curvature, float control)
        {
            if (curvature < 0.5 / control) {
                return curvature * (1.0 - curvature * control);
            }
            return 0.25 / control;
        }

        float workbenchCurvature(vec2 screenCoordinate, ivec2 size)
        {
            // Blender offsets these samples by ui_scale in normalized
            // viewport coordinates. Keep the fractional scale (for example,
            // on non-integer device pixel ratios) instead of rounding it to
            // a whole texel before sampling.
            vec2 offset = vec2(uCavityUiScale) / vec2(size);
            vec2 upCoordinate = screenCoordinate + vec2(0.0, offset.y);
            vec2 downCoordinate = screenCoordinate - vec2(0.0, offset.y);
            vec2 rightCoordinate = screenCoordinate + vec2(offset.x, 0.0);
            vec2 leftCoordinate = screenCoordinate - vec2(offset.x, 0.0);
            uint objectUp = texture(uCavityObjectId, upCoordinate).r;
            uint objectDown = texture(uCavityObjectId, downCoordinate).r;
            uint objectRight = texture(uCavityObjectId, rightCoordinate).r;
            uint objectLeft = texture(uCavityObjectId, leftCoordinate).r;
            if (objectUp != objectDown || objectRight != objectLeft ||
                (objectUp == 0u && objectRight == 0u)) {
                return 0.0;
            }
            float normalUp = texture(uCavityNormal, upCoordinate).g;
            float normalDown = texture(uCavityNormal, downCoordinate).g;
            float normalRight = texture(uCavityNormal, rightCoordinate).r;
            float normalLeft = texture(uCavityNormal, leftCoordinate).r;
            float normalDifference = (normalUp - normalDown) +
                                     (normalRight - normalLeft);
            if (normalDifference < 0.0) {
                return -2.0 * curvatureSoftClamp(-normalDifference,
                                                 uCurvatureValleyControl);
            }
            return 2.0 * curvatureSoftClamp(normalDifference,
                                            uCurvatureRidgeControl);
        }

        vec2 workbenchMatcapUv(vec3 incident, vec3 normal)
        {
            float a = 1.0 / (1.0 + incident.z);
            float b = -incident.x * incident.y * a;
            vec3 basis1 = vec3(1.0 - incident.x * incident.x * a,
                               b,
                               -incident.x);
            vec3 basis2 = vec3(b,
                               1.0 - incident.y * incident.y * a,
                               -incident.y);
            return vec2(dot(basis1, normal), dot(basis2, normal)) * 0.496 + 0.5;
        }

        void main()
        {
            if (uTransparencyAccumulation && uExcludeFrontCoverage &&
                texelFetch(uFrontDepthMask, ivec2(gl_FragCoord.xy), 0).r < 1.0) {
                discard;
            }
            vec3 normal = normalize(vNormalView);
            if (uFaceAwareNormals && !gl_FrontFacing) {
                normal = -normal;
            }
            vec3 incident = vec3(0.0, 0.0, 1.0);
            if (uPerspective) {
                vec3 incidentWorld = normalize(uCameraPosition - vWorldPosition);
                // Studio lights and normals share the same camera-following
                // view-space frame used by Blender Workbench.
                incident = normalize(vec3(dot(incidentWorld, uViewRight),
                                          dot(incidentWorld, uViewUp),
                                          dot(incidentWorld, uViewFacing)));
            }
            vec3 textureColor = vec3(1.0);
            if (uUseTextureColor) {
                vec4 imageColor = sampleWorkbenchImage(vUv);
                if (uTextureAlphaCutoff && imageColor.a < 0.1) {
                    discard;
                }
                if (uTexturePremultiplied && imageColor.a != 0.0 &&
                    imageColor.a != 1.0) {
                    imageColor.rgb /= imageColor.a;
                }
                textureColor = imageColor.rgb;
            }
            vec3 baseColor = uUseTextureColor
                                 ? textureColor
                                 : (uUseAttributeColor ? vAttributeColor
                                                       : uBaseColor.rgb);
            vec3 color = baseColor;
            if (uLightingMode == 0) {
                color = workbenchStudioLighting(baseColor,
                                                uRoughness,
                                                uMetallic,
                                                normal,
                                                incident);
            } else if (uLightingMode == 1 && uMatcapAvailable) {
                vec2 uv = workbenchMatcapUv(incident, normal);
                if (uMatcapFlipX) {
                    uv.x = 1.0 - uv.x;
                }
                vec3 diffuse = texture(uMatcapTexture, vec3(uv, 0.0)).rgb;
                vec3 specular = uUseSpecular
                                    ? texture(uMatcapTexture,
                                              vec3(uv, 1.0)).rgb
                                    : vec3(0.0);
                color = diffuse * baseColor + specular;
            }
            if (uUseCavity) {
                ivec2 size = textureSize(uCavityNormal, 0);
                vec2 screenCoordinate = gl_FragCoord.xy / vec2(size);
                float cavityAmount = 0.0;
                float edgeAmount = 0.0;
                if (uUseSsao) {
                    float depth = texture(uCavityDepth,
                                          screenCoordinate).r;
                    vec2 cavity = workbenchCavity(
                        screenCoordinate, depth,
                        cavityViewPosition(screenCoordinate, depth),
                        normal);
                    cavityAmount = cavity.x;
                    edgeAmount = cavity.y;
                }
                float curvature = uUseCurvature
                                      ? workbenchCurvature(screenCoordinate, size)
                                      : 0.0;
                color *= clamp((1.0 - cavityAmount) * (1.0 + edgeAmount) *
                                   (1.0 + curvature),
                               0.0, 4.0);
            }
            if (uUseShadows) {
                float visibility = 1.0;
                if (uUseShadowMap) {
                    vec3 shadowCoordinate =
                        vShadowPosition.xyz / vShadowPosition.w * 0.5 + 0.5;
                    if (all(greaterThanEqual(shadowCoordinate.xy, vec2(0.0))) &&
                        all(lessThanEqual(shadowCoordinate.xy, vec2(1.0))) &&
                        shadowCoordinate.z >= 0.0 && shadowCoordinate.z <= 1.0) {
                        vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
                        visibility = 0.0;
                        for (int y = -1; y <= 1; ++y) {
                            for (int x = -1; x <= 1; ++x) {
                                visibility += texture(
                                    uShadowMap,
                                    vec3(shadowCoordinate.xy + vec2(x, y) * texel,
                                         shadowCoordinate.z - uShadowBias));
                            }
                        }
                        visibility /= 9.0;
                    }
                }
                float focus = clamp(uShadowFocus, 0.0001, 0.99999);
                float focusEdge = 1.0 - focus * (1.0 - uShadowShift);
                float lightFactor = -dot(normal, uShadowDirectionView);
                float shadowMix = smoothstep(uShadowShift, focusEdge,
                                             lightFactor);
                float shadowVisibility = uShadowVolumeShadowed ? 0.0 : visibility;
                shadowMix *= shadowVisibility *
                             clamp(uShadowIntensity, 0.0, 1.0);
                shadowMix += 1.0 - clamp(uShadowIntensity, 0.0, 1.0);
                color *= shadowMix;
            }
            if (uTransparencyAccumulation) {
                // Blender 5.2.2 Workbench's Eq. 7 weight, using eye-space
                // depth. Accumulate lit scene-linear color; the OIT resolve
                // applies the active display transform before compositing.
                float alpha = uBaseColor.a;
                if (uEditModeSelected) {
                    vec3 selectionColor = uSceneLinearOutput
                        ? srgbToDisplayLinear(uEditSelectionColor)
                        : uEditSelectionColor;
                    color = mix(color, selectionColor,
                                uEditSelectionMix);
                }
                float signedViewDepth = dot(vWorldPosition - uCameraPosition,
                                            uViewFacing);
                // Perspective linear_zdepth is signed eye-space z. In
                // orthographic projection Blender returns depth from the near
                // plane, whose eye is offset from the view target.
                float viewDepth = uPerspective
                    ? abs(signedViewDepth)
                    : max(uOrthographicTransparencyDepthOffset -
                              signedViewDepth,
                          0.0);
                float depthRatio = viewDepth / 5.0;
                float farRatio = viewDepth / 200.0;
                farRatio *= farRatio;
                float weight = clamp(
                    10.0 / ((1.0e-5 + depthRatio * depthRatio) +
                            farRatio * (farRatio * farRatio)),
                    1.0e-2, 3.0e2);
                fragmentColor = vec4(color * (alpha * weight), alpha);
                transparencyWeight = vec4(alpha * weight, 0.0, 0.0, 1.0);
            } else {
                if (uSceneLinearOutput) {
                    if (uEditModeSelected) {
                        color = mix(color,
                                    srgbToDisplayLinear(uEditSelectionColor),
                                    uEditSelectionMix);
                    }
                } else {
                    color = sceneLinearToAgxSrgb(color);
                    // Blender composites Edit Mode overlays after the view
                    // transform, in linear display space.
                    if (uEditModeSelected) {
                        vec3 displayLinear = srgbToDisplayLinear(color);
                        color = sceneLinearToSrgb(
                            mix(displayLinear, uEditSelectionColor,
                                uEditSelectionMix));
                    }
                }
                fragmentColor = vec4(color, uBaseColor.a);
                transparencyWeight = vec4(0.0);
            }
        }
    )glsl";

    static constexpr const char *transparencyResolveVertexShader = R"glsl(
        #version 330 core
        out vec2 vUv;
        void main()
        {
            vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
            vUv = p * 0.5;
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        }
    )glsl";
    static constexpr const char *transparencyResolveFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uAccumulation;
        uniform sampler2D uWeight;
        uniform sampler3D uAgxDisplayTexture;
        uniform bool uAgxDisplayAvailable;
        uniform int uAgxDisplayLutSize;
        uniform vec2 uAgxDisplayLutDomain;
        uniform int uAgxDisplayLutShaper;
        uniform float uAgxDisplayLutShaperScale;
        uniform int uColorTransform;
        uniform float uColorExposure;
        uniform float uColorGamma;
        uniform bool uSceneLinearOutput;
        out vec4 fragmentColor;
        vec3 sceneLinearToSrgb(vec3 color)
        {
            color = max(color, vec3(0.0));
            vec3 low = color * 12.92;
            vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
            return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
        }
        vec3 workbenchDisplayLutCoordinate(vec3 color)
        {
            vec3 bounded = clamp(color, vec3(uAgxDisplayLutDomain.x),
                                 vec3(uAgxDisplayLutDomain.y)) -
                           vec3(uAgxDisplayLutDomain.x);
            float span = max(uAgxDisplayLutDomain.y - uAgxDisplayLutDomain.x,
                             1.0e-20);
            vec3 normalized = bounded / span;
            if (uAgxDisplayLutShaper == 1) {
                float denominator = log2(1.0 + uAgxDisplayLutShaperScale * span);
                normalized = log2(vec3(1.0) +
                                  uAgxDisplayLutShaperScale * bounded) /
                             max(denominator, 1.0e-20);
            }
            return (clamp(normalized, vec3(0.0), vec3(1.0)) *
                    float(uAgxDisplayLutSize - 1) + 0.5) /
                   float(uAgxDisplayLutSize);
        }
        void main()
        {
            vec4 accumulation = texture(uAccumulation, vUv);
            float weight = texture(uWeight, vUv).r;
            // Blender 5.2.2 clamps its resolved weight to this range in
            // workbench_transparent_resolve_frag.glsl.
            float resolvedWeight = clamp(weight, 1.0e-4, 5.0e4);
            // Blender resolves the signed weighted sum directly. Keep this
            // scene-linear value intact here; display transforms can apply
            // their own gamut/domain handling below.
            vec3 color = accumulation.rgb / resolvedWeight;
            if (uSceneLinearOutput) {
                fragmentColor = vec4(color, 1.0 - accumulation.a);
                return;
            }
            color *= exp2(uColorExposure);
            vec3 displayColor;
            if (uColorTransform == 1) {
                displayColor = sceneLinearToSrgb(color);
            } else if (uColorTransform == 4 && !uAgxDisplayAvailable) {
                displayColor = color;
            } else if (!uAgxDisplayAvailable) {
                displayColor = sceneLinearToSrgb(color);
            } else {
                vec3 coordinate = workbenchDisplayLutCoordinate(color);
                displayColor = texture(uAgxDisplayTexture, coordinate).rgb;
            }
            displayColor = pow(max(displayColor, vec3(0.0)),
                              vec3(1.0 / max(uColorGamma, 0.000001)));
            fragmentColor = vec4(displayColor, 1.0 - accumulation.a);
        }
    )glsl";

    static constexpr const char *objectIdVertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec3 aNormal;
        layout(location = 3) in vec2 aUv;
        uniform mat4 uViewProjection;
        uniform vec3 uWorldOffset;
        uniform vec3 uViewRight;
        uniform vec3 uViewUp;
        uniform vec3 uViewFacing;
        out vec3 vNormalView;
        out vec2 vUv;
        void main()
        {
            vNormalView = vec3(dot(aNormal, uViewRight),
                               dot(aNormal, uViewUp),
                               dot(aNormal, uViewFacing));
            vUv = aUv;
            gl_Position = uViewProjection *
                          vec4(aPosition + uWorldOffset, 1.0);
        }
    )glsl";
    static constexpr const char *frontMergeVertexShader = R"glsl(
        #version 330 core
        out vec2 vUv;
        void main()
        {
            vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
            vUv = p * 0.5;
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        }
    )glsl";
    static constexpr const char *frontMergeFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uFrontColor;
        uniform sampler2D uFrontDepth;
        uniform sampler2D uFrontNormal;
        uniform usampler2D uFrontObjectId;
        uniform usampler2D uFrontOutlineId;
        uniform bool uMergeColor;
        layout(location = 0) out vec4 mergedColor;
        layout(location = 1) out vec4 mergedNormal;
        layout(location = 2) out uint mergedObjectId;
        layout(location = 3) out uint mergedOutlineId;
        void main()
        {
            float depth = texture(uFrontDepth, vUv).r;
            if (depth >= 1.0) discard;
            gl_FragDepth = depth;
            if (uMergeColor) mergedColor = texture(uFrontColor, vUv);
            mergedNormal = texture(uFrontNormal, vUv);
            mergedObjectId = texture(uFrontObjectId, vUv).r;
            mergedOutlineId = texture(uFrontOutlineId, vUv).r;
        }
    )glsl";
    static constexpr const char *objectIdFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform uint uObjectId;
        uniform bool uUseTextureAlphaMask;
        uniform bool uTextureAlphaCutoff;
        uniform sampler2D uWorkbenchImage;
        uniform bool uUseUdimImage;
        uniform sampler2DArray uWorkbenchUdimImage;
        uniform sampler2D uWorkbenchUdimLookup;
        uniform int uTextureInterpolation;
        uniform int uTextureExtension;
        layout(location = 0) out uint fragmentObjectId;
        ivec2 mapUdimAlphaCoord(ivec2 p, ivec2 size, out bool valid)
        {
            valid = true;
            for (int axis = 0; axis < 2; ++axis) {
                int n = size[axis]; int c = p[axis];
                if (uTextureExtension == 0) c = (c % n + n) % n;
                else if (uTextureExtension == 1) c = clamp(c, 0, n - 1);
                else if (uTextureExtension == 2) { if (c < 0 || c >= n) valid = false; }
                else { c = (c % (2 * n) + 2 * n) % (2 * n); if (c >= n) c = 2 * n - 1 - c; }
                p[axis] = c;
            }
            return p;
        }
        vec4 fetchWorkbenchUdimAlpha(sampler2DArray image, vec4 info, ivec2 p)
        {
            ivec2 size = ivec2(round(vec2(textureSize(image, 0).xy) * info.yz)); bool valid;
            ivec2 mapped = mapUdimAlphaCoord(p, size, valid);
            return valid ? texelFetch(image, ivec3(mapped, int(info.x + 0.5) - 1), 0)
                         : vec4(0.0);
        }
        vec4 sampleWorkbenchUdimAlphaTexture(sampler2DArray image,
                                             sampler2D lookup, vec2 uv)
        {
            vec2 tile = floor(uv);
            if (tile.x < 0.0 || tile.x >= 10.0 || tile.y < 0.0 || tile.y >= 100.0)
                return vec4(1.0, 0.0, 1.0, 1.0);
            vec4 info = texelFetch(lookup, ivec2(tile), 0);
            if (info.x < 0.5) return vec4(1.0, 0.0, 1.0, 1.0);
            ivec2 size = ivec2(round(vec2(textureSize(image, 0).xy) * info.yz));
            vec2 p = fract(uv) * vec2(size) - 0.5;
            ivec2 base = ivec2(floor(p)); vec2 f = fract(p);
            if (uTextureInterpolation == 1)
                return fetchWorkbenchUdimAlpha(image, info, ivec2(floor(fract(uv) * vec2(size))));
            if (uTextureInterpolation != 2) {
                vec4 a = mix(fetchWorkbenchUdimAlpha(image, info, base), fetchWorkbenchUdimAlpha(image, info, base + ivec2(1,0)), f.x);
                vec4 b = mix(fetchWorkbenchUdimAlpha(image, info, base + ivec2(0,1)), fetchWorkbenchUdimAlpha(image, info, base + ivec2(1,1)), f.x);
                return mix(a, b, f.y);
            }
            vec2 w0=(1.0-f)*(1.0-f)*(1.0-f)/6.0;
            vec2 w1=(3.0*f*f*f-6.0*f*f+4.0)/6.0;
            vec2 w2=(-3.0*f*f*f+3.0*f*f+3.0*f+1.0)/6.0;
            vec2 w3=f*f*f/6.0; vec4 result=vec4(0.0);
            for (int y=0;y<4;++y) { float wy=y==0?w0.y:y==1?w1.y:y==2?w2.y:w3.y;
                for (int x=0;x<4;++x) { float wx=x==0?w0.x:x==1?w1.x:x==2?w2.x:w3.x;
                    result += fetchWorkbenchUdimAlpha(image,info,base+ivec2(x-1,y-1))*wx*wy; } }
            return result;
        }
        vec4 sampleWorkbenchUdimAlphaLinear(vec2 uv)
        {
            return sampleWorkbenchUdimAlphaTexture(uWorkbenchUdimImage,
                                                    uWorkbenchUdimLookup, uv);
        }
        vec4 sampleWorkbenchUdimAlpha(vec2 uv)
        {
            return sampleWorkbenchUdimAlphaTexture(uWorkbenchUdimImage,
                                                    uWorkbenchUdimLookup, uv);
        }
        vec4 sampleWorkbenchAlphaImage(vec2 uv)
        {
            if (uUseUdimImage) return sampleWorkbenchUdimAlpha(uv);
            if (uTextureInterpolation != 2) return texture(uWorkbenchImage, uv);
            vec2 size = vec2(textureSize(uWorkbenchImage, 0));
            vec2 p = uv * size - 0.5;
            vec2 base = floor(p);
            vec2 f = fract(p);
            vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
            vec2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
            vec2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
            vec2 w3 = f * f * f / 6.0;
            vec4 result = vec4(0.0);
            for (int y = 0; y < 4; ++y) {
                float wy = y == 0 ? w0.y : y == 1 ? w1.y : y == 2 ? w2.y : w3.y;
                for (int x = 0; x < 4; ++x) {
                    float wx = x == 0 ? w0.x : x == 1 ? w1.x : x == 2 ? w2.x : w3.x;
                    result += texture(uWorkbenchImage,
                        (base + vec2(float(x - 1), float(y - 1)) + 0.5) / size) * wx * wy;
                }
            }
            return result;
        }
        void main()
        {
            if (uUseTextureAlphaMask && uTextureAlphaCutoff &&
                sampleWorkbenchAlphaImage(vUv).a < 0.1) discard;
            fragmentObjectId = uObjectId;
        }
    )glsl";
    static constexpr const char *workbenchEffectFragmentShader = R"glsl(
        #version 330 core
        in vec3 vNormalView;
        in vec2 vUv;
        uniform uint uObjectId;
        uniform uint uOutlineObjectId;
        uniform bool uUseTextureAlphaMask;
        uniform bool uTextureAlphaCutoff;
        uniform bool uExcludeFrontCoverage;
        uniform sampler2D uFrontDepthMask;
        uniform sampler2D uWorkbenchImage;
        uniform bool uUseUdimImage;
        uniform sampler2DArray uWorkbenchUdimImage;
        uniform sampler2D uWorkbenchUdimLookup;
        uniform int uTextureInterpolation;
        uniform int uTextureExtension;
        layout(location = 0) out vec4 fragmentViewNormal;
        layout(location = 1) out uint fragmentObjectId;
        layout(location = 2) out uint fragmentOutlineObjectId;
        ivec2 mapUdimAlphaCoord(ivec2 p, ivec2 size, out bool valid)
        {
            valid=true;
            for(int axis=0;axis<2;++axis){int n=size[axis];int c=p[axis];
                if(uTextureExtension==0)c=(c%n+n)%n;
                else if(uTextureExtension==1)c=clamp(c,0,n-1);
                else if(uTextureExtension==2){if(c<0||c>=n)valid=false;}
                else{c=(c%(2*n)+2*n)%(2*n);if(c>=n)c=2*n-1-c;}p[axis]=c;}
            return p;
        }
        vec4 fetchWorkbenchUdimAlpha(sampler2DArray image,vec4 info,ivec2 p)
        {ivec2 size=ivec2(round(vec2(textureSize(image,0).xy)*info.yz));bool valid;ivec2 mapped=mapUdimAlphaCoord(p,size,valid);
         return valid?texelFetch(image,ivec3(mapped,int(info.x+0.5)-1),0):vec4(0.0);}
        vec4 sampleWorkbenchUdimAlphaTexture(sampler2DArray image,sampler2D lookup,vec2 uv)
        {
            vec2 tile=floor(uv);
            if(tile.x<0.0||tile.x>=10.0||tile.y<0.0||tile.y>=100.0)return vec4(1.0,0.0,1.0,1.0);
            vec4 info=texelFetch(lookup,ivec2(tile),0);if(info.x<0.5)return vec4(1.0,0.0,1.0,1.0);
            ivec2 size=ivec2(round(vec2(textureSize(image,0).xy)*info.yz));vec2 p=fract(uv)*vec2(size)-0.5;ivec2 base=ivec2(floor(p));vec2 f=fract(p);
            if(uTextureInterpolation==1)return fetchWorkbenchUdimAlpha(image,info,ivec2(floor(fract(uv)*vec2(size))));
            if(uTextureInterpolation!=2){vec4 a=mix(fetchWorkbenchUdimAlpha(image,info,base),fetchWorkbenchUdimAlpha(image,info,base+ivec2(1,0)),f.x);
                vec4 b=mix(fetchWorkbenchUdimAlpha(image,info,base+ivec2(0,1)),fetchWorkbenchUdimAlpha(image,info,base+ivec2(1,1)),f.x);return mix(a,b,f.y);}
            vec2 w0=(1.0-f)*(1.0-f)*(1.0-f)/6.0;vec2 w1=(3.0*f*f*f-6.0*f*f+4.0)/6.0;
            vec2 w2=(-3.0*f*f*f+3.0*f*f+3.0*f+1.0)/6.0;vec2 w3=f*f*f/6.0;vec4 result=vec4(0.0);
            for(int y=0;y<4;++y){float wy=y==0?w0.y:y==1?w1.y:y==2?w2.y:w3.y;for(int x=0;x<4;++x){float wx=x==0?w0.x:x==1?w1.x:x==2?w2.x:w3.x;
                result+=fetchWorkbenchUdimAlpha(image,info,base+ivec2(x-1,y-1))*wx*wy;}}
            return result;
        }
        vec4 sampleWorkbenchUdimAlphaLinear(vec2 uv)
        {
            return sampleWorkbenchUdimAlphaTexture(uWorkbenchUdimImage,
                                                    uWorkbenchUdimLookup, uv);
        }
        vec4 sampleWorkbenchUdimAlpha(vec2 uv)
        {
            return sampleWorkbenchUdimAlphaTexture(uWorkbenchUdimImage,
                                                    uWorkbenchUdimLookup, uv);
        }
        vec4 sampleWorkbenchAlphaImage(vec2 uv)
        {
            if (uUseUdimImage) return sampleWorkbenchUdimAlpha(uv);
            if (uTextureInterpolation != 2) return texture(uWorkbenchImage, uv);
            vec2 size = vec2(textureSize(uWorkbenchImage, 0));
            vec2 p = uv * size - 0.5;
            vec2 base = floor(p);
            vec2 f = fract(p);
            vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
            vec2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
            vec2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
            vec2 w3 = f * f * f / 6.0;
            vec4 result = vec4(0.0);
            for (int y = 0; y < 4; ++y) {
                float wy = y == 0 ? w0.y : y == 1 ? w1.y : y == 2 ? w2.y : w3.y;
                for (int x = 0; x < 4; ++x) {
                    float wx = x == 0 ? w0.x : x == 1 ? w1.x : x == 2 ? w2.x : w3.x;
                    result += texture(uWorkbenchImage,
                        (base + vec2(float(x - 1), float(y - 1)) + 0.5) / size) * wx * wy;
                }
            }
            return result;
        }
        void main()
        {
            if (uExcludeFrontCoverage &&
                texelFetch(uFrontDepthMask, ivec2(gl_FragCoord.xy), 0).r < 1.0) {
                discard;
            }
            if (uUseTextureAlphaMask && uTextureAlphaCutoff &&
                sampleWorkbenchAlphaImage(vUv).a < 0.1) {
                discard;
            }
            fragmentViewNormal = vec4(normalize(vNormalView), 1.0);
            fragmentObjectId = uObjectId;
            fragmentOutlineObjectId = uOutlineObjectId;
        }
    )glsl";
    static constexpr const char *outlineVertexShader = R"glsl(
        #version 330 core
        void main()
        {
            vec2 positions[3] = vec2[3](vec2(-1.0, -1.0),
                                         vec2(3.0, -1.0),
                                         vec2(-1.0, 3.0));
            gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
        }
    )glsl";
    static constexpr const char *outlineFragmentShader = R"glsl(
        #version 330 core
        uniform usampler2D uObjectIdBuffer;
        uniform vec4 uOutlineColor;
        uniform vec4 uSelectionColor;
        uniform float uUiScale;
        layout(location = 0) out vec4 fragmentColor;
        void main()
        {
            ivec2 size = textureSize(uObjectIdBuffer, 0);
            ivec2 pixel = ivec2(gl_FragCoord.xy);
            uint centerId = texelFetch(uObjectIdBuffer, pixel, 0).r;
            vec2 inverseSize = 1.0 / vec2(size);
            vec2 uv = (vec2(pixel) + vec2(0.5)) * inverseSize;
            vec2 dx = vec2(inverseSize.x * uUiScale, 0.0);
            vec2 dy = vec2(0.0, inverseSize.y * uUiScale);
            uvec4 cardinalIds = uvec4(
                texture(uObjectIdBuffer, uv + dx).r,
                texture(uObjectIdBuffer, uv - dx).r,
                texture(uObjectIdBuffer, uv + dy).r,
                texture(uObjectIdBuffer, uv - dy).r);
            // Blender Workbench derives silhouette coverage from the center
            // ID and its four cardinal neighbors. Apply selection color over
            // that same neighborhood so diagonal-only contact does not color
            // this pixel as selected.
            float opacity = 1.0 -
                dot(vec4(equal(uvec4(centerId), cardinalIds)), vec4(0.25));
            if (opacity <= 0.0) {
                discard;
            }
            const uint selectedBit = 0x80000000u;
            bool selected = (centerId & selectedBit) != 0u;
            for (int index = 0; index < 4; ++index) {
                selected = selected || ((cardinalIds[index] & selectedBit) != 0u);
            }
            vec4 color = selected ? uSelectionColor : uOutlineColor;
            fragmentColor = vec4(color.rgb, color.a * opacity);
        }
    )glsl";
    static constexpr const char *shadowVertexShader = R"glsl(
        #version 330 core
        layout(location = 0) in vec3 aPosition;
        layout(location = 3) in vec2 aUv;
        uniform mat4 uShadowViewProjection;
        uniform vec3 uWorldOffset;
        out vec2 vUv;
        void main()
        {
            vUv = aUv;
            gl_Position = uShadowViewProjection *
                          vec4(aPosition + uWorldOffset, 1.0);
        }
    )glsl";
    static constexpr const char *shadowFragmentShader = R"glsl(
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uShadowAlphaTexture;
        uniform bool uUseShadowUdimImage;
        uniform sampler2DArray uShadowUdimImage;
        uniform sampler2D uShadowUdimLookup;
        uniform bool uUseShadowAlphaMask;
        uniform int uTextureInterpolation;
        uniform int uTextureExtension;
        ivec2 mapShadowUdimCoord(ivec2 p, ivec2 size, out bool valid)
        {
            valid=true;
            for(int axis=0;axis<2;++axis){int n=size[axis];int c=p[axis];
                if(uTextureExtension==0)c=(c%n+n)%n;
                else if(uTextureExtension==1)c=clamp(c,0,n-1);
                else if(uTextureExtension==2){if(c<0||c>=n)valid=false;}
                else{c=(c%(2*n)+2*n)%(2*n);if(c>=n)c=2*n-1-c;}p[axis]=c;}
            return p;
        }
        vec4 fetchShadowUdimAlpha(sampler2DArray image,vec4 info,ivec2 p)
        {ivec2 size=ivec2(round(vec2(textureSize(image,0).xy)*info.yz));bool valid;ivec2 mapped=mapShadowUdimCoord(p,size,valid);
         return valid?texelFetch(image,ivec3(mapped,int(info.x+0.5)-1),0):vec4(0.0);}
        vec4 sampleShadowUdimTexture(sampler2DArray image,sampler2D lookup,vec2 uv)
        {
            vec2 tile=floor(uv);
            if(tile.x<0.0||tile.x>=10.0||tile.y<0.0||tile.y>=100.0)return vec4(1.0,0.0,1.0,1.0);
            vec4 info=texelFetch(lookup,ivec2(tile),0);if(info.x<0.5)return vec4(1.0,0.0,1.0,1.0);
            ivec2 size=ivec2(round(vec2(textureSize(image,0).xy)*info.yz));vec2 p=fract(uv)*vec2(size)-0.5;ivec2 base=ivec2(floor(p));vec2 f=fract(p);
            if(uTextureInterpolation==1)return fetchShadowUdimAlpha(image,info,ivec2(floor(fract(uv)*vec2(size))));
            if(uTextureInterpolation!=2){vec4 a=mix(fetchShadowUdimAlpha(image,info,base),fetchShadowUdimAlpha(image,info,base+ivec2(1,0)),f.x);
                vec4 b=mix(fetchShadowUdimAlpha(image,info,base+ivec2(0,1)),fetchShadowUdimAlpha(image,info,base+ivec2(1,1)),f.x);return mix(a,b,f.y);}
            vec2 w0=(1.0-f)*(1.0-f)*(1.0-f)/6.0;vec2 w1=(3.0*f*f*f-6.0*f*f+4.0)/6.0;
            vec2 w2=(-3.0*f*f*f+3.0*f*f+3.0*f+1.0)/6.0;vec2 w3=f*f*f/6.0;vec4 result=vec4(0.0);
            for(int y=0;y<4;++y){float wy=y==0?w0.y:y==1?w1.y:y==2?w2.y:w3.y;for(int x=0;x<4;++x){float wx=x==0?w0.x:x==1?w1.x:x==2?w2.x:w3.x;
                result+=fetchShadowUdimAlpha(image,info,base+ivec2(x-1,y-1))*wx*wy;}}
            return result;
        }
        vec4 sampleShadowUdimAlphaLinear(vec2 uv)
        {
            return sampleShadowUdimTexture(uShadowUdimImage,
                                            uShadowUdimLookup, uv);
        }
        vec4 sampleShadowUdimAlpha(vec2 uv)
        {
            return sampleShadowUdimTexture(uShadowUdimImage,
                                            uShadowUdimLookup, uv);
        }
        vec4 sampleShadowAlphaImage(vec2 uv)
        {
            if (uUseShadowUdimImage) return sampleShadowUdimAlpha(uv);
            if (uTextureInterpolation != 2) return texture(uShadowAlphaTexture, uv);
            vec2 size = vec2(textureSize(uShadowAlphaTexture, 0));
            vec2 p = uv * size - 0.5;
            vec2 base = floor(p);
            vec2 f = fract(p);
            vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
            vec2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
            vec2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
            vec2 w3 = f * f * f / 6.0;
            vec4 result = vec4(0.0);
            for (int y = 0; y < 4; ++y) {
                float wy = y == 0 ? w0.y : y == 1 ? w1.y : y == 2 ? w2.y : w3.y;
                for (int x = 0; x < 4; ++x) {
                    float wx = x == 0 ? w0.x : x == 1 ? w1.x : x == 2 ? w2.x : w3.x;
                    result += texture(uShadowAlphaTexture,
                        (base + vec2(float(x - 1), float(y - 1)) + 0.5) / size) * wx * wy;
                }
            }
            return result;
        }
        void main()
        {
            if (uUseShadowAlphaMask &&
                sampleShadowAlphaImage(vUv).a < 0.1) {
                discard;
            }
        }
    )glsl";

    if (QOpenGLContext::currentContext() == nullptr ||
        !initializeOpenGLFunctions() ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
        !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader) ||
        !program_.link() || !vertexArray_.create() || !vertexBuffer_.create() ||
        !shadowVolumeVertexArray_.create() ||
        !shadowVolumeVertexBuffer_.create() ||
        !objectIdProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                  objectIdVertexShader) ||
        !objectIdProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                  objectIdFragmentShader) ||
        !objectIdProgram_.link() ||
        !workbenchEffectProgram_.addShaderFromSourceCode(
            QOpenGLShader::Vertex, objectIdVertexShader) ||
        !workbenchEffectProgram_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, workbenchEffectFragmentShader) ||
        !workbenchEffectProgram_.link() ||
        !outlineProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                 outlineVertexShader) ||
        !outlineProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                 outlineFragmentShader) ||
        !outlineProgram_.link() || !outlineVertexArray_.create() ||
        !shadowProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                shadowVertexShader) ||
        !shadowProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                shadowFragmentShader) ||
        !shadowProgram_.link() ||
        !transparencyResolveProgram_.addShaderFromSourceCode(
            QOpenGLShader::Vertex, transparencyResolveVertexShader) ||
        !transparencyResolveProgram_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, transparencyResolveFragmentShader) ||
        !transparencyResolveProgram_.link() ||
        !frontMergeProgram_.addShaderFromSourceCode(
            QOpenGLShader::Vertex, frontMergeVertexShader) ||
        !frontMergeProgram_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, frontMergeFragmentShader) ||
        !frontMergeProgram_.link()) {
        qWarning().noquote()
            << "Viewport shader setup failed. Surface:" << program_.log()
            << "Object ID:" << objectIdProgram_.log()
            << "Workbench effects:" << workbenchEffectProgram_.log()
            << "Outline:" << outlineProgram_.log()
            << "Shadow:" << shadowProgram_.log()
            << "Transparency resolve:" << transparencyResolveProgram_.log();
        return false;
    }

    vertexArray_.bind();
    vertexBuffer_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, attributeR)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                          reinterpret_cast<const void *>(offsetof(SurfaceVertex, u)));
    vertexBuffer_.release();
    vertexArray_.release();

    shadowVolumeVertexArray_.bind();
    shadowVolumeVertexBuffer_.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(QVector3D), nullptr);
    shadowVolumeVertexBuffer_.release();
    shadowVolumeVertexArray_.release();

    initialized_ = true;
    return true;
}

void ViewportSurfaceRenderer::prepareGeometry(
    const QVector<ViewportRenderObject> &objects)
{
    QByteArray key;
    QDataStream keyStream(&key, QIODevice::WriteOnly);
    keyStream << qint32(objects.size());
    for (int objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ViewportRenderObject &object = objects[objectIndex];
        const bool hasSurface = hasNurbsSurfaceGeometry(object);
        const bool hasMesh = hasPolygonMeshGeometry(object);
        const ViewportDepthGeometry *geometry =
            hasSurface ? object.preparedDepthGeometry.data() : nullptr;
        keyStream << quint64(object.objectId.value())
                  << quint64(object.geometryRevision)
                  << quint8(object.showInFront ? 1 : 0)
                  << quint8(hasSurface ? 1 : (hasMesh ? 2 : 0))
                  << quint64(reinterpret_cast<quintptr>(geometry))
                  << qint32(geometry != nullptr
                                ? geometry->surfaceVertices.size()
                                : 0);
        if (object.shape.geometryType == GeometryType::NurbsSurface) {
            keyStream << object.workbenchMaterial.textureImagePath
                      << QCryptographicHash::hash(
                             object.workbenchMaterial.textureImagePackedDataBase64.toLatin1(),
                             QCryptographicHash::Sha256)
                      << object.workbenchMaterial.textureImageSrgb
                      << object.workbenchMaterial.textureImageAlphaMode
                      << object.workbenchMaterial.textureImageInterpolation
                      << object.workbenchMaterial.textureImageExtension;
            for (const DocumentImageTextureTile &tile :
                 object.workbenchMaterial.textureImageTiles) {
                keyStream << qint32(tile.number) << tile.filepath
                          << tile.sourceFileExists;
            }
        }
        if (hasMesh) {
            const PolygonMesh3D &mesh = object.shape.polygonMesh;
            keyStream << qint32(mesh.vertices.size()) << qint32(mesh.faces.size());
            for (const Point3D &point : mesh.vertices) {
                keyStream << double(point.x) << double(point.y) << double(point.z);
            }
            for (const QVector<int> &face : mesh.faces) {
                keyStream << qint32(face.size());
                for (int index : face) keyStream << qint32(index);
            }
            for (const Point3D &normal : mesh.cornerNormals)
                keyStream << double(normal.x) << double(normal.y) << double(normal.z);
            keyStream << mesh.activeColorAttributeName;
            keyStream << mesh.activeUvMapName;
            for (const MeshUvMap &uvMap : mesh.uvMaps) {
                if (uvMap.name != mesh.activeUvMapName) continue;
                for (const QPointF &uv : uvMap.cornerValues)
                    keyStream << double(uv.x()) << double(uv.y());
            }
            for (const WorkbenchMaterialData &material : object.meshMaterials) {
                keyStream << material.textureImagePath
                          << QCryptographicHash::hash(
                                 material.textureImagePackedDataBase64.toLatin1(),
                                 QCryptographicHash::Sha256)
                          << material.textureImageSrgb
                          << material.textureImageAlphaMode
                          << material.textureImageInterpolation
                          << material.textureImageExtension;
                for (const DocumentImageTextureTile &tile : material.textureImageTiles) {
                    keyStream << qint32(tile.number) << tile.filepath
                              << tile.sourceFileExists;
                }
            }
            for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
                if (attribute.name != mesh.activeColorAttributeName) continue;
                keyStream << attribute.name << qint32(attribute.domain)
                          << attribute.dataType << attribute.colorSpace
                          << qint32(attribute.componentCount)
                          << qint32(attribute.values.size());
                for (float value : attribute.values) keyStream << value;
            }
            for (double value : object.meshObjectTransform) keyStream << value;
        }
    }
    const bool changed = key != geometryKey_;
    ranges_.clear();
    ranges_.reserve(objects.size());
    if (changed) {
        geometryKey_ = std::move(key);
        geometryDirty_ = true;
        vertices_.clear();
    }
    for (int objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ViewportRenderObject &object = objects[objectIndex];
        const ViewportDepthGeometry *geometry =
            hasNurbsSurfaceGeometry(object)
                ? object.preparedDepthGeometry.data()
                : nullptr;
        const auto makeRange = [&object, objectIndex](int first, int count) {
            DrawRange range;
            range.first = first;
            range.count = count;
            range.offset = object.preparedGeometryOffset;
            range.selected = object.selected;
            range.showInFront = object.showInFront;
            range.displayColorSceneLinear = object.displayColorSceneLinear;
            range.workbenchMaterial = object.workbenchMaterial;
            range.objectSeed = object.objectId.value();
            range.effectObjectId = static_cast<quint32>(objectIndex + 1) &
                                   0x7fffffffu;
            range.displayName = object.displayName;
            range.libraryPath = object.libraryPath;
            range.outlineHandledBySceneStroke =
                object.outlineHandledBySceneStroke;
            return range;
        };
        if (hasPolygonMeshGeometry(object)) {
            const PolygonMesh3D &mesh = object.shape.polygonMesh;
            const int meshFirstRange = ranges_.size();
            QVector<QVector3D> meshShadowVertices;
            QVector<std::array<int, 3>> meshShadowTriangles;
            const bool meshHasClosedShadowTopology =
                buildClosedPolygonShadowMesh(mesh, object.meshObjectTransform,
                                             &meshShadowVertices,
                                             &meshShadowTriangles);
            const MeshColorAttribute *activeAttribute = nullptr;
            for (const MeshColorAttribute &attribute : mesh.colorAttributes) {
                if (attribute.name == mesh.activeColorAttributeName) {
                    activeAttribute = &attribute;
                    break;
                }
            }
            int firstCorner = 0;
            const auto &matrix = object.meshObjectTransform;
            const qreal linearDeterminant =
                matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
                matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
                matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
            const bool reflected = std::isfinite(linearDeterminant) &&
                                   linearDeterminant < -1.0e-12;
            const MeshUvMap *activeUvMap = nullptr;
            for (const MeshUvMap &uvMap : mesh.uvMaps) {
                if (uvMap.name == mesh.activeUvMapName) {
                    activeUvMap = &uvMap;
                    break;
                }
            }
            for (int faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
                const QVector<int> &face = mesh.faces[faceIndex];
                QVector<Point3D> localLoop;
                QVector<Point3D> worldLoop;
                localLoop.reserve(face.size());
                worldLoop.reserve(face.size());
                for (int index : face) {
                    const Point3D &p = mesh.vertices[index];
                    localLoop.append(p);
                    const qreal w = matrix[12] * p.x + matrix[13] * p.y +
                                    matrix[14] * p.z + matrix[15];
                    const qreal divisor = std::abs(w) > 1.0e-12 ? w : 1.0;
                    worldLoop.append({(matrix[0] * p.x + matrix[1] * p.y +
                                       matrix[2] * p.z + matrix[3]) / divisor,
                                      (matrix[4] * p.x + matrix[5] * p.y +
                                       matrix[6] * p.z + matrix[7]) / divisor,
                                      (matrix[8] * p.x + matrix[9] * p.y +
                                       matrix[10] * p.z + matrix[11]) / divisor});
                }
                const auto triangles = triangulatePolygonLoop(localLoop);
                const int faceFirstCorner = firstCorner;
                firstCorner += face.size();
                const int faceSlot = mesh.faceMaterialIndices.value(faceIndex, -1);
                const QString imagePath = faceSlot >= 0 &&
                        faceSlot < object.meshMaterials.size()
                    ? object.meshMaterials[faceSlot].textureImagePath : QString{};
                const bool packedImageAvailable = faceSlot >= 0 &&
                        faceSlot < object.meshMaterials.size() &&
                    !object.meshMaterials[faceSlot].textureImagePackedDataBase64.isEmpty();
                const bool tiledImageAvailable = faceSlot >= 0 &&
                        faceSlot < object.meshMaterials.size() &&
                    !object.meshMaterials[faceSlot].textureImageTiles.isEmpty();
                const bool textureAvailable = (!imagePath.isEmpty() || packedImageAvailable ||
                                               tiledImageAvailable) &&
                    activeUvMap != nullptr &&
                    faceFirstCorner + face.size() <= activeUvMap->cornerValues.size();
                const int first = changed
                    ? vertices_.size()
                    : (ranges_.isEmpty() ? 0
                                         : ranges_.last().first + ranges_.last().count);
                int count = 0;
                for (const auto &triangle : triangles) {
                    const int indexA = triangle[0];
                    const int indexB = reflected ? triangle[2] : triangle[1];
                    const int indexC = reflected ? triangle[1] : triangle[2];
                    const Point3D &a = worldLoop[indexA];
                    const Point3D &b = worldLoop[indexB];
                    const Point3D &c = worldLoop[indexC];
                    QVector3D normal(
                        static_cast<float>((b.y - a.y) * (c.z - a.z) -
                                           (b.z - a.z) * (c.y - a.y)),
                        static_cast<float>((b.z - a.z) * (c.x - a.x) -
                                           (b.x - a.x) * (c.z - a.z)),
                        static_cast<float>((b.x - a.x) * (c.y - a.y) -
                                           (b.y - a.y) * (c.x - a.x)));
                    if (normal.lengthSquared() <= 1.0e-16f) continue;
                    normal.normalize();
                    QVector3D triangleColors[3] = {
                        QVector3D(1.0f, 1.0f, 1.0f),
                        QVector3D(1.0f, 1.0f, 1.0f),
                        QVector3D(1.0f, 1.0f, 1.0f)};
                    QPointF triangleUvs[3] = {};
                    if (textureAvailable) {
                        for (int corner = 0; corner < 3; ++corner)
                            triangleUvs[corner] = activeUvMap->cornerValues[
                                faceFirstCorner + triangle[corner]];
                    }
                    bool triangleHasAttribute = activeAttribute != nullptr &&
                        activeAttribute->componentCount >= 1 &&
                        activeAttribute->componentCount <= 4;
                    if (triangleHasAttribute) {
                        for (int corner = 0; corner < 3; ++corner) {
                            const int loopIndex = triangle[corner];
                            int elementIndex = faceIndex;
                            if (activeAttribute->domain == MeshAttributeDomain::Point) {
                                elementIndex = face[loopIndex];
                            } else if (activeAttribute->domain == MeshAttributeDomain::Corner) {
                                elementIndex = faceFirstCorner + loopIndex;
                            }
                            const int base = elementIndex * activeAttribute->componentCount;
                            if (base < 0 || base + activeAttribute->componentCount >
                                                activeAttribute->values.size()) {
                                triangleHasAttribute = false;
                                break;
                            }
                            float r = activeAttribute->values[base];
                            float g = activeAttribute->componentCount >= 3
                                          ? activeAttribute->values[base + 1] : r;
                            float b = activeAttribute->componentCount >= 3
                                          ? activeAttribute->values[base + 2] : r;
                            if (activeAttribute->colorSpace.contains(
                                    QStringLiteral("srgb"), Qt::CaseInsensitive)) {
                                const QVector3D linear = workbenchSrgbToSceneLinear(
                                    QVector3D(r, g, b));
                                r = linear.x(); g = linear.y(); b = linear.z();
                            }
                            triangleColors[corner] = QVector3D(r, g, b);
                        }
                    }
                    if (changed) {
                        const Point3D *trianglePoints[3] = {&a, &b, &c};
                        for (int corner = 0; corner < 3; ++corner) {
                            const Point3D *point = trianglePoints[corner];
                            const QVector3D &color = triangleColors[corner];
                            QVector3D vertexNormal = normal;
                            if (!mesh.cornerNormals.isEmpty()) {
                                const int loopIndex = triangle[corner];
                                vertexNormal = transformMeshNormal(
                                    matrix, mesh.cornerNormals[faceFirstCorner + loopIndex]);
                                if (vertexNormal.lengthSquared() <= 1.0e-16f)
                                    vertexNormal = normal;
                            }
                            vertices_.append({static_cast<float>(point->x),
                                              static_cast<float>(point->y),
                                              static_cast<float>(point->z),
                                              vertexNormal.x(), vertexNormal.y(), vertexNormal.z(),
                                              color.x(), color.y(), color.z(),
                                              static_cast<float>(triangleUvs[corner].x()),
                                              static_cast<float>(triangleUvs[corner].y())});
                        }
                    }
                    count += 3;
                }
                DrawRange range = makeRange(first, count);
                range.useAttributeColor = activeAttribute != nullptr &&
                    activeAttribute->componentCount >= 1 &&
                    activeAttribute->componentCount <= 4;
                range.useTextureColor = textureAvailable;
                range.shadowTextureAlphaMask = textureAvailable;
                range.textureImagePath = textureAvailable ? imagePath : QString{};
                const int slotIndex = mesh.faceMaterialIndices.value(faceIndex, -1);
                if (slotIndex >= 0 && slotIndex < object.meshMaterials.size()) {
                    range.workbenchMaterial = object.meshMaterials[slotIndex];
                } else if (slotIndex >= 0 && slotIndex < mesh.materialSlots.size()) {
                    const PolygonMeshMaterialSlot &slot = mesh.materialSlots[slotIndex];
                    if (slot.baseColorSceneLinear.size() >= 3) {
                        range.workbenchMaterial.baseColorSceneLinear = QVector3D(
                            slot.baseColorSceneLinear[0], slot.baseColorSceneLinear[1],
                            slot.baseColorSceneLinear[2]);
                    }
                    range.workbenchMaterial.roughness = slot.roughness;
                    range.workbenchMaterial.metallic = slot.metallic;
                    range.workbenchMaterial.alpha = slot.alpha;
                }
                range.textureImageSrgb = range.workbenchMaterial.textureImageSrgb;
                range.textureImageTiles = textureAvailable
                    ? range.workbenchMaterial.textureImageTiles
                    : QVector<DocumentImageTextureTile>{};
                ranges_.append(std::move(range));
            }
            if (meshHasClosedShadowTopology &&
                meshFirstRange < ranges_.size()) {
                // Mesh faces have separate material/color draw ranges. Mark
                // every face as belonging to this validated volume so the
                // fallback map does not draw the other faces a second time;
                // keep the shared indexed topology on the first range only.
                for (int rangeIndex = meshFirstRange;
                     rangeIndex < ranges_.size(); ++rangeIndex) {
                    DrawRange &range = ranges_[rangeIndex];
                    range.shadowVolumeSupported = true;
                    range.shadowVolumeClosed = true;
                    range.shadowVolumePolygonMesh = true;
                }
                ranges_[meshFirstRange].shadowVertices =
                    std::move(meshShadowVertices);
                ranges_[meshFirstRange].shadowTriangles =
                    std::move(meshShadowTriangles);
            }
        } else {
            const int first = changed ? vertices_.size() :
                (ranges_.isEmpty() ? 0 : ranges_.last().first + ranges_.last().count);
            const int count = geometry != nullptr
                                  ? static_cast<int>(std::min(
                                        geometry->surfaceVertices.size(),
                                        geometry->surfaceNormals.size())) : 0;
            if (changed) {
                for (int index = 0; index < count; ++index) {
                    const QVector3D &position = geometry->surfaceVertices[index];
                    const QVector3D &normal = geometry->surfaceNormals[index];
                    const QPointF uv = geometry->surfaceTextureCoordinates.size() == count
                        ? geometry->surfaceTextureCoordinates[index] : QPointF();
                    vertices_.append({position.x(), position.y(), position.z(),
                                      normal.x(), normal.y(), normal.z(),
                                      1.0f, 1.0f, 1.0f,
                                      static_cast<float>(uv.x()),
                                      static_cast<float>(uv.y())});
                }
            }
            DrawRange range = makeRange(first, count);
            const bool hasSurfaceUv =
                object.shape.geometryType == GeometryType::NurbsSurface &&
                geometry != nullptr &&
                geometry->surfaceTextureCoordinates.size() == count;
            const bool hasSurfaceTexture =
                !object.workbenchMaterial.textureImagePath.isEmpty() ||
                !object.workbenchMaterial.textureImagePackedDataBase64.isEmpty() ||
                !object.workbenchMaterial.textureImageTiles.isEmpty();
            range.useTextureColor = hasSurfaceUv && hasSurfaceTexture;
            // Tessellation carries active-domain UVs for each rendered vertex,
            // including vertices on trimmed regions, so the GPU color, object-ID,
            // and depth-map alpha masks use the same texture placement.
            range.shadowTextureAlphaMask = range.useTextureColor;
            range.textureImagePath = range.useTextureColor
                ? object.workbenchMaterial.textureImagePath : QString{};
            range.textureImageTiles = range.useTextureColor
                ? object.workbenchMaterial.textureImageTiles
                : QVector<DocumentImageTextureTile>{};
            range.textureImageSrgb = object.workbenchMaterial.textureImageSrgb;
            if ((object.shape.geometryType == GeometryType::NurbsSolid ||
                 object.shape.geometryType == GeometryType::NurbsSurface) &&
                geometry != nullptr && geometry->shadowCasterMesh.valid) {
                range.shadowVolumeSupported = true;
                range.shadowVolumeClosed =
                    object.shape.geometryType == GeometryType::NurbsSolid;
                range.shadowVertices = geometry->shadowCasterMesh.vertices;
                range.shadowTriangles = geometry->shadowCasterMesh.triangles;
            }
            ranges_.append(std::move(range));
        }
    }
}

bool ViewportSurfaceRenderer::ensureShadowMap()
{
    if (shadowTexture_ != 0 && shadowFramebuffer_ != 0) {
        return true;
    }
    GLint previousTexture = 0;
    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGenTextures(1, &shadowTexture_);
    glBindTexture(GL_TEXTURE_2D, shadowTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 kWorkbenchShadowMapSize, kWorkbenchShadowMapSize,
                 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE,
                    GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    const GLfloat borderColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glGenFramebuffers(1, &shadowFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, shadowTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                          GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(previousFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    if (!complete) {
        qWarning() << "Workbench shadow depth framebuffer is incomplete";
        glDeleteFramebuffers(1, &shadowFramebuffer_);
        glDeleteTextures(1, &shadowTexture_);
        shadowFramebuffer_ = 0;
        shadowTexture_ = 0;
    }
    return complete;
}

bool ViewportSurfaceRenderer::ensureObjectIdFramebuffer(
    const QSize &pixelSize)
{
    if (objectIdFramebufferAttempted_ &&
        objectIdFramebufferSize_ == pixelSize) {
        return objectIdFramebuffer_ != 0;
    }

    if (objectIdFramebuffer_ != 0) {
        glDeleteFramebuffers(1, &objectIdFramebuffer_);
        objectIdFramebuffer_ = 0;
    }
    if (objectIdDepthStencil_ != 0) {
        glDeleteRenderbuffers(1, &objectIdDepthStencil_);
        objectIdDepthStencil_ = 0;
    }
    if (objectIdTexture_ != 0) {
        glDeleteTextures(1, &objectIdTexture_);
        objectIdTexture_ = 0;
    }

    objectIdFramebufferAttempted_ = true;
    objectIdFramebufferSize_ = pixelSize;

    GLint previousFramebuffer = 0;
    GLint previousRenderbuffer = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    glGenTextures(1, &objectIdTexture_);
    glBindTexture(GL_TEXTURE_2D, objectIdTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                 pixelSize.width(), pixelSize.height(), 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &objectIdDepthStencil_);
    glBindRenderbuffer(GL_RENDERBUFFER, objectIdDepthStencil_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          pixelSize.width(), pixelSize.height());

    glGenFramebuffers(1, &objectIdFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, objectIdFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, objectIdTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, objectIdDepthStencil_);
    const GLenum colorAttachment = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &colorAttachment);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(previousFramebuffer));
    glBindRenderbuffer(GL_RENDERBUFFER,
                       static_cast<GLuint>(previousRenderbuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Object-ID outline framebuffer is incomplete; status:"
                   << Qt::hex << status;
        glDeleteFramebuffers(1, &objectIdFramebuffer_);
        glDeleteRenderbuffers(1, &objectIdDepthStencil_);
        glDeleteTextures(1, &objectIdTexture_);
        objectIdFramebuffer_ = 0;
        objectIdDepthStencil_ = 0;
        objectIdTexture_ = 0;
        return false;
    }
    return true;
}

bool ViewportSurfaceRenderer::ensureWorkbenchEffectFramebuffers(
    const QSize &pixelSize,
    const ViewportRenderTargets &targets)
{
    if (pixelSize.isEmpty() || targets.depthTexture == 0 ||
        targets.viewSpaceNormalTexture == 0 || targets.objectIdTexture == 0 ||
        targets.pixelSize != pixelSize) {
        return false;
    }

    if (workbenchEffectFramebufferSize_ != pixelSize) {
        if (workbenchEffectFramebuffer_ != 0) {
            glDeleteFramebuffers(1, &workbenchEffectFramebuffer_);
            workbenchEffectFramebuffer_ = 0;
        }
        if (workbenchOutlineIdTexture_ != 0) {
            glDeleteTextures(1, &workbenchOutlineIdTexture_);
            workbenchOutlineIdTexture_ = 0;
        }
        if (cavityDepthFramebuffer_ != 0) {
            glDeleteFramebuffers(1, &cavityDepthFramebuffer_);
            cavityDepthFramebuffer_ = 0;
        }
        if (cavityDepthTexture_ != 0) {
            glDeleteTextures(1, &cavityDepthTexture_);
            cavityDepthTexture_ = 0;
        }
        workbenchEffectFramebufferSize_ = pixelSize;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    if (workbenchOutlineIdTexture_ == 0) {
        glGenTextures(1, &workbenchOutlineIdTexture_);
        glBindTexture(GL_TEXTURE_2D, workbenchOutlineIdTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                     pixelSize.width(), pixelSize.height(), 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (cavityDepthTexture_ == 0) {
        glGenTextures(1, &cavityDepthTexture_);
        glBindTexture(GL_TEXTURE_2D, cavityDepthTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                     pixelSize.width(), pixelSize.height(), 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (workbenchEffectFramebuffer_ == 0) {
        glGenFramebuffers(1, &workbenchEffectFramebuffer_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, workbenchEffectFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, targets.viewSpaceNormalTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, targets.objectIdTexture, 0);
    const bool frontDepthDomain = targets.frontDepthTexture != 0 &&
                                  targets.depthTexture ==
                                      targets.frontDepthTexture;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                           GL_TEXTURE_2D,
                           frontDepthDomain &&
                                   targets.frontOutlineIdTexture != 0
                               ? targets.frontOutlineIdTexture
                               : workbenchOutlineIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, targets.depthTexture, 0);
    const GLenum drawAttachments[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, drawAttachments);
    const bool effectsComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (cavityDepthFramebuffer_ == 0) {
        glGenFramebuffers(1, &cavityDepthFramebuffer_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, cavityDepthFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, cavityDepthTexture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool depthSnapshotComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (!effectsComplete || !depthSnapshotComplete) {
        qWarning() << "Workbench effect framebuffer is incomplete"
                   << "effects=" << effectsComplete
                   << "depthSnapshot=" << depthSnapshotComplete;
        return false;
    }
    return true;
}

bool ViewportSurfaceRenderer::mergeWorkbenchFrontAttachments(
    const ViewportRenderTargets &targets, bool mergeColor, bool mergeNormal)
{
    if (targets.frontDepthTexture == 0 ||
        targets.frontViewSpaceNormalTexture == 0 ||
        targets.frontObjectIdTexture == 0 ||
        targets.frontOutlineIdTexture == 0 || targets.depthTexture == 0 ||
        targets.viewSpaceNormalTexture == 0 || targets.objectIdTexture == 0 ||
        (mergeColor && targets.currentSampleColorTexture == 0)) {
        return false;
    }
    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousDepthFunction = GL_LESS;
    GLint previousTextureBindings[5] = {};
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousStencil = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean previousCull = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    for (int unit = 0; unit < 5; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBindings[unit]);
    }
    if (frontMergeFramebuffer_ == 0) {
        glGenFramebuffers(1, &frontMergeFramebuffer_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, frontMergeFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           mergeColor ? targets.currentSampleColorTexture : 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, targets.viewSpaceNormalTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                           GL_TEXTURE_2D, targets.objectIdTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3,
                           GL_TEXTURE_2D, workbenchOutlineIdTexture_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, targets.depthTexture, 0);
    const GLenum drawBuffers[] = {mergeColor
                                      ? static_cast<GLenum>(GL_COLOR_ATTACHMENT0)
                                      : static_cast<GLenum>(GL_NONE),
                                  mergeNormal ? static_cast<GLenum>(GL_COLOR_ATTACHMENT1)
                                              : static_cast<GLenum>(GL_NONE),
                                  GL_COLOR_ATTACHMENT2,
                                  GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, drawBuffers);
    const bool complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glViewport(0, 0, targets.pixelSize.width(), targets.pixelSize.height());
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, targets.frontColorTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, targets.frontDepthTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, targets.frontViewSpaceNormalTexture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, targets.frontObjectIdTexture);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, targets.frontOutlineIdTexture);
        frontMergeProgram_.bind();
        frontMergeProgram_.setUniformValue("uFrontColor", 0);
        frontMergeProgram_.setUniformValue("uFrontDepth", 1);
        frontMergeProgram_.setUniformValue("uFrontNormal", 2);
        frontMergeProgram_.setUniformValue("uFrontObjectId", 3);
        frontMergeProgram_.setUniformValue("uFrontOutlineId", 4);
        frontMergeProgram_.setUniformValue("uMergeColor", mergeColor);
        outlineVertexArray_.bind();
        glDrawArrays(GL_TRIANGLES, 0, 3);
        outlineVertexArray_.release();
        frontMergeProgram_.release();
    }
    for (int unit = 0; unit < 5; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousTextureBindings[unit]));
    }
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glDepthMask(previousDepthMask);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (previousCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    return complete;
}

bool ViewportSurfaceRenderer::ensureTransparencyFramebuffer(
    const QSize &pixelSize,
    GLuint depthStencilTexture,
    bool frontDomain)
{
    if (pixelSize.isEmpty() || depthStencilTexture == 0) return false;

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    if (transparencyFramebufferSize_ != pixelSize) {
        if (transparencyFramebuffer_ != 0) {
            glDeleteFramebuffers(1, &transparencyFramebuffer_);
            transparencyFramebuffer_ = 0;
        }
        if (transparencyAccumulationTexture_ != 0) {
            glDeleteTextures(1, &transparencyAccumulationTexture_);
            transparencyAccumulationTexture_ = 0;
        }
        if (transparencyWeightTexture_ != 0) {
            glDeleteTextures(1, &transparencyWeightTexture_);
            transparencyWeightTexture_ = 0;
        }
        if (frontTransparencyFramebuffer_ != 0) {
            glDeleteFramebuffers(1, &frontTransparencyFramebuffer_);
            frontTransparencyFramebuffer_ = 0;
        }
        if (frontTransparencyAccumulationTexture_ != 0) {
            glDeleteTextures(1, &frontTransparencyAccumulationTexture_);
            frontTransparencyAccumulationTexture_ = 0;
        }
        if (frontTransparencyWeightTexture_ != 0) {
            glDeleteTextures(1, &frontTransparencyWeightTexture_);
            frontTransparencyWeightTexture_ = 0;
        }
        transparencyFramebufferSize_ = pixelSize;
    }

    GLuint &framebuffer = frontDomain ? frontTransparencyFramebuffer_
                                      : transparencyFramebuffer_;
    GLuint &accumulationTexture = frontDomain
        ? frontTransparencyAccumulationTexture_ : transparencyAccumulationTexture_;
    GLuint &weightTexture = frontDomain
        ? frontTransparencyWeightTexture_ : transparencyWeightTexture_;
    if (framebuffer == 0) {
        glGenTextures(1, &accumulationTexture);
        glBindTexture(GL_TEXTURE_2D, accumulationTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                     pixelSize.width(), pixelSize.height(), 0,
                     GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &weightTexture);
        glBindTexture(GL_TEXTURE_2D, weightTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F,
                     pixelSize.width(), pixelSize.height(), 0,
                     GL_RED, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &framebuffer);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, accumulationTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, weightTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, depthStencilTexture, 0);
    const GLenum drawAttachments[] = {GL_COLOR_ATTACHMENT0,
                                      GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, drawAttachments);
    const bool complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    return complete;
}

bool ViewportSurfaceRenderer::renderWorkbenchEffectPrepass(
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportRenderTargets &targets,
    const QVector<int> &rangeIndices,
    bool snapshotDepth,
    bool *snapshotDepthReady)
{
    if (snapshotDepthReady != nullptr) {
        *snapshotDepthReady = !snapshotDepth;
    }
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    if (vertices_.isEmpty() ||
        !ensureWorkbenchEffectFramebuffers(pixelSize, targets)) {
        return false;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousDepthFunction = GL_LESS;
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture0 = 0;
    GLint previousUdimArrayTexture = 0;
    GLint previousUdimLookupTexture = 0;
    GLint previousFrontDepthTexture = 0;
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);

    glBindFramebuffer(GL_FRAMEBUFFER, workbenchEffectFramebuffer_);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousUdimArrayTexture);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousUdimLookupTexture);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousFrontDepthTexture);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    if (settings.backfaceCulling) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }

    const GLfloat clearNormal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLuint clearId[4] = {0u, 0u, 0u, 0u};
    const GLfloat clearDepth[1] = {1.0f};
    glClearBufferfv(GL_COLOR, 0, clearNormal);
    glClearBufferuiv(GL_COLOR, 1, clearId);
    glClearBufferuiv(GL_COLOR, 2, clearId);
    glClearBufferfv(GL_DEPTH, 0, clearDepth);

    const Point3D viewDirection =
        normalized(transform.viewDirection(), {0.0, 0.0, 1.0});
    const Point3D viewUp = normalized(transform.viewUp(), {0.0, 1.0, 0.0});
    const Point3D viewRight = normalized(cross(viewUp, viewDirection),
                                         {1.0, 0.0, 0.0});
    workbenchEffectProgram_.bind();
    workbenchEffectProgram_.setUniformValue(
        "uViewProjection", viewportViewProjection(transform, viewportSize));
    workbenchEffectProgram_.setUniformValue("uViewRight", asVector(viewRight));
    workbenchEffectProgram_.setUniformValue("uViewUp", asVector(viewUp));
    workbenchEffectProgram_.setUniformValue("uViewFacing", asVector(viewDirection));
    const GLint objectIdUniformLocation =
        workbenchEffectProgram_.uniformLocation("uObjectId");
    const GLint outlineObjectIdUniformLocation =
        workbenchEffectProgram_.uniformLocation("uOutlineObjectId");
    workbenchEffectProgram_.setUniformValue("uWorkbenchImage", 0);
    workbenchEffectProgram_.setUniformValue("uWorkbenchUdimImage", 1);
    workbenchEffectProgram_.setUniformValue("uWorkbenchUdimLookup", 2);
    // This program is also used by the later transparent-depth pass, which
    // may leave front-coverage rejection enabled from the previous frame.
    workbenchEffectProgram_.setUniformValue("uExcludeFrontCoverage", false);
    workbenchEffectProgram_.setUniformValue("uFrontDepthMask", 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    const GLint useTextureAlphaMaskUniformLocation =
        workbenchEffectProgram_.uniformLocation("uUseTextureAlphaMask");
    const GLint textureAlphaCutoffUniformLocation =
        workbenchEffectProgram_.uniformLocation("uTextureAlphaCutoff");

    vertexArray_.bind();
    vertexBuffer_.bind();
    if (geometryDirty_) {
        vertexBuffer_.allocate(vertices_.constData(),
                               vertices_.size() *
                                   static_cast<int>(sizeof(SurfaceVertex)));
        geometryDirty_ = false;
    }
    for (int rangeIndex : rangeIndices) {
        if (rangeIndex < 0 || rangeIndex >= ranges_.size()) continue;
        const DrawRange &range = ranges_[rangeIndex];
        if (range.count <= 0) {
            continue;
        }
        const GLuint objectId = range.effectObjectId;
        GLuint outlineObjectId = range.outlineHandledBySceneStroke
                                     ? 0u
                                     : objectId;
        if (range.selected && !range.outlineHandledBySceneStroke) {
            outlineObjectId |= 0x80000000u;
        }
        glUniform1ui(objectIdUniformLocation, objectId);
        glUniform1ui(outlineObjectIdUniformLocation, outlineObjectId);
        const QString alphaMode = range.workbenchMaterial.textureImageAlphaMode;
        const bool ignoreTextureAlpha =
            alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
            alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
            alphaMode.compare(QStringLiteral("CHANNEL_PACKED"),
                              Qt::CaseInsensitive) == 0;
        const GLuint imageTexture = settings.colorMode == ViewportColorMode::Texture &&
                range.useTextureColor
            ? ensureImageTexture(range.textureImagePath,
                                 range.workbenchMaterial.textureImagePackedDataBase64,
                                 range.textureImageSrgb)
            : 0;
        const UdimTextureSet udimTexture = settings.colorMode == ViewportColorMode::Texture &&
                range.useTextureColor
            ? ensureUdimTexture(range.textureImageTiles, range.textureImageSrgb)
            : UdimTextureSet{};
        glUniform1i(useTextureAlphaMaskUniformLocation,
                    imageTexture != 0 || udimTexture.imageArray != 0);
        workbenchEffectProgram_.setUniformValue(
            "uUseUdimImage", udimTexture.imageArray != 0);
        glUniform1i(textureAlphaCutoffUniformLocation, !ignoreTextureAlpha);
        workbenchEffectProgram_.setUniformValue(
            "uTextureInterpolation", workbenchTextureInterpolation(
                range.workbenchMaterial.textureImageInterpolation));
        workbenchEffectProgram_.setUniformValue(
            "uTextureExtension", workbenchTextureExtension(
                range.workbenchMaterial.textureImageExtension));
        configureWorkbenchImageTexture(imageTexture, range.workbenchMaterial);
        glBindTexture(GL_TEXTURE_2D, imageTexture);
        configureWorkbenchUdimTexture(udimTexture.imageArray,
                                      range.workbenchMaterial);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, udimTexture.imageArray);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, udimTexture.tileLookup);
        glActiveTexture(GL_TEXTURE0);
        workbenchEffectProgram_.setUniformValue(
            "uWorldOffset",
            QVector3D(static_cast<float>(range.offset.x),
                      static_cast<float>(range.offset.y),
                      static_cast<float>(range.offset.z)));
        glDrawArrays(GL_TRIANGLES, range.first, range.count);
    }
    vertexBuffer_.release();
    vertexArray_.release();
    workbenchEffectProgram_.release();

    if (snapshotDepth) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, workbenchEffectFramebuffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cavityDepthFramebuffer_);
        const bool sourceComplete =
            glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE;
        const bool destinationComplete =
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE;
        if (sourceComplete && destinationComplete) {
            glBlitFramebuffer(0, 0, pixelSize.width(), pixelSize.height(),
                              0, 0, pixelSize.width(), pixelSize.height(),
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            if (snapshotDepthReady != nullptr) {
                *snapshotDepthReady = true;
            }
        } else {
            if (snapshotDepthReady != nullptr) {
                *snapshotDepthReady = false;
            }
        }
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  static_cast<GLuint>(previousUdimArrayTexture));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D,
                  static_cast<GLuint>(previousUdimLookupTexture));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D,
                  static_cast<GLuint>(previousFrontDepthTexture));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glUseProgram(static_cast<GLuint>(previousProgram));
    return true;
}

bool ViewportSurfaceRenderer::renderWorkbenchTransparentDepthPass(
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    const ViewportRenderTargets &targets,
    const QVector<int> &rangeIndices,
    bool excludeFrontCoverage)
{
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const QSize pixelSize(qRound(viewportSize.width() * dpr),
                          qRound(viewportSize.height() * dpr));
    if (rangeIndices.isEmpty() || vertices_.isEmpty() ||
        !ensureWorkbenchEffectFramebuffers(pixelSize, targets)) {
        return false;
    }

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousProgram = 0;
    GLint previousDepthFunction = GL_LESS;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture0 = 0;
    GLint previousTexture3 = 0;
    GLint previousUdimArrayTexture = 0;
    GLint previousUdimLookupTexture = 0;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousDrawBuffers[3] = {GL_COLOR_ATTACHMENT0,
                                    GL_COLOR_ATTACHMENT1,
                                    GL_COLOR_ATTACHMENT2};
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint previousCullFaceMode = GL_BACK;
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previousFramebufferSrgb =
        glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLint previousBlendSourceRgb = GL_ONE;
    GLint previousBlendDestinationRgb = GL_ZERO;
    GLint previousBlendSourceAlpha = GL_ONE;
    GLint previousBlendDestinationAlpha = GL_ZERO;
    GLint previousBlendEquationRgb = GL_FUNC_ADD;
    GLint previousBlendEquationAlpha = GL_FUNC_ADD;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFaceMode);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSourceRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDestinationRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSourceAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDestinationAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousUdimArrayTexture);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousUdimLookupTexture);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture3);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, workbenchEffectFramebuffer_);
    glGetIntegerv(GL_DRAW_BUFFER0, &previousDrawBuffers[0]);
    glGetIntegerv(GL_DRAW_BUFFER1, &previousDrawBuffers[1]);
    glGetIntegerv(GL_DRAW_BUFFER2, &previousDrawBuffers[2]);
    const GLenum depthPassDrawBuffers[] = {
        GL_NONE, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, depthPassDrawBuffers);
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    if (settings.backfaceCulling) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }

    const Point3D viewDirection =
        normalized(transform.viewDirection(), {0.0, 0.0, 1.0});
    const Point3D viewUp = normalized(transform.viewUp(), {0.0, 1.0, 0.0});
    const Point3D viewRight = normalized(cross(viewUp, viewDirection),
                                         {1.0, 0.0, 0.0});
    workbenchEffectProgram_.bind();
    workbenchEffectProgram_.setUniformValue(
        "uViewProjection", viewportViewProjection(transform, viewportSize));
    workbenchEffectProgram_.setUniformValue("uViewRight", asVector(viewRight));
    workbenchEffectProgram_.setUniformValue("uViewUp", asVector(viewUp));
    workbenchEffectProgram_.setUniformValue("uViewFacing", asVector(viewDirection));
    workbenchEffectProgram_.setUniformValue("uWorkbenchImage", 0);
    workbenchEffectProgram_.setUniformValue("uWorkbenchUdimImage", 1);
    workbenchEffectProgram_.setUniformValue("uWorkbenchUdimLookup", 2);
    workbenchEffectProgram_.setUniformValue("uExcludeFrontCoverage",
                                            excludeFrontCoverage);
    workbenchEffectProgram_.setUniformValue("uFrontDepthMask", 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D,
                  excludeFrontCoverage ? targets.frontDepthTexture : 0);
    glActiveTexture(GL_TEXTURE0);
    const GLint objectIdUniformLocation =
        workbenchEffectProgram_.uniformLocation("uObjectId");
    const GLint outlineObjectIdUniformLocation =
        workbenchEffectProgram_.uniformLocation("uOutlineObjectId");
    const GLint useTextureAlphaMaskUniformLocation =
        workbenchEffectProgram_.uniformLocation("uUseTextureAlphaMask");
    const GLint textureAlphaCutoffUniformLocation =
        workbenchEffectProgram_.uniformLocation("uTextureAlphaCutoff");

    vertexArray_.bind();
    vertexBuffer_.bind();
    if (geometryDirty_) {
        vertexBuffer_.allocate(vertices_.constData(),
                               vertices_.size() *
                                   static_cast<int>(sizeof(SurfaceVertex)));
        geometryDirty_ = false;
    }
    for (int rangeIndex : rangeIndices) {
        if (rangeIndex < 0 || rangeIndex >= ranges_.size()) continue;
        const DrawRange &range = ranges_[rangeIndex];
        if (range.count <= 0) continue;

        GLuint outlineObjectId = range.outlineHandledBySceneStroke
                                     ? 0u
                                     : range.effectObjectId;
        if (range.selected && !range.outlineHandledBySceneStroke) {
            outlineObjectId |= 0x80000000u;
        }
        glUniform1ui(objectIdUniformLocation, range.effectObjectId);
        glUniform1ui(outlineObjectIdUniformLocation, outlineObjectId);

        const QString alphaMode = range.workbenchMaterial.textureImageAlphaMode;
        const bool ignoreTextureAlpha =
            alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
            alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
            alphaMode.compare(QStringLiteral("CHANNEL_PACKED"),
                              Qt::CaseInsensitive) == 0;
        const GLuint imageTexture =
            settings.colorMode == ViewportColorMode::Texture &&
                    range.useTextureColor
                ? ensureImageTexture(
                      range.textureImagePath,
                      range.workbenchMaterial.textureImagePackedDataBase64,
                      range.textureImageSrgb)
                : 0;
        const UdimTextureSet udimTexture = settings.colorMode == ViewportColorMode::Texture &&
                range.useTextureColor
            ? ensureUdimTexture(range.textureImageTiles, range.textureImageSrgb)
            : UdimTextureSet{};
        glUniform1i(useTextureAlphaMaskUniformLocation,
                    imageTexture != 0 || udimTexture.imageArray != 0);
        workbenchEffectProgram_.setUniformValue(
            "uUseUdimImage", udimTexture.imageArray != 0);
        glUniform1i(textureAlphaCutoffUniformLocation, !ignoreTextureAlpha);
        workbenchEffectProgram_.setUniformValue(
            "uTextureInterpolation", workbenchTextureInterpolation(
                range.workbenchMaterial.textureImageInterpolation));
        workbenchEffectProgram_.setUniformValue(
            "uTextureExtension", workbenchTextureExtension(
                range.workbenchMaterial.textureImageExtension));
        configureWorkbenchImageTexture(imageTexture, range.workbenchMaterial);
        glBindTexture(GL_TEXTURE_2D, imageTexture);
        configureWorkbenchUdimTexture(udimTexture.imageArray,
                                      range.workbenchMaterial);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, udimTexture.imageArray);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, udimTexture.tileLookup);
        glActiveTexture(GL_TEXTURE0);
        workbenchEffectProgram_.setUniformValue(
            "uWorldOffset",
            QVector3D(static_cast<float>(range.offset.x),
                      static_cast<float>(range.offset.y),
                      static_cast<float>(range.offset.z)));
        glDrawArrays(GL_TRIANGLES, range.first, range.count);
    }
    vertexBuffer_.release();
    vertexArray_.release();
    workbenchEffectProgram_.release();

    const GLenum restoredDrawBuffers[] = {
        static_cast<GLenum>(previousDrawBuffers[0]),
        static_cast<GLenum>(previousDrawBuffers[1]),
        static_cast<GLenum>(previousDrawBuffers[2])};
    glDrawBuffers(3, restoredDrawBuffers);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (previousBlend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    if (previousCullFace) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    glCullFace(static_cast<GLenum>(previousCullFaceMode));
    if (previousScissor) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    if (previousStencilTest) glEnable(GL_STENCIL_TEST);
    else glDisable(GL_STENCIL_TEST);
    if (previousFramebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB);
    else glDisable(GL_FRAMEBUFFER_SRGB);
    glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                            static_cast<GLenum>(previousBlendEquationAlpha));
    glBlendFuncSeparate(static_cast<GLenum>(previousBlendSourceRgb),
                        static_cast<GLenum>(previousBlendDestinationRgb),
                        static_cast<GLenum>(previousBlendSourceAlpha),
                        static_cast<GLenum>(previousBlendDestinationAlpha));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  static_cast<GLuint>(previousUdimArrayTexture));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D,
                  static_cast<GLuint>(previousUdimLookupTexture));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture3));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glUseProgram(static_cast<GLuint>(previousProgram));
    return true;
}

bool ViewportSurfaceRenderer::renderShadowMap(
    const ViewportTransform &transform,
    const QSize &viewportSize,
    const ViewportShadingSettings &settings,
    bool fallbackOnly)
{
    if (vertices_.isEmpty() || !ensureShadowMap()) {
        return false;
    }

    QVector3D minimum(std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    QVector3D maximum(std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest());
    bool hasVertex = false;
    for (const DrawRange &range : ranges_) {
        const QVector3D offset(static_cast<float>(range.offset.x),
                               static_cast<float>(range.offset.y),
                               static_cast<float>(range.offset.z));
        for (int index = range.first; index < range.first + range.count; ++index) {
            const SurfaceVertex &vertex = vertices_[index];
            const QVector3D position(vertex.x, vertex.y, vertex.z);
            minimum.setX(std::min(minimum.x(), position.x() + offset.x()));
            minimum.setY(std::min(minimum.y(), position.y() + offset.y()));
            minimum.setZ(std::min(minimum.z(), position.z() + offset.z()));
            maximum.setX(std::max(maximum.x(), position.x() + offset.x()));
            maximum.setY(std::max(maximum.y(), position.y() + offset.y()));
            maximum.setZ(std::max(maximum.z(), position.z() + offset.z()));
            hasVertex = true;
        }
    }
    if (!hasVertex) {
        return false;
    }

    const QVector3D center = (minimum + maximum) * 0.5f;
    float radius = std::max((maximum - minimum).length() * 0.5f, 0.5f);
    const QVector3D lightDirection = workbenchShadowDirectionWorld(settings);
    const QVector3D eye = center - lightDirection * (radius * 2.5f);
    QVector3D up(0.0f, 1.0f, 0.0f);
    if (std::abs(QVector3D::dotProduct(lightDirection, up)) > 0.95f) {
        up = QVector3D(1.0f, 0.0f, 0.0f);
    }
    QMatrix4x4 lightView;
    lightView.lookAt(eye, center, up);
    QMatrix4x4 lightProjection;
    lightProjection.ortho(-radius, radius, -radius, radius,
                          0.01f, radius * 6.0f);
    shadowViewProjection_ = lightProjection * lightView;

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {};
    GLint previousDepthFunction = GL_LESS;
    GLint previousProgram = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture0 = 0;
    GLint previousShadowUdimArrayTexture = 0;
    GLint previousShadowUdimLookupTexture = 0;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previousDepthMask = GL_TRUE;
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,
                  &previousShadowUdimArrayTexture);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,
                  &previousShadowUdimLookupTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glViewport(0, 0, kWorkbenchShadowMapSize, kWorkbenchShadowMapSize);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);

    vertexArray_.bind();
    vertexBuffer_.bind();
    if (geometryDirty_) {
        vertexBuffer_.allocate(vertices_.constData(),
                               vertices_.size() *
                                   static_cast<int>(sizeof(SurfaceVertex)));
        geometryDirty_ = false;
    }
    shadowProgram_.bind();
    shadowProgram_.setUniformValue("uShadowViewProjection",
                                   shadowViewProjection_);
    shadowProgram_.setUniformValue("uShadowAlphaTexture", 0);
    shadowProgram_.setUniformValue("uShadowUdimImage", 1);
    shadowProgram_.setUniformValue("uShadowUdimLookup", 2);
    for (const DrawRange &range : ranges_) {
        if (range.count <= 0 ||
            (fallbackOnly && range.shadowVolumeSupported)) {
            continue;
        }
        shadowProgram_.setUniformValue(
            "uWorldOffset",
            QVector3D(static_cast<float>(range.offset.x),
                      static_cast<float>(range.offset.y),
                      static_cast<float>(range.offset.z)));
        const QString alphaMode = range.workbenchMaterial.textureImageAlphaMode;
        const bool ignoreTextureAlpha =
            alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
            alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
            alphaMode.compare(QStringLiteral("CHANNEL_PACKED"),
                              Qt::CaseInsensitive) == 0;
        const GLuint alphaTexture = range.shadowTextureAlphaMask &&
                settings.colorMode == ViewportColorMode::Texture &&
                !ignoreTextureAlpha
            ? ensureImageTexture(range.textureImagePath,
                                 range.workbenchMaterial.textureImagePackedDataBase64,
                                 range.textureImageSrgb)
            : 0;
        const UdimTextureSet udimTexture = range.shadowTextureAlphaMask &&
                settings.colorMode == ViewportColorMode::Texture &&
                !ignoreTextureAlpha
            ? ensureUdimTexture(range.textureImageTiles, range.textureImageSrgb)
            : UdimTextureSet{};
        shadowProgram_.setUniformValue(
            "uTextureInterpolation", workbenchTextureInterpolation(
                range.workbenchMaterial.textureImageInterpolation));
        shadowProgram_.setUniformValue(
            "uTextureExtension", workbenchTextureExtension(
                range.workbenchMaterial.textureImageExtension));
        configureWorkbenchImageTexture(alphaTexture, range.workbenchMaterial);
        configureWorkbenchUdimTexture(udimTexture.imageArray,
                                      range.workbenchMaterial);
        shadowProgram_.setUniformValue("uUseShadowAlphaMask",
                                      alphaTexture != 0 || udimTexture.imageArray != 0);
        shadowProgram_.setUniformValue("uUseShadowUdimImage",
                                      udimTexture.imageArray != 0);
        glBindTexture(GL_TEXTURE_2D, alphaTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, udimTexture.imageArray);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, udimTexture.tileLookup);
        glActiveTexture(GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLES, range.first, range.count);
    }
    shadowProgram_.release();
    vertexBuffer_.release();
    vertexArray_.release();

    glBindFramebuffer(GL_FRAMEBUFFER,
                      static_cast<GLuint>(previousFramebuffer));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
                  static_cast<GLuint>(previousShadowUdimArrayTexture));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D,
                  static_cast<GLuint>(previousShadowUdimLookupTexture));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glUseProgram(static_cast<GLuint>(previousProgram));
    Q_UNUSED(transform);
    Q_UNUSED(viewportSize);
    return true;
}

bool ViewportSurfaceRenderer::renderShadowVolumes(
    const QVector3D &lightDirection,
    const QMatrix4x4 &viewProjection,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const QVector<int> &volumeRangeIndices,
    const QVector<int> &opaqueRangeIndices)
{
    struct EdgeAdjacency {
        int count = 0;
        int firstTriangle = -1;
        int secondTriangle = -1;
        int firstFrom = -1;
        int firstTo = -1;
        int secondFrom = -1;
        int secondTo = -1;
    };
    const auto edgeKey = [](int first, int second) {
        const quint32 low = static_cast<quint32>(std::min(first, second));
        const quint32 high = static_cast<quint32>(std::max(first, second));
        return (quint64(low) << 32u) | high;
    };
    const auto appendTriangle = [](QVector<QVector3D> *vertices,
                                   const QVector3D &a,
                                   const QVector3D &b,
                                   const QVector3D &c) {
        vertices->append(a);
        vertices->append(b);
        vertices->append(c);
    };

    // OpenGL clip space uses z <= w for the far clip plane. Row 3 minus row 2
    // gives its inward clip-plane covector; negate the normal to get the
    // outward plane orientation used by Blender's shadow extrusion math.
    const QVector4D inwardFarClipPlane(
        viewProjection(3, 0) - viewProjection(2, 0),
        viewProjection(3, 1) - viewProjection(2, 1),
        viewProjection(3, 2) - viewProjection(2, 2),
        viewProjection(3, 3) - viewProjection(2, 3));
    const float farPlaneNormalLength =
        QVector3D(inwardFarClipPlane.x(), inwardFarClipPlane.y(),
                  inwardFarClipPlane.z()).length();
    if (!std::isfinite(farPlaneNormalLength) ||
        farPlaneNormalLength <= 1.0e-8f) {
        return false;
    }
    const QVector3D farPlaneNormal(
        -inwardFarClipPlane.x() / farPlaneNormalLength,
        -inwardFarClipPlane.y() / farPlaneNormalLength,
        -inwardFarClipPlane.z() / farPlaneNormalLength);
    const float farPlaneOffset =
        inwardFarClipPlane.w() / farPlaneNormalLength;
    const float lightFarPlaneDot =
        QVector3D::dotProduct(lightDirection, farPlaneNormal);
    const auto extrudeToFarPlane = [&](const QVector3D &position) {
        float extrusionDistance = 1.0e5f;
        if (lightFarPlaneDot > 0.0f) {
            const float signedDistance =
                QVector3D::dotProduct(farPlaneNormal, position) -
                farPlaneOffset;
            extrusionDistance = -signedDistance / lightFarPlaneDot;
            // Keep the cap just inside the far plane, matching Blender.
            extrusionDistance -= 1.0e-3f;
        }
        return position + lightDirection * extrusionDistance;
    };

    QVector<QVector3D> volumeVertices;
    QVector<QVector3D> depthPassSideVertices;
    bool useDepthPass = true;
    const auto isNearPlaneSafe = [&viewProjection](const QVector3D &position) {
        const QVector4D clip =
            viewProjection * QVector4D(position, 1.0f);
        if (!std::isfinite(clip.x()) || !std::isfinite(clip.y()) ||
            !std::isfinite(clip.z()) || !std::isfinite(clip.w()) ||
            clip.w() <= 1.0e-6f) {
            return false;
        }
        const float nearDistance = clip.z() + clip.w();
        const float safetyMargin =
            1.0e-5f * std::max(1.0f, std::abs(clip.w()));
        return nearDistance > safetyMargin;
    };
    for (const int rangeIndex : volumeRangeIndices) {
        if (rangeIndex < 0 || rangeIndex >= ranges_.size()) return false;
        const DrawRange &range = ranges_[rangeIndex];
        if (!range.shadowVolumeSupported || range.shadowTriangles.isEmpty()) {
            continue;
        }
        if (!range.shadowVolumeClosed ||
            !opaqueRangeIndices.contains(rangeIndex)) {
            useDepthPass = false;
        }
        const QVector3D offset(static_cast<float>(range.offset.x),
                               static_cast<float>(range.offset.y),
                               static_cast<float>(range.offset.z));
        for (const QVector3D &vertex : range.shadowVertices) {
            const QVector3D position = vertex + offset;
            if (!isNearPlaneSafe(position) ||
                !isNearPlaneSafe(extrudeToFarPlane(position))) {
                useDepthPass = false;
                break;
            }
        }
        QVector<bool> facingLight;
        facingLight.reserve(range.shadowTriangles.size());
        QHash<quint64, EdgeAdjacency> edges;

        for (int triangleIndex = 0;
             triangleIndex < range.shadowTriangles.size(); ++triangleIndex) {
            const auto &triangle = range.shadowTriangles[triangleIndex];
            for (const int index : triangle) {
                if (index < 0 || index >= range.shadowVertices.size()) {
                    return false;
                }
            }
            const QVector3D a = range.shadowVertices[triangle[0]] + offset;
            const QVector3D b = range.shadowVertices[triangle[1]] + offset;
            const QVector3D c = range.shadowVertices[triangle[2]] + offset;
            const QVector3D normal = QVector3D::crossProduct(b - a, c - a);
            if (!std::isfinite(normal.lengthSquared()) ||
                normal.lengthSquared() <= 1.0e-18f) {
                return false;
            }
            // Blender's pass marks dot(N, light-ray-direction) > 0 as the
            // back-facing side. The opposite faces receive light.
            const bool faceLit =
                QVector3D::dotProduct(normal, lightDirection) < 0.0f;
            facingLight.append(faceLit);

            if (faceLit) {
                const QVector3D farA = extrudeToFarPlane(a);
                const QVector3D farB = extrudeToFarPlane(b);
                const QVector3D farC = extrudeToFarPlane(c);
                // Only light-facing faces cap the volume; their exposed
                // boundary is closed by the silhouette side quads.
                appendTriangle(&volumeVertices, a, b, c);
                appendTriangle(&volumeVertices, farC, farB, farA);
            }

            const int directedEdges[][2] = {
                {triangle[0], triangle[1]},
                {triangle[1], triangle[2]},
                {triangle[2], triangle[0]}};
            for (const auto &edge : directedEdges) {
                EdgeAdjacency &adjacent = edges[edgeKey(edge[0], edge[1])];
                if (adjacent.count == 0) {
                    adjacent.firstTriangle = triangleIndex;
                    adjacent.firstFrom = edge[0];
                    adjacent.firstTo = edge[1];
                } else if (adjacent.count == 1) {
                    adjacent.secondTriangle = triangleIndex;
                    adjacent.secondFrom = edge[0];
                    adjacent.secondTo = edge[1];
                }
                ++adjacent.count;
                if (adjacent.count > 2) return false;
            }
        }

        for (const EdgeAdjacency &adjacent : edges) {
            if (adjacent.count == 1 && adjacent.firstTriangle >= 0) {
                if (facingLight[adjacent.firstTriangle]) {
                    const QVector3D a =
                        range.shadowVertices[adjacent.firstFrom] + offset;
                    const QVector3D b =
                        range.shadowVertices[adjacent.firstTo] + offset;
                    const QVector3D farA = extrudeToFarPlane(a);
                    const QVector3D farB = extrudeToFarPlane(b);
                    // Boundary edges close the forced-fail volume of an open
                    // sheet, matching Workbench's single border-edge quad.
                    appendTriangle(&volumeVertices, a, farA, farB);
                    appendTriangle(&volumeVertices, a, farB, b);
                    appendTriangle(&depthPassSideVertices, a, farA, farB);
                    appendTriangle(&depthPassSideVertices, a, farB, b);
                }
                continue;
            }
            if (adjacent.count != 2 || adjacent.firstTriangle < 0 ||
                adjacent.secondTriangle < 0) {
                return false;
            }
            const bool firstFacesLight =
                facingLight[adjacent.firstTriangle];
            const bool secondFacesLight =
                facingLight[adjacent.secondTriangle];
            if (firstFacesLight == secondFacesLight) continue;

            // Take the directed edge from the light-facing triangle. This
            // winding closes the volume against both caps.
            const int from = firstFacesLight ? adjacent.firstFrom
                                             : adjacent.secondFrom;
            const int to = firstFacesLight ? adjacent.firstTo
                                           : adjacent.secondTo;
            const QVector3D a = range.shadowVertices[from] + offset;
            const QVector3D b = range.shadowVertices[to] + offset;
            const QVector3D farA = extrudeToFarPlane(a);
            const QVector3D farB = extrudeToFarPlane(b);
            appendTriangle(&volumeVertices, a, farA, farB);
            appendTriangle(&volumeVertices, a, farB, b);
            appendTriangle(&depthPassSideVertices, a, farA, farB);
            appendTriangle(&depthPassSideVertices, a, farB, b);
        }
    }
    if (volumeVertices.isEmpty()) return false;
    useDepthPass = useDepthPass && !depthPassSideVertices.isEmpty();

    GLint previousViewport[4] = {};
    GLint previousDepthFunction = GL_LESS;
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousArrayBuffer = 0;
    GLint previousFrontFace = GL_CCW;
    GLint previousStencilClearValue = 0;
    GLint previousStencilFunc = GL_ALWAYS;
    GLint previousStencilRef = 0;
    GLint previousStencilValueMask = -1;
    GLint previousStencilWriteMask = -1;
    GLint previousStencilFail = GL_KEEP;
    GLint previousStencilDepthFail = GL_KEEP;
    GLint previousStencilDepthPass = GL_KEEP;
    GLint previousStencilBackFunc = GL_ALWAYS;
    GLint previousStencilBackRef = 0;
    GLint previousStencilBackValueMask = -1;
    GLint previousStencilBackWriteMask = -1;
    GLint previousStencilBackFail = GL_KEEP;
    GLint previousStencilBackDepthFail = GL_KEEP;
    GLint previousStencilBackDepthPass = GL_KEEP;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean previousDepthMask = GL_TRUE;
    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previousDepthClamp = glIsEnabled(GL_DEPTH_CLAMP);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_FRONT_FACE, &previousFrontFace);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &previousStencilClearValue);
    glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunc);
    glGetIntegerv(GL_STENCIL_REF, &previousStencilRef);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
    glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &previousStencilDepthFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &previousStencilDepthPass);
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &previousStencilBackFunc);
    glGetIntegerv(GL_STENCIL_BACK_REF, &previousStencilBackRef);
    glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK,
                  &previousStencilBackValueMask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK,
                  &previousStencilBackWriteMask);
    glGetIntegerv(GL_STENCIL_BACK_FAIL, &previousStencilBackFail);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                  &previousStencilBackDepthFail);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS,
                  &previousStencilBackDepthPass);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);

    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    glViewport(0, 0, qRound(viewportSize.width() * dpr),
               qRound(viewportSize.height() * dpr));
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFFFFFFFFu);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFFu);
    if (useDepthPass) {
        // Blender uses the depth-pass technique for opaque manifold casters.
        // This route is enabled only when the full volume lies beyond the
        // camera near plane; all other cases retain the capped depth-fail path.
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    } else {
        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    }

    shadowVolumeVertexArray_.bind();
    shadowVolumeVertexBuffer_.bind();
    shadowVolumeVertexBuffer_.allocate(
        (useDepthPass ? depthPassSideVertices : volumeVertices).constData(),
        (useDepthPass ? depthPassSideVertices.size() : volumeVertices.size()) *
            static_cast<int>(sizeof(QVector3D)));
    shadowProgram_.bind();
    shadowProgram_.setUniformValue("uShadowViewProjection", viewProjection);
    shadowProgram_.setUniformValue("uWorldOffset", QVector3D());
    shadowProgram_.setUniformValue("uUseShadowAlphaMask", false);
    glDrawArrays(GL_TRIANGLES, 0,
                 useDepthPass ? depthPassSideVertices.size()
                              : volumeVertices.size());
    shadowProgram_.release();
    shadowVolumeVertexBuffer_.release();
    shadowVolumeVertexArray_.release();

    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousStencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (previousDepthClamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glFrontFace(static_cast<GLenum>(previousFrontFace));
    glClearStencil(previousStencilClearValue);
    glStencilFuncSeparate(GL_FRONT, static_cast<GLenum>(previousStencilFunc),
                          previousStencilRef,
                          static_cast<GLuint>(previousStencilValueMask));
    glStencilMaskSeparate(GL_FRONT,
                          static_cast<GLuint>(previousStencilWriteMask));
    glStencilOpSeparate(GL_FRONT, static_cast<GLenum>(previousStencilFail),
                        static_cast<GLenum>(previousStencilDepthFail),
                        static_cast<GLenum>(previousStencilDepthPass));
    glStencilFuncSeparate(GL_BACK,
                          static_cast<GLenum>(previousStencilBackFunc),
                          previousStencilBackRef,
                          static_cast<GLuint>(previousStencilBackValueMask));
    glStencilMaskSeparate(GL_BACK,
                          static_cast<GLuint>(previousStencilBackWriteMask));
    glStencilOpSeparate(GL_BACK,
                        static_cast<GLenum>(previousStencilBackFail),
                        static_cast<GLenum>(previousStencilBackDepthFail),
                        static_cast<GLenum>(previousStencilBackDepthPass));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glUseProgram(static_cast<GLuint>(previousProgram));
    return true;
}

bool ViewportSurfaceRenderer::ensureMatcapTexture(const QString &presetName)
{
    if (matcapTexture_ != 0 && matcapTextureName_ == presetName) {
        return true;
    }
    QImage diffuseImage = workbenchMatcapDiffuseImage(presetName);
    QImage specularImage = workbenchMatcapSpecularImage(presetName);
    if (diffuseImage.isNull() || specularImage.isNull() ||
        diffuseImage.size() != specularImage.size()) {
        return false;
    }
    diffuseImage = diffuseImage.convertToFormat(QImage::Format_RGBA8888)
                       .mirrored(false, true);
    specularImage = specularImage.convertToFormat(QImage::Format_RGBA8888)
                        .mirrored(false, true);
    QByteArray texturePixels;
    texturePixels.reserve(static_cast<qsizetype>(diffuseImage.sizeInBytes() +
                                                 specularImage.sizeInBytes()));
    texturePixels.append(reinterpret_cast<const char *>(diffuseImage.constBits()),
                         static_cast<qsizetype>(diffuseImage.sizeInBytes()));
    texturePixels.append(reinterpret_cast<const char *>(specularImage.constBits()),
                         static_cast<qsizetype>(specularImage.sizeInBytes()));

    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousBinding);
    if (matcapTexture_ == 0) {
        glGenTextures(1, &matcapTexture_);
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, matcapTexture_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8,
                 diffuseImage.width(), diffuseImage.height(), 2, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texturePixels.constData());
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(previousBinding));
    matcapTextureName_ = presetName;
    return matcapTexture_ != 0;
}

GLuint ViewportSurfaceRenderer::ensureImageTexture(const QString &path, bool srgb)
{
    return ensureImageTexture(path, {}, srgb);
}

GLuint ViewportSurfaceRenderer::ensureImageTexture(
    const QString &path, const QString &packedDataBase64, bool srgb)
{
    if (path.isEmpty() && packedDataBase64.isEmpty()) return 0;
    const QString sourceKey = !packedDataBase64.isEmpty()
        ? QStringLiteral("PACKED\n") + QString::fromLatin1(
              QCryptographicHash::hash(packedDataBase64.toLatin1(),
                                       QCryptographicHash::Sha256).toHex())
        : path;
    const QString cacheKey = sourceKey + (srgb ? QStringLiteral("\nSRGB")
                                               : QStringLiteral("\nLINEAR"));
    const auto cached = imageTextures_.constFind(cacheKey);
    if (cached != imageTextures_.cend()) return cached.value();
    QImage image;
    if (!packedDataBase64.isEmpty()) {
        const QByteArray encoded = packedDataBase64.toLatin1();
        if (encoded.size() > 44 * 1024 * 1024) return 0;
        const QByteArray bytes = QByteArray::fromBase64(encoded);
        if (bytes.size() > 32 * 1024 * 1024) return 0;
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) return 0;
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
            static_cast<quint64>(size.width()) * size.height() > 67108864ULL) {
            imageTextures_.insert(cacheKey, 0);
            return 0;
        }
        image = reader.read();
    } else {
        const QFileInfo info(path);
        const QByteArray suffix = info.suffix().toLower().toLatin1();
        static const QSet<QByteArray> allowedFormats{
            "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"};
        if (!info.isFile() || !allowedFormats.contains(suffix) ||
            info.size() <= 0 || info.size() > 256LL * 1024LL * 1024LL) {
            imageTextures_.insert(cacheKey, 0);
            return 0;
        }
        QImageReader reader(path);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
            static_cast<quint64>(size.width()) * size.height() > 67108864ULL) {
            imageTextures_.insert(cacheKey, 0);
            return 0;
        }
        image = reader.read();
    }
    if (image.isNull()) {
        imageTextures_.insert(cacheKey, 0);
        return 0;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888)
                 .mirrored(false, true);
    if (image.width() > 16384 || image.height() > 16384 ||
        static_cast<quint64>(image.width()) * image.height() > 67108864ULL) {
        imageTextures_.insert(cacheKey, 0);
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        imageTextures_.insert(cacheKey, 0);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                 image.width(), image.height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    imageTextures_.insert(cacheKey, texture);
    return texture;
}

ViewportSurfaceRenderer::UdimTextureSet ViewportSurfaceRenderer::ensureUdimTexture(
    const QVector<DocumentImageTextureTile> &tiles, bool srgb)
{
    if (tiles.isEmpty()) return {};
    QByteArray cacheMaterial;
    QDataStream cacheStream(&cacheMaterial, QIODevice::WriteOnly);
    cacheStream.setVersion(QDataStream::Qt_5_15);
    cacheStream << srgb;
    for (const DocumentImageTextureTile &tile : tiles) {
        const QFileInfo info(tile.filepath);
        cacheStream << qint32(tile.number) << tile.filepath << tile.sourceFileExists
                    << info.size() << info.lastModified().toMSecsSinceEpoch()
                    << QCryptographicHash::hash(tile.packedDataBase64.toLatin1(),
                                                QCryptographicHash::Sha256);
    }
    const QString cacheKey = QStringLiteral("UDIM\n") + QString::fromLatin1(
        QCryptographicHash::hash(cacheMaterial, QCryptographicHash::Sha256).toHex());
    const auto cached = udimImageTextures_.constFind(cacheKey);
    if (cached != udimImageTextures_.cend()) return cached.value();

    struct LoadedTile {
        int number = 0;
        QImage image;
    };
    QVector<LoadedTile> loaded;
    QSet<int> seenNumbers;
    int maximumWidth = 0;
    int maximumHeight = 0;
    quint64 decodedPixelCount = 0;
    qsizetype packedTileBytes = 0;
    for (const DocumentImageTextureTile &tile : tiles) {
        if (tile.number < 1001 || tile.number > 1999 ||
            seenNumbers.contains(tile.number)) continue;
        seenNumbers.insert(tile.number);
        const QFileInfo info(tile.filepath);
        const QByteArray suffix = info.suffix().toLower().toLatin1();
        static const QSet<QByteArray> allowedFormats{
            "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"};
        QByteArray packedBytes;
        if (!tile.packedDataBase64.isEmpty()) {
            const QByteArray encoded = tile.packedDataBase64.toLatin1();
            if (encoded.size() <= 44 * 1024 * 1024) {
                packedBytes = QByteArray::fromBase64(
                    encoded, QByteArray::AbortOnBase64DecodingErrors);
            }
            if (packedBytes.size() > 32 * 1024 * 1024 ||
                packedTileBytes + packedBytes.size() > 32 * 1024 * 1024) {
                packedBytes.clear();
            } else if (!packedBytes.isEmpty()) {
                packedTileBytes += packedBytes.size();
            }
        }
        QImage image;
        if (!packedBytes.isEmpty()) {
            QBuffer buffer;
            buffer.setData(packedBytes);
            if (buffer.open(QIODevice::ReadOnly)) {
                QImageReader reader(&buffer);
                const QSize size = reader.size();
                if (size.isValid() && size.width() <= 16384 && size.height() <= 16384 &&
                    static_cast<quint64>(size.width()) * size.height() <= 67108864ULL &&
                    decodedPixelCount + static_cast<quint64>(size.width()) * size.height() <=
                        67108864ULL) image = reader.read();
            }
        }
        if (image.isNull() && tile.sourceFileExists && info.isFile() &&
            allowedFormats.contains(suffix) && info.size() > 0 &&
            info.size() <= 256LL * 1024LL * 1024LL) {
            QImageReader reader(tile.filepath);
            const QSize size = reader.size();
            if (size.isValid() && size.width() <= 16384 && size.height() <= 16384 &&
                static_cast<quint64>(size.width()) * size.height() <= 67108864ULL &&
                decodedPixelCount + static_cast<quint64>(size.width()) * size.height() <=
                    67108864ULL) image = reader.read();
        }
        if (image.isNull()) continue;
        const quint64 tilePixels = static_cast<quint64>(image.width()) * image.height();
        if (decodedPixelCount + tilePixels > 67108864ULL) continue;
        decodedPixelCount += tilePixels;
        image = image.convertToFormat(QImage::Format_RGBA8888)
                     .mirrored(false, true);
        maximumWidth = std::max(maximumWidth, image.width());
        maximumHeight = std::max(maximumHeight, image.height());
        loaded.append({tile.number, std::move(image)});
    }
    if (loaded.isEmpty()) {
        QImage missingTile(1, 1, QImage::Format_RGBA8888);
        missingTile.fill(QColor(255, 0, 255, 255));
        loaded.append({0, std::move(missingTile)});
        maximumWidth = 1;
        maximumHeight = 1;
    }
    GLint maximumLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maximumLayers);
    const quint64 layerPixels = static_cast<quint64>(maximumWidth) * maximumHeight;
    if (maximumLayers <= 0 || loaded.size() > maximumLayers ||
        layerPixels > 67108864ULL ||
        layerPixels * static_cast<quint64>(loaded.size()) > 67108864ULL) {
        udimImageTextures_.insert(cacheKey, {});
        return {};
    }

    QVector<float> tileLookup(10 * 100 * 4, 0.0f);
    QVector<QImage> paddedImages;
    paddedImages.reserve(loaded.size());
    for (int layer = 0; layer < loaded.size(); ++layer) {
        QImage image(maximumWidth, maximumHeight, QImage::Format_RGBA8888);
        const QImage &source = loaded[layer].image;
        for (int y = 0; y < maximumHeight; ++y) {
            uchar *destination = image.scanLine(y);
            const int sourceY = std::min(y, source.height() - 1);
            const uchar *sourceLine = source.constScanLine(sourceY);
            for (int x = 0; x < maximumWidth; ++x) {
                const int sourceX = std::min(x, source.width() - 1);
                std::memcpy(destination + x * 4, sourceLine + sourceX * 4, 4);
            }
        }
        if (loaded[layer].number < 1001 || loaded[layer].number > 1999) {
            paddedImages.append(std::move(image));
            continue;
        }
        const int tileOffset = loaded[layer].number - 1001;
        const int mapOffset = tileOffset * 4;
        tileLookup[mapOffset] = static_cast<float>(layer + 1);
        tileLookup[mapOffset + 1] = static_cast<float>(source.width()) /
                                    static_cast<float>(maximumWidth);
        tileLookup[mapOffset + 2] = static_cast<float>(source.height()) /
                                    static_cast<float>(maximumHeight);
        paddedImages.append(std::move(image));
    }

    GLint previousArrayBinding = 0;
    GLint previous2dBinding = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousArrayBinding);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous2dBinding);
    GLuint imageArray = 0;
    GLuint lookupTexture = 0;
    glGenTextures(1, &imageArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, imageArray);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                 maximumWidth, maximumHeight, paddedImages.size(), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    for (int layer = 0; layer < paddedImages.size(); ++layer) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
                        maximumWidth, maximumHeight, 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, paddedImages[layer].constBits());
    }
    glGenTextures(1, &lookupTexture);
    glBindTexture(GL_TEXTURE_2D, lookupTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 10, 100, 0, GL_RGBA,
                 GL_FLOAT, tileLookup.constData());
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(previousArrayBinding));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous2dBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    const UdimTextureSet result{imageArray, lookupTexture};
    udimImageTextures_.insert(cacheKey, result);
    return result;
}

bool ViewportSurfaceRenderer::ensureAgxDisplayTexture(
    int colorTransform, const QString &customLutId,
    const QByteArray &customLutBytes,
    const ImportedBlenderColorLutMetadata &customLutMetadata)
{
    const QString customKey = customLutId + QLatin1Char('|') +
        QString::number(customLutMetadata.size) + QLatin1Char('|') +
        QString::number(customLutMetadata.domainMin, 'g', 17) + QLatin1Char('|') +
        QString::number(customLutMetadata.domainMax, 'g', 17) + QLatin1Char('|') +
        customLutMetadata.shaper + QLatin1Char('|') +
        QString::number(customLutMetadata.shaperScale, 'g', 17);
    if (colorTransform == 4) {
        const auto cached = customDisplayTextures_.constFind(customKey);
        if (cached != customDisplayTextures_.cend()) {
            agxDisplayTexture_ = cached.value();
            agxDisplayTextureTransform_ = 4;
            agxDisplayTextureCustomId_ = customKey;
            return true;
        }
    }
    if (agxDisplayTexture_ != 0 &&
        agxDisplayTextureTransform_ == colorTransform &&
        (colorTransform != 4 || agxDisplayTextureCustomId_ == customKey)) {
        return true;
    }
    QVector<float> customValues;
    const int edge = colorTransform == 4
        ? customLutMetadata.size : workbenchAgxDisplayLutSize;
    if (colorTransform == 4) {
        const qsizetype count = qsizetype(edge) * edge * edge * 3;
        if (customLutId.size() != 64 || edge < 2 || edge > 140 ||
            !std::isfinite(customLutMetadata.domainMin) ||
            !std::isfinite(customLutMetadata.domainMax) ||
            customLutMetadata.domainMax <= customLutMetadata.domainMin ||
            !std::isfinite(customLutMetadata.shaperScale) ||
            customLutMetadata.shaperScale <= 0.0 ||
            (customLutMetadata.shaper != QStringLiteral("linear") &&
             customLutMetadata.shaper != QStringLiteral("log2_1p")) ||
            customLutBytes.size() != count * 4)
            return false;
        customValues.resize(count);
        const auto *source = reinterpret_cast<const uchar *>(customLutBytes.constData());
        for (qsizetype i = 0; i < count; ++i) {
            const quint32 bits = qFromLittleEndian<quint32>(source + i * 4);
            std::memcpy(&customValues[i], &bits, sizeof(bits));
            if (!std::isfinite(customValues[i])) return false;
        }
    }
    const QVector<float> &builtinLut = colorTransform == 2
        ? workbenchPbrNeutralDisplayLutData()
        : colorTransform == 3 ? workbenchFilmicDisplayLutData()
                             : workbenchAgxDisplayLutData();
    const QVector<float> &lut = colorTransform == 4 ? customValues : builtinLut;
    if (lut.size() != edge * edge * edge * 3) {
        return false;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &previousBinding);
    if (agxDisplayTexture_ != 0 && agxDisplayTextureTransform_ != 4)
        glDeleteTextures(1, &agxDisplayTexture_);
    glGenTextures(1, &agxDisplayTexture_);
    glBindTexture(GL_TEXTURE_3D, agxDisplayTexture_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F,
                 edge, edge, edge, 0, GL_RGB, GL_FLOAT,
                 lut.constData());
    agxDisplayTextureTransform_ = colorTransform;
    agxDisplayTextureCustomId_ = colorTransform == 4 ? customKey : QString{};
    if (colorTransform == 4)
        customDisplayTextures_.insert(customKey, agxDisplayTexture_);
    glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(previousBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    return agxDisplayTexture_ != 0;
}

bool ViewportSurfaceRenderer::ensureCavityJitterTexture(int totalSamples)
{
    const int sampleCount = std::clamp(totalSamples, 1,
                                       kWorkbenchCavityMaxSamples);
    if (cavityJitterTexture_ != 0 &&
        cavityJitterSampleCount_ == sampleCount) {
        return true;
    }

    const QByteArray blueNoiseRg = QByteArray::fromBase64(QByteArray(
        detail::kWorkbenchCavityBlueNoiseRgBase64,
        static_cast<qsizetype>(
            sizeof(detail::kWorkbenchCavityBlueNoiseRgBase64) - 1)));
    constexpr int kJitterTextureSize = 64;
    constexpr int kBlueNoiseChannelCount = 2;
    constexpr int kExpectedBlueNoiseBytes =
        kJitterTextureSize * kJitterTextureSize * kBlueNoiseChannelCount;
    if (blueNoiseRg.size() != kExpectedBlueNoiseBytes) {
        return false;
    }

    // Blender's LUT channels are quantized to 1/256 steps. The texture is
    // rebuilt when the total Workbench cavity sample count changes because
    // its blue-noise radius offset is scaled by 1 / total_samples.
    std::array<float, kJitterTextureSize * kJitterTextureSize * 3> pixels{};
    constexpr float kInverseBlueNoiseRange = 1.0f / 256.0f;
    constexpr double kPi = 3.14159265358979323846;
    const float inverseSampleCount = 1.0f / static_cast<float>(sampleCount);
    for (int pixelIndex = 0;
         pixelIndex < kJitterTextureSize * kJitterTextureSize;
         ++pixelIndex) {
        const int sourceIndex = pixelIndex * kBlueNoiseChannelCount;
        const float angleSample = static_cast<float>(
            static_cast<unsigned char>(blueNoiseRg.at(sourceIndex))) *
            kInverseBlueNoiseRange;
        const float radiusSample = static_cast<float>(
            static_cast<unsigned char>(blueNoiseRg.at(sourceIndex + 1))) *
            kInverseBlueNoiseRange;
        const double angle = static_cast<double>(angleSample * 2.0f) * kPi;
        const int destinationIndex = pixelIndex * 3;
        pixels[destinationIndex] = static_cast<float>(std::cos(angle));
        pixels[destinationIndex + 1] = static_cast<float>(std::sin(angle));
        pixels[destinationIndex + 2] =
            std::clamp(radiusSample - 0.5f, -0.499f, 0.499f) *
            inverseSampleCount;
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTextureBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding);
    if (cavityJitterTexture_ == 0) {
        glGenTextures(1, &cavityJitterTexture_);
    }
    if (cavityJitterTexture_ == 0) {
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousTextureBinding));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, cavityJitterTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F,
                 kJitterTextureSize, kJitterTextureSize, 0, GL_RGB, GL_FLOAT,
                 pixels.data());
    glBindTexture(GL_TEXTURE_2D,
                  static_cast<GLuint>(previousTextureBinding));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    cavityJitterSampleCount_ = sampleCount;
    return true;
}

bool ViewportSurfaceRenderer::draw(
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    const ViewportShadingSettings &settings,
    bool previewOverlay,
    bool clearDepth,
    bool selectionOverlay,
    const ViewportRenderTargets *sharedTargets,
    ViewportRenderPassReport *passReport,
    bool sceneLinearOutput)
{
    if (passReport != nullptr) *passReport = {};
    if (QOpenGLContext::currentContext() == nullptr || viewportSize.isEmpty() ||
        !initialize()) {
        return false;
    }

    const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
    GLboolean previousDepthMask = GL_TRUE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint previousDepthFunction = GL_LESS;
    GLint previousBlendSourceRgb = GL_ONE;
    GLint previousBlendDestinationRgb = GL_ZERO;
    GLint previousBlendSourceAlpha = GL_ONE;
    GLint previousBlendDestinationAlpha = GL_ZERO;
    GLint previousBlendEquationRgb = GL_FUNC_ADD;
    GLint previousBlendEquationAlpha = GL_FUNC_ADD;
    GLint previousProgram = 0;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
    glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSourceRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDestinationRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSourceAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDestinationAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);

    if (clearDepth) {
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    prepareGeometry(objects);
    QVector<int> opaqueRangeIndices;
    QVector<int> frontOpaqueRangeIndices;
    QVector<int> shadowVolumeRangeIndices;
    bool hasPartialAlpha = false;
    bool hasFrontTransparentRanges = false;
    bool transparentDepthCoverageReady = false;
    if (settings.mode == ViewportShadingMode::Solid && !previewOverlay &&
        !selectionOverlay && !settings.xrayEnabled()) {
        for (int index = 0; index < ranges_.size(); ++index) {
            const DrawRange &range = ranges_[index];
            if (range.count <= 0) continue;
            if (range.shadowVolumeSupported &&
                !range.shadowVolumePolygonMesh) {
                shadowVolumeRangeIndices.append(index);
            }
            const qreal alpha = workbenchSourceAlpha(
                settings.colorMode, range.displayColorSceneLinear.w(),
                range.workbenchMaterial, settings.customColor);
            const float packedAlpha = workbenchPackUnitFloatToUnorm8(
                static_cast<float>(alpha));
            if (workbenchEffectiveAlpha(packedAlpha, settings, false) >=
                1.0 - 1.0e-6) {
                if (range.showInFront && sharedTargets != nullptr &&
                    sharedTargets->frontFramebuffer != 0) {
                    frontOpaqueRangeIndices.append(index);
                } else {
                    opaqueRangeIndices.append(index);
                }
            } else {
                hasPartialAlpha = true;
            }
        }
        // Imported meshes are split into one color range per face. Admit the
        // shared indexed volume only when every range for that object is
        // opaque in the current Workbench color/alpha mode. Otherwise restore
        // every face to the normal fallback-caster set.
        for (int index = 0; index < ranges_.size(); ++index) {
            const DrawRange &candidate = ranges_[index];
            if (!candidate.shadowVolumeSupported ||
                !candidate.shadowVolumePolygonMesh ||
                candidate.shadowTriangles.isEmpty()) {
                continue;
            }
            bool allObjectRangesOpaque = true;
            for (int rangeIndex = 0; rangeIndex < ranges_.size(); ++rangeIndex) {
                const DrawRange &objectRange = ranges_[rangeIndex];
                if (objectRange.shadowVolumePolygonMesh &&
                    objectRange.objectSeed == candidate.objectSeed &&
                    (objectRange.count <= 0 ||
                     (!opaqueRangeIndices.contains(rangeIndex) &&
                      !frontOpaqueRangeIndices.contains(rangeIndex)))) {
                    allObjectRangesOpaque = false;
                    break;
                }
            }
            if (allObjectRangesOpaque) {
                shadowVolumeRangeIndices.append(index);
            } else {
                const quint64 objectSeed = candidate.objectSeed;
                for (DrawRange &objectRange : ranges_) {
                    if (objectRange.shadowVolumePolygonMesh &&
                        objectRange.objectSeed == objectSeed) {
                        objectRange.shadowVolumeSupported = false;
                    }
                }
            }
        }
    }
    const bool primarySceneEffectsDraw =
        settings.mode == ViewportShadingMode::Solid && !previewOverlay &&
        !selectionOverlay && clearDepth && !settings.xrayEnabled() &&
        !vertices_.isEmpty();
    if (settings.mode == ViewportShadingMode::Solid &&
        settings.backfaceCulling && !settings.xrayEnabled()) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (selectionOverlay) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else if (settings.mode == ViewportShadingMode::Wireframe &&
        !settings.xrayEnabled()) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else if (settings.mode == ViewportShadingMode::Solid) {
        const bool xray = previewOverlay || settings.xrayEnabled();
        if (xray) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else if (hasPartialAlpha) {
            // The surface-range draw below renders opaque ranges first and
            // blends transparent ranges back-to-front against opaque depth.
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
    }

    // Solid X-Ray keeps the faces translucent, but their depth still defines
    // which scene strokes are behind a surface. Without this depth-only pass,
    // the later stroke pass draws back edges through every face.
    const bool xraySurfaceDepthAvailable =
        settings.mode == ViewportShadingMode::Solid &&
        settings.xrayEnabled() && !previewOverlay && !vertices_.isEmpty();
    if (xraySurfaceDepthAvailable) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        glViewport(0, 0, qRound(viewportSize.width() * dpr),
                   qRound(viewportSize.height() * dpr));
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        vertexArray_.bind();
        vertexBuffer_.bind();
        if (geometryDirty_) {
            vertexBuffer_.allocate(vertices_.constData(),
                                   vertices_.size() *
                                       static_cast<int>(sizeof(SurfaceVertex)));
            geometryDirty_ = false;
        }
        shadowProgram_.bind();
        shadowProgram_.setUniformValue(
            "uShadowViewProjection",
            viewportViewProjection(transform, viewportSize));
        shadowProgram_.setUniformValue("uUseShadowAlphaMask", false);
        for (const DrawRange &range : ranges_) {
            if (range.count <= 0) {
                continue;
            }
            shadowProgram_.setUniformValue(
                "uWorldOffset",
                QVector3D(static_cast<float>(range.offset.x),
                          static_cast<float>(range.offset.y),
                          static_cast<float>(range.offset.z)));
            glDrawArrays(GL_TRIANGLES, range.first, range.count);
        }
        shadowProgram_.release();
        vertexBuffer_.release();
        vertexArray_.release();

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
    }

    const bool cavityRequested = primarySceneEffectsDraw && settings.cavity;
    const bool outlineRequested = primarySceneEffectsDraw && settings.outline;
    const bool worldSsaoRequested = cavityRequested &&
        (settings.cavityType == ViewportCavityType::World ||
         settings.cavityType == ViewportCavityType::Both);
    const int aaSampleCount = sharedTargets != nullptr
                                  ? std::clamp(sharedTargets->sampleCount, 1, 32)
                                  : 1;
    const int cavitySamplesPerIteration =
        std::clamp(settings.cavitySamples, 1, 500);
    const int cavityTotalSampleCount = std::min(
        aaSampleCount * cavitySamplesPerIteration,
        kWorkbenchCavityMaxSamples);
    const bool ssaoJitterReady =
        worldSsaoRequested &&
        ensureCavityJitterTexture(cavityTotalSampleCount);
    const bool ssaoEnabled = worldSsaoRequested && ssaoJitterReady;
    const bool sharedPrepassRequested = sharedTargets != nullptr &&
        (cavityRequested || outlineRequested ||
         !frontOpaqueRangeIndices.isEmpty());
    bool sharedPrepassReady = false;
    bool cavityDepthReady = false;
    bool frontDomainReady = frontOpaqueRangeIndices.isEmpty();
    if (sharedPrepassRequested) {
        bool snapshotDepthReady = !ssaoEnabled;
        sharedPrepassReady = renderWorkbenchEffectPrepass(
            transform, viewportSize, devicePixelRatio, settings,
            *sharedTargets, opaqueRangeIndices, ssaoEnabled,
            &snapshotDepthReady);
        bool frontPrepassReady = false;
        if (sharedPrepassReady && !frontOpaqueRangeIndices.isEmpty()) {
            ViewportRenderTargets frontTargets = *sharedTargets;
            frontTargets.depthTexture = sharedTargets->frontDepthTexture;
            frontTargets.viewSpaceNormalTexture =
                sharedTargets->frontViewSpaceNormalTexture;
            frontTargets.objectIdTexture = sharedTargets->frontObjectIdTexture;
            frontTargets.frontOutlineIdTexture =
                sharedTargets->frontOutlineIdTexture;
            frontPrepassReady = renderWorkbenchEffectPrepass(
                transform, viewportSize, devicePixelRatio, settings,
                frontTargets, frontOpaqueRangeIndices, false, nullptr);
            if (frontPrepassReady) {
                frontPrepassReady = mergeWorkbenchFrontAttachments(
                    *sharedTargets, false);
                if (ssaoEnabled) {
                    const QSize frontPixelSize = sharedTargets->pixelSize;
                    GLint previousRead = 0;
                    GLint previousDraw = 0;
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                                      frontMergeFramebuffer_);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                                      cavityDepthFramebuffer_);
                    glBlitFramebuffer(0, 0, frontPixelSize.width(),
                                      frontPixelSize.height(), 0, 0,
                                      frontPixelSize.width(),
                                      frontPixelSize.height(),
                                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                                      static_cast<GLuint>(previousRead));
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                                      static_cast<GLuint>(previousDraw));
                    snapshotDepthReady = true;
                }
            }
            frontDomainReady = frontPrepassReady;
        }
        sharedPrepassReady = sharedPrepassReady &&
            (frontOpaqueRangeIndices.isEmpty() || frontPrepassReady);
        cavityDepthReady = sharedPrepassReady && ssaoEnabled &&
                           snapshotDepthReady;
        if (sharedPrepassReady && passReport != nullptr) {
            passReport->populatedAttachments |=
                ViewportRenderTargets::Depth |
                ViewportRenderTargets::ViewSpaceNormal |
                ViewportRenderTargets::ObjectId;
        }
        if (frontDomainReady && passReport != nullptr) {
            passReport->populatedAttachments |=
                ViewportRenderTargets::FrontDepth |
                ViewportRenderTargets::FrontViewSpaceNormal |
                ViewportRenderTargets::FrontObjectId |
                ViewportRenderTargets::FrontOutlineId;
        }
    }
    if (sharedPrepassReady) {
        // The prepass has already filled the shared depth attachment. The
        // color pass redraws the same visible samples into the scene target.
        glDepthFunc(GL_LEQUAL);
    }

    const bool shadowMapRequested =
        settings.mode == ViewportShadingMode::Solid && settings.shadows &&
        !settings.xrayEnabled() && !previewOverlay && !selectionOverlay &&
        clearDepth;
    bool shadowVolumesAvailable = false;
    bool frontShadowVolumesAvailable = false;
    bool sceneDepthPrepassReady = sharedPrepassReady;
    if (shadowMapRequested && !shadowVolumeRangeIndices.isEmpty()) {
        GLint stencilBits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
        if (stencilBits >= 8) {
            if (!sceneDepthPrepassReady) {
                GLint previousViewport[4] = {};
                GLint previousDepthFunction = GL_LESS;
                GLint previousProgram = 0;
                GLboolean previousColorMask[4] = {
                    GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
                GLboolean previousDepthMask = GL_TRUE;
                const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
                const GLboolean previousBlend = glIsEnabled(GL_BLEND);
                const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
                glGetIntegerv(GL_VIEWPORT, previousViewport);
                glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
                glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
                glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
                glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
                const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
                glViewport(0, 0, qRound(viewportSize.width() * dpr),
                           qRound(viewportSize.height() * dpr));
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                if (settings.backfaceCulling) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                } else {
                    glDisable(GL_CULL_FACE);
                }
                vertexArray_.bind();
                vertexBuffer_.bind();
                if (geometryDirty_) {
                    vertexBuffer_.allocate(
                        vertices_.constData(),
                        vertices_.size() *
                            static_cast<int>(sizeof(SurfaceVertex)));
                    geometryDirty_ = false;
                }
                shadowProgram_.bind();
                shadowProgram_.setUniformValue(
                    "uShadowViewProjection",
                    viewportViewProjection(transform, viewportSize));
                shadowProgram_.setUniformValue("uUseShadowAlphaMask", false);
                for (const int rangeIndex : opaqueRangeIndices) {
                    const DrawRange &range = ranges_[rangeIndex];
                    shadowProgram_.setUniformValue(
                        "uWorldOffset",
                        QVector3D(static_cast<float>(range.offset.x),
                                  static_cast<float>(range.offset.y),
                                  static_cast<float>(range.offset.z)));
                    glDrawArrays(GL_TRIANGLES, range.first, range.count);
                }
                shadowProgram_.release();
                vertexBuffer_.release();
                vertexArray_.release();
                glViewport(previousViewport[0], previousViewport[1],
                           previousViewport[2], previousViewport[3]);
                glColorMask(previousColorMask[0], previousColorMask[1],
                            previousColorMask[2], previousColorMask[3]);
                glDepthMask(previousDepthMask);
                glDepthFunc(static_cast<GLenum>(previousDepthFunction));
                if (previousDepthTest) glEnable(GL_DEPTH_TEST);
                else glDisable(GL_DEPTH_TEST);
                if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
                if (previousCullFace) glEnable(GL_CULL_FACE);
                else glDisable(GL_CULL_FACE);
                glUseProgram(static_cast<GLuint>(previousProgram));
                sceneDepthPrepassReady = true;
            }

            if (sceneDepthPrepassReady) {
                const QMatrix4x4 shadowViewProjection =
                    viewportViewProjection(transform, viewportSize);
                shadowVolumesAvailable = renderShadowVolumes(
                    workbenchShadowDirectionWorld(settings),
                    shadowViewProjection, viewportSize, devicePixelRatio,
                    shadowVolumeRangeIndices, opaqueRangeIndices);
                if (shadowVolumesAvailable) {
                    glDepthFunc(GL_LEQUAL);
                }
            }
        }
    }

    if (shadowMapRequested && sharedTargets != nullptr &&
        sharedTargets->frontFramebuffer != 0 &&
        !frontOpaqueRangeIndices.isEmpty() &&
        !shadowVolumeRangeIndices.isEmpty() && frontDomainReady) {
        GLint previousDrawFramebuffer = 0;
        GLint previousReadFramebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, sharedTargets->frontFramebuffer);
        GLint frontStencilBits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &frontStencilBits);
        if (frontStencilBits >= 8) {
            QVector<int> frontOpaqueCasters = opaqueRangeIndices;
            frontOpaqueCasters += frontOpaqueRangeIndices;
            frontShadowVolumesAvailable = renderShadowVolumes(
                workbenchShadowDirectionWorld(settings),
                viewportViewProjection(transform, viewportSize), viewportSize,
                devicePixelRatio, shadowVolumeRangeIndices,
                frontOpaqueCasters);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(previousDrawFramebuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(previousReadFramebuffer));
    }

    bool shadowMapAvailable = false;
    if (shadowMapRequested) {
        // The volume pass already covers every supported caster, including
        // transparent ones. In mixed-alpha scenes the map is only for ranges
        // whose topology could not enter that pass.
        const bool fallbackOnly = shadowVolumesAvailable &&
            (frontOpaqueRangeIndices.isEmpty() ||
             frontShadowVolumesAvailable);
        const bool hasFallbackCasters = std::any_of(
            ranges_.cbegin(), ranges_.cend(),
            [fallbackOnly](const DrawRange &range) {
                return range.count > 0 &&
                       (!fallbackOnly || !range.shadowVolumeSupported);
            });
        if (hasFallbackCasters) {
            shadowMapAvailable = renderShadowMap(
                transform, viewportSize, settings, fallbackOnly);
        }
    }
    const bool useShadows = shadowVolumesAvailable ||
                            frontShadowVolumesAvailable || shadowMapAvailable;
    if (sceneDepthPrepassReady) {
        glDepthFunc(GL_LEQUAL);
    }

    const bool drawColor = settings.mode == ViewportShadingMode::Solid;
    if (!vertices_.isEmpty() &&
        (drawColor || !settings.xrayEnabled())) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        glViewport(0, 0, qRound(viewportSize.width() * dpr),
                   qRound(viewportSize.height() * dpr));
        const QMatrix4x4 viewProjection =
            viewportViewProjection(transform, viewportSize);
        program_.bind();
        program_.setUniformValue("uViewProjection", viewProjection);
        const Point3D viewDirection =
            normalized(transform.viewDirection(), {0.0, 0.0, 1.0});
        const Point3D viewUp = normalized(transform.viewUp(), {0.0, 1.0, 0.0});
        const Point3D viewRight = normalized(cross(viewUp, viewDirection),
                                             {1.0, 0.0, 0.0});
        const Point3D viewFacing = viewDirection;
        const Point3D cameraPosition = transform.cameraPosition(viewportSize);
        float orthographicTransparencyDepthOffset = 0.0f;
        if (!transform.isPerspectiveEnabled()) {
            const ViewportCameraState camera = transform.cameraState();
            const ViewportCameraPreferences cameraPreferences =
                transform.cameraPreferences();
            const qreal orthographicEyeDistance = std::min(
                cameraPreferences.clipEnd,
                std::max<qreal>(1000.0, camera.gridViewDistance));
            orthographicTransparencyDepthOffset = static_cast<float>(
                orthographicEyeDistance - cameraPreferences.clipStart);
        }
        program_.setUniformValue("uViewRight", asVector(viewRight));
        program_.setUniformValue("uViewUp", asVector(viewUp));
        program_.setUniformValue("uViewFacing", asVector(viewFacing));
        program_.setUniformValue("uCameraPosition", asVector(cameraPosition));
        program_.setUniformValue("uOrthographicTransparencyDepthOffset",
                                 orthographicTransparencyDepthOffset);
        program_.setUniformValue("uPerspective",
                                 transform.isPerspectiveEnabled());
        const WorkbenchStudioLighting &lighting =
            workbenchStudioLightingPreset(settings.studioLightPreset);
        const WorkbenchStudioLighting viewLighting =
            workbenchStudioLightingForView(
                lighting, settings.studioLightRotationDegrees,
                settings.worldSpaceLighting, asVector(
                    Point3D{viewRight.x, viewRight.y, viewRight.z}),
                asVector(Point3D{viewUp.x, viewUp.y, viewUp.z}),
                asVector(Point3D{viewFacing.x, viewFacing.y, viewFacing.z}));
        QVector3D lightDirections[4];
        QVector3D lightDiffuse[4];
        QVector3D lightSpecular[4];
        float lightWrap[4];
        for (int index = 0; index < 4; ++index) {
            const WorkbenchStudioLight &light = viewLighting.lights[index];
            lightDirections[index] = light.enabled
                                         ? light.direction
                                         : QVector3D(1.0f, 0.0f, 0.0f);
            lightDiffuse[index] = light.enabled
                                      ? light.diffuseColor
                                      : QVector3D();
            lightSpecular[index] = light.enabled
                                       ? light.specularColor
                                       : QVector3D();
            lightWrap[index] = light.enabled ? light.wrap : 0.0f;
        }
        program_.setUniformValueArray("uLightDirection", lightDirections, 4);
        program_.setUniformValueArray("uLightDiffuse", lightDiffuse, 4);
        program_.setUniformValueArray("uLightSpecular", lightSpecular, 4);
        program_.setUniformValueArray("uLightWrap[0]", lightWrap, 4, 1);
        program_.setUniformValue("uAmbientColor", lighting.ambientColor);
        program_.setUniformValue("uUseSpecular",
                                 settings.specularLighting &&
                                     lighting.useSpecular);
        const QVector3D shadowDirectionWorld =
            workbenchShadowDirectionWorld(settings);
        const QVector3D shadowDirectionView(
            QVector3D::dotProduct(shadowDirectionWorld,
                                  asVector(viewRight)),
            QVector3D::dotProduct(shadowDirectionWorld,
                                  asVector(viewUp)),
            QVector3D::dotProduct(shadowDirectionWorld,
                                  asVector(viewFacing)));
        const int lightingMode = settings.lightingMode == ViewportLightingMode::Studio
                                     ? 0
                                     : settings.lightingMode == ViewportLightingMode::MatCap
                                           ? 1
                                           : 2;
        program_.setUniformValue("uLightingMode", lightingMode);
        program_.setUniformValue("uMatcapFlipX", settings.matcapFlipX);
        const bool matcapAvailable =
            settings.lightingMode == ViewportLightingMode::MatCap &&
            ensureMatcapTexture(settings.matcapPreset);
        program_.setUniformValue("uMatcapAvailable", matcapAvailable);
        const bool agxDisplayAvailable = settings.colorTransform != 1 &&
            ensureAgxDisplayTexture(settings.colorTransform,
                                    settings.colorLutId,
                                    settings.colorLutData,
                                    settings.colorLutMetadata);
        program_.setUniformValue("uAgxDisplayAvailable", agxDisplayAvailable);
        program_.setUniformValue("uColorTransform",
                                 settings.colorTransform);
        program_.setUniformValue("uAgxDisplayLutSize",
                                 settings.colorTransform == 4
                                     ? settings.colorLutMetadata.size
                                     : workbenchAgxDisplayLutSize);
        program_.setUniformValue(
            "uAgxDisplayLutDomain",
            QVector2D(static_cast<float>(settings.colorTransform == 4
                              ? settings.colorLutMetadata.domainMin : 0.0),
                      static_cast<float>(settings.colorTransform == 4
                              ? settings.colorLutMetadata.domainMax
                              : workbenchAgxDisplayLutMaximum)));
        program_.setUniformValue(
            "uAgxDisplayLutShaper",
            settings.colorTransform == 4 &&
                    settings.colorLutMetadata.shaper == QStringLiteral("log2_1p")
                ? 1 : 0);
        program_.setUniformValue(
            "uAgxDisplayLutShaperScale",
            static_cast<float>(settings.colorTransform == 4
                                   ? settings.colorLutMetadata.shaperScale : 1.0));
        program_.setUniformValue("uColorExposure",
                                 static_cast<float>(settings.colorExposure));
        program_.setUniformValue("uColorGamma",
                                 static_cast<float>(settings.colorGamma));
        program_.setUniformValue("uSceneLinearOutput", sceneLinearOutput);
        program_.setUniformValue("uUseShadows", useShadows);
        program_.setUniformValue("uUseShadowMap", shadowMapAvailable);
        program_.setUniformValue("uShadowVolumeShadowed", false);
        program_.setUniformValue("uShadowIntensity",
                                 static_cast<float>(settings.shadowIntensity));
        program_.setUniformValue("uShadowBias", 0.0005f);
        program_.setUniformValue("uShadowFocus",
                                 static_cast<float>(settings.shadowFocus));
        program_.setUniformValue("uShadowShift",
                                 static_cast<float>(std::clamp(
                                     settings.shadowOffset, 0.0, 1.0)));
        program_.setUniformValue("uShadowDirectionView",
                                 shadowDirectionView);
        program_.setUniformValue("uShadowViewProjection",
                                 shadowViewProjection_);
        const bool cavityAvailable = sharedPrepassReady && cavityRequested;
        const bool screenCurvatureRequested = cavityAvailable &&
            (settings.cavityType == ViewportCavityType::Screen ||
             settings.cavityType == ViewportCavityType::Both);
        program_.setUniformValue("uUseCavity", cavityAvailable);
        program_.setUniformValue("uUseSsao",
                                 cavityAvailable && ssaoEnabled &&
                                     cavityDepthReady);
        program_.setUniformValue("uUseCurvature",
                                 screenCurvatureRequested);
        QMatrix4x4 inverseViewProjection;
        if (sharedTargets != nullptr) {
            inverseViewProjection = sharedTargets->inverseViewProjection;
        }
        program_.setUniformValue("uInverseViewProjection",
                                 inverseViewProjection);
        const qreal viewScale = std::max<qreal>(
            transform.viewScalePixelsPerWorldUnit(viewportSize), 1.0e-8);
        qreal projectionScale = viewScale;
        if (transform.isPerspectiveEnabled()) {
            const Point3D target = transform.viewTarget();
            const qreal cameraDistance = std::hypot(
                std::hypot(cameraPosition.x - target.x,
                           cameraPosition.y - target.y),
                cameraPosition.z - target.z);
            projectionScale *= cameraDistance;
        }
        program_.setUniformValue(
            "uCavityProjectionScale",
            QVector2D(static_cast<float>(2.0 * projectionScale /
                                         viewportSize.width()),
                      static_cast<float>(2.0 * projectionScale /
                                         viewportSize.height())));
        program_.setUniformValue("uCavityUiScale", static_cast<float>(dpr));
        program_.setUniformValue("uCavityDistance",
                                 static_cast<float>(settings.cavityDistance));
        program_.setUniformValue("uCavityAttenuation",
                                 static_cast<float>(settings.cavityAttenuation));
        program_.setUniformValue("uCavityValleyFactor",
                                 static_cast<float>(settings.cavityValleyFactor));
        program_.setUniformValue("uCavityRidgeFactor",
                                 static_cast<float>(settings.cavityRidgeFactor));
        program_.setUniformValue(
            "uCurvatureRidgeControl",
            0.5f / std::max(static_cast<float>(settings.curvatureRidgeFactor) *
                                static_cast<float>(settings.curvatureRidgeFactor),
                            1.0e-4f));
        program_.setUniformValue(
            "uCurvatureValleyControl",
            0.7f / std::max(static_cast<float>(settings.curvatureValleyFactor) *
                                static_cast<float>(settings.curvatureValleyFactor),
                            1.0e-4f));
        const int cavityIterationCount = std::max(
            1, cavityTotalSampleCount / cavitySamplesPerIteration);
        const int cavityIteration = sharedTargets != nullptr
            ? std::max(0, sharedTargets->sampleIndex) % cavityIterationCount
            : 0;
        const int cavitySampleStart = cavityIteration *
                                      cavitySamplesPerIteration;
        const int cavitySampleEnd = cavitySampleStart +
                                    cavitySamplesPerIteration;
        program_.setUniformValue("uCavitySampleStart", cavitySampleStart);
        program_.setUniformValue("uCavitySampleEnd", cavitySampleEnd);
        program_.setUniformValue("uCavitySamplesPerIteration",
                                 cavitySamplesPerIteration);
        program_.setUniformValue(
            "uCavitySampleCountInv",
            1.0f / static_cast<float>(cavitySampleEnd - cavitySampleStart));
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousMatcapTextureBinding = 0;
        GLint previousAgxTextureBinding = 0;
        GLint previousShadowTextureBinding = 0;
        GLint previousCavityNormalTextureBinding = 0;
        GLint previousCavityObjectIdTextureBinding = 0;
        GLint previousCavityDepthTextureBinding = 0;
        GLint previousCavityJitterTextureBinding = 0;
        GLint previousImageTextureBinding = 0;
        GLint previousUdimArrayTextureBinding = 0;
        GLint previousUdimLookupTextureBinding = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        if (matcapAvailable) {
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,
                          &previousMatcapTextureBinding);
            glBindTexture(GL_TEXTURE_2D_ARRAY, matcapTexture_);
            program_.setUniformValue("uMatcapTexture", 0);
        }
        glActiveTexture(GL_TEXTURE1);
        if (agxDisplayAvailable) {
            glGetIntegerv(GL_TEXTURE_BINDING_3D,
                          &previousAgxTextureBinding);
            glBindTexture(GL_TEXTURE_3D, agxDisplayTexture_);
            program_.setUniformValue("uAgxDisplayTexture", 1);
        }
        glActiveTexture(GL_TEXTURE2);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousShadowTextureBinding);
        if (shadowMapAvailable) {
            glBindTexture(GL_TEXTURE_2D, shadowTexture_);
        }
        program_.setUniformValue("uShadowMap", 2);
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityNormalTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityAvailable && sharedTargets != nullptr
                          ? sharedTargets->viewSpaceNormalTexture
                          : 0);
        program_.setUniformValue("uCavityNormal", 3);
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityObjectIdTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityAvailable && sharedTargets != nullptr
                          ? sharedTargets->objectIdTexture
                          : 0);
        program_.setUniformValue("uCavityObjectId", 4);
        glActiveTexture(GL_TEXTURE5);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityDepthTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityDepthReady ? cavityDepthTexture_ : 0);
        program_.setUniformValue("uCavityDepth", 5);
        glActiveTexture(GL_TEXTURE6);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousImageTextureBinding);
        program_.setUniformValue("uWorkbenchImage", 6);
        program_.setUniformValue("uWorkbenchUdimImage", 8);
        program_.setUniformValue("uWorkbenchUdimLookup", 9);
        glActiveTexture(GL_TEXTURE7);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousCavityJitterTextureBinding);
        glBindTexture(GL_TEXTURE_2D,
                      cavityAvailable && ssaoEnabled
                          ? cavityJitterTexture_
                          : 0);
        program_.setUniformValue("uCavityJitterTexture", 7);
        glActiveTexture(GL_TEXTURE8);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,
                      &previousUdimArrayTextureBinding);
        glActiveTexture(GL_TEXTURE9);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,
                      &previousUdimLookupTextureBinding);
        // Blender 5.2's 3D View face_select color is #FFA300 with alpha 0x33.
        const QColor editSelectionColor(QStringLiteral("#ffa300"));
        // Theme colors are sRGB; Blender's overlay framebuffer blends in
        // linear display space after the view transform.
        const QVector3D editSelectionLinear = workbenchSrgbToSceneLinear(
            QVector3D(editSelectionColor.redF(), editSelectionColor.greenF(),
                      editSelectionColor.blueF()));
        program_.setUniformValue(
            "uEditSelectionColor",
            editSelectionLinear);
        program_.setUniformValue("uEditSelectionMix", 51.0f / 255.0f);
        vertexArray_.bind();
        vertexBuffer_.bind();
        if (geometryDirty_) {
            vertexBuffer_.allocate(vertices_.constData(),
                                   vertices_.size() *
                                       static_cast<int>(sizeof(SurfaceVertex)));
            geometryDirty_ = false;
        }
        struct TransparentRange {
            int rangeIndex = -1;
            qreal projectedDepth = 0.0;
        };
        QVector<int> transparentRangeIndices;
        QVector<int> frontTransparentRangeIndices;
        QVector<TransparentRange> fallbackTransparentRanges;
        for (int index = 0; index < ranges_.size(); ++index) {
            const DrawRange &range = ranges_[index];
            if (range.count <= 0) {
                continue;
            }
            if (opaqueRangeIndices.contains(index) ||
                frontOpaqueRangeIndices.contains(index)) {
                continue;
            }
            if (range.showInFront) {
                hasFrontTransparentRanges = true;
                if (sharedTargets != nullptr &&
                    sharedTargets->frontDepthTexture != 0) {
                    frontTransparentRangeIndices.append(index);
                } else {
                    transparentRangeIndices.append(index);
                }
            } else {
                transparentRangeIndices.append(index);
            }
            // The shared AA scene framebuffer supplies depth-stencil for the
            // weighted OIT pass. Keep a coarse object-order fallback for
            // auxiliary draws that have no shareable depth texture.
            QVector4D center;
            for (int vertexIndex = range.first;
                 vertexIndex < range.first + range.count; ++vertexIndex) {
                const SurfaceVertex &vertex = vertices_[vertexIndex];
                center += QVector4D(vertex.x, vertex.y, vertex.z, 1.0f);
            }
            center /= static_cast<float>(range.count);
            center.setX(center.x() + static_cast<float>(range.offset.x));
            center.setY(center.y() + static_cast<float>(range.offset.y));
            center.setZ(center.z() + static_cast<float>(range.offset.z));
            const QVector4D clip = viewProjection * center;
            const qreal depth = std::abs(clip.w()) > 1.0e-8f
                ? static_cast<qreal>(clip.z() / clip.w())
                : static_cast<qreal>(clip.z());
            fallbackTransparentRanges.append(
                {index, std::isfinite(depth) ? depth : 0.0});
        }
        std::stable_sort(fallbackTransparentRanges.begin(),
                         fallbackTransparentRanges.end(),
                         [](const TransparentRange &left,
                            const TransparentRange &right) {
                             if (left.projectedDepth !=
                                 right.projectedDepth) {
                                 return left.projectedDepth >
                                        right.projectedDepth;
                             }
                             return left.rangeIndex < right.rangeIndex;
                         });
        const QSize transparencyPixelSize(
            qRound(viewportSize.width() * dpr),
            qRound(viewportSize.height() * dpr));
        const bool weightedOitAvailable = hasPartialAlpha &&
            sharedTargets != nullptr &&
            sharedTargets->pixelSize == transparencyPixelSize &&
            ensureTransparencyFramebuffer(transparencyPixelSize,
                                          sharedTargets->depthTexture);
        const bool frontWeightedOitAvailable = weightedOitAvailable &&
            hasPartialAlpha &&
            !frontTransparentRangeIndices.isEmpty() && sharedTargets != nullptr &&
            sharedTargets->pixelSize == transparencyPixelSize &&
            ensureTransparencyFramebuffer(transparencyPixelSize,
                                          sharedTargets->frontDepthTexture,
                                          true);
        if (!frontWeightedOitAvailable) {
            // Preserve the established regular OIT/sorted fallback if a
            // separate front-domain accumulation target cannot be created.
            transparentRangeIndices += frontTransparentRangeIndices;
            frontTransparentRangeIndices.clear();
        }

        const auto drawRange = [&](const DrawRange &range,
                                   bool volumeShadowed,
                                   bool enableShadows,
                                   int firstVertex,
                                   int vertexCount,
                                   bool transparencyAccumulation = false) {
            program_.setUniformValue("uShadowVolumeShadowed",
                                     volumeShadowed);
            program_.setUniformValue("uUseShadows",
                                     useShadows && enableShadows);
            program_.setUniformValue("uUseShadowMap",
                                     shadowMapAvailable && enableShadows);
            program_.setUniformValue(
                "uUseAttributeColor",
                settings.colorMode == ViewportColorMode::Attribute &&
                    range.useAttributeColor);
            const GLuint imageTexture = settings.colorMode == ViewportColorMode::Texture &&
                    range.useTextureColor
                ? ensureImageTexture(range.textureImagePath,
                                     range.workbenchMaterial.textureImagePackedDataBase64,
                                     range.textureImageSrgb) : 0;
            const UdimTextureSet udimTexture =
                settings.colorMode == ViewportColorMode::Texture &&
                        range.useTextureColor
                    ? ensureUdimTexture(range.textureImageTiles,
                                        range.textureImageSrgb)
                    : UdimTextureSet{};
            program_.setUniformValue("uUseTextureColor",
                                     imageTexture != 0 || udimTexture.imageArray != 0);
            program_.setUniformValue("uUseUdimImage",
                                     udimTexture.imageArray != 0);
            program_.setUniformValue(
                "uTextureInterpolation", workbenchTextureInterpolation(
                    range.workbenchMaterial.textureImageInterpolation));
            program_.setUniformValue(
                "uTextureExtension", workbenchTextureExtension(
                    range.workbenchMaterial.textureImageExtension));
            configureWorkbenchImageTexture(imageTexture, range.workbenchMaterial);
            const QString alphaMode = range.workbenchMaterial.textureImageAlphaMode;
            const bool ignoreTextureAlpha =
                alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
                alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
                alphaMode.compare(QStringLiteral("CHANNEL_PACKED"), Qt::CaseInsensitive) == 0;
            program_.setUniformValue("uTexturePremultiplied",
                alphaMode.compare(QStringLiteral("PREMUL"), Qt::CaseInsensitive) == 0);
            program_.setUniformValue("uTextureAlphaCutoff", !ignoreTextureAlpha);
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, imageTexture);
            configureWorkbenchUdimTexture(udimTexture.imageArray,
                                          range.workbenchMaterial);
            glActiveTexture(GL_TEXTURE8);
            glBindTexture(GL_TEXTURE_2D_ARRAY, udimTexture.imageArray);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, udimTexture.tileLookup);
            glActiveTexture(GL_TEXTURE6);
            const QVector3D offset(static_cast<float>(range.offset.x),
                                   static_cast<float>(range.offset.y),
                                   static_cast<float>(range.offset.z));
            program_.setUniformValue("uWorldOffset", offset);
            program_.setUniformValue("uEditModeSelected",
                                     range.selected && !previewOverlay);
            const WorkbenchMaterialData &material = range.workbenchMaterial;
            QVector3D baseColorLinear =
                workbenchDefaultSolidMaterialDiffuseColor();
            float roughness = 0.4f;
            float metallic = 0.0f;
            if (previewOverlay) {
                const QColor previewColor(QStringLiteral("#d89a4e"));
                baseColorLinear = workbenchSrgbToSceneLinear(
                    QVector3D(previewColor.redF(), previewColor.greenF(),
                              previewColor.blueF()));
            } else {
                switch (settings.colorMode) {
                case ViewportColorMode::Material:
                case ViewportColorMode::Attribute:
                case ViewportColorMode::Texture:
                    // Attribute and Texture have no color-attribute, UV,
                    // or image inputs in the scene model yet; Blender
                    // falls back to the Material source for these modes.
                    baseColorLinear = material.baseColorSceneLinear;
                    roughness = static_cast<float>(material.roughness);
                    metallic = static_cast<float>(material.metallic);
                    break;
                case ViewportColorMode::Object:
                    baseColorLinear = QVector3D(
                        range.displayColorSceneLinear.x(),
                        range.displayColorSceneLinear.y(),
                        range.displayColorSceneLinear.z());
                    break;
                case ViewportColorMode::Random: {
                    const QColor randomColor =
                        workbenchRandomColor(range.objectSeed,
                                             range.displayName,
                                             range.libraryPath);
                    baseColorLinear = workbenchRec709ToSceneLinear(
                        QVector3D(randomColor.redF(), randomColor.greenF(),
                                  randomColor.blueF()));
                    break;
                }
                case ViewportColorMode::Custom:
                    baseColorLinear = workbenchSrgbToSceneLinear(
                        QVector3D(settings.customColor.redF(),
                                  settings.customColor.greenF(),
                                  settings.customColor.blueF()));
                    break;
                }
            }
            qreal sourceAlpha = workbenchSourceAlpha(
                settings.colorMode, range.displayColorSceneLinear.w(),
                material, settings.customColor);
            const WorkbenchPackedMaterialData packed =
                workbenchPackMaterialData(metallic, roughness,
                                          static_cast<float>(sourceAlpha));
            roughness = packed.roughness;
            metallic = packed.metallic;
            if (!previewOverlay) {
                sourceAlpha = packed.alpha;
            }
            const float alpha = static_cast<float>(workbenchEffectiveAlpha(
                sourceAlpha, settings, previewOverlay));
            const bool rangeOpaque = alpha >= 1.0f - 1.0e-6f;
            program_.setUniformValue("uFaceAwareNormals",
                                     rangeOpaque &&
                                         !transparencyAccumulation);
            const bool rangeCavity = cavityAvailable && rangeOpaque;
            program_.setUniformValue("uUseCavity", rangeCavity);
            program_.setUniformValue("uUseSsao",
                                     rangeCavity && ssaoEnabled &&
                                         cavityDepthReady);
            program_.setUniformValue("uUseCurvature",
                                     rangeCavity &&
                                         screenCurvatureRequested);
            program_.setUniformValue("uRoughness", roughness);
            program_.setUniformValue("uMetallic", metallic);
            program_.setUniformValue(
                "uBaseColor",
                QVector4D(baseColorLinear.x(), baseColorLinear.y(),
                          baseColorLinear.z(), alpha));
            program_.setUniformValue("uTransparencyAccumulation",
                                     transparencyAccumulation);
            glDrawArrays(GL_TRIANGLES, firstVertex, vertexCount);
        };
        const auto drawTransparentTriangles = [&]() {
            if (!hasPartialAlpha) return;
            if (!weightedOitAvailable) {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                for (const TransparentRange &range : fallbackTransparentRanges) {
                    const DrawRange &draw = ranges_[range.rangeIndex];
                    drawRange(draw, false, false, draw.first, draw.count);
                }
                glDepthFunc(GL_LESS);
                if (passReport != nullptr) {
                    passReport->markPassCompleted(
                        ViewportRenderPass::TransparentColor);
                }
                return;
            }

            GLint previousDrawFramebuffer = 0;
            GLint previousReadFramebuffer = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            GLint previousDepthFunction = GL_LESS;
            GLint previousBlendSourceRgb = GL_ONE;
            GLint previousBlendDestinationRgb = GL_ZERO;
            GLint previousBlendSourceAlpha = GL_ONE;
            GLint previousBlendDestinationAlpha = GL_ZERO;
            GLint previousBlendEquationRgb = GL_FUNC_ADD;
            GLint previousBlendEquationAlpha = GL_FUNC_ADD;
            GLint previousActiveTexture = GL_TEXTURE0;
            GLint previousTexture0 = 0;
            GLint previousTexture1 = 0;
            GLint previousTexture2D3D = 0;
            GLint previousFrontDepthTexture = 0;
            GLboolean previousDepthMask = GL_TRUE;
            GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
            const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
            const GLboolean previousBlend = glIsEnabled(GL_BLEND);
            const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
            const GLboolean previousFramebufferSrgb =
                glIsEnabled(GL_FRAMEBUFFER_SRGB);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,
                          &previousDrawFramebuffer);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,
                          &previousReadFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
            glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSourceRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDestinationRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSourceAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDestinationAlpha);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
            glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
            glActiveTexture(GL_TEXTURE1);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture1);
            glActiveTexture(GL_TEXTURE2);
            glGetIntegerv(GL_TEXTURE_BINDING_3D, &previousTexture2D3D);
            glActiveTexture(GL_TEXTURE10);
            glGetIntegerv(GL_TEXTURE_BINDING_2D,
                          &previousFrontDepthTexture);

            glBindFramebuffer(GL_FRAMEBUFFER, transparencyFramebuffer_);
            glViewport(0, 0, transparencyPixelSize.width(),
                       transparencyPixelSize.height());
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDisable(GL_STENCIL_TEST);
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE,
                                GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            program_.setUniformValue(
                "uExcludeFrontCoverage",
                sharedTargets != nullptr &&
                    sharedTargets->frontDepthTexture != 0 &&
                    !frontOpaqueRangeIndices.isEmpty());
            program_.setUniformValue("uFrontDepthMask", 10);
            glActiveTexture(GL_TEXTURE10);
            glBindTexture(GL_TEXTURE_2D,
                          sharedTargets != nullptr
                              ? sharedTargets->frontDepthTexture : 0);
            glActiveTexture(GL_TEXTURE0);
            const GLfloat clearAccumulation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            const GLfloat clearWeight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 0, clearAccumulation);
            glClearBufferfv(GL_COLOR, 1, clearWeight);
            program_.setUniformValue("uTransparencyAccumulation", true);
            for (const int rangeIndex : transparentRangeIndices) {
                const DrawRange &range = ranges_[rangeIndex];
                drawRange(range, false, false, range.first, range.count, true);
            }
            program_.setUniformValue("uTransparencyAccumulation", false);
            program_.setUniformValue("uExcludeFrontCoverage", false);
            if (passReport != nullptr) {
                passReport->markPassCompleted(
                    ViewportRenderPass::TransparentAccumulation);
            }

            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              static_cast<GLuint>(previousDrawFramebuffer));
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(previousReadFramebuffer));
            glViewport(previousViewport[0], previousViewport[1],
                       previousViewport[2], previousViewport[3]);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_STENCIL_TEST);
            // Composite in the target's active color space. The primary AA
            // scene target is scene-linear; display overlays keep their
            // legacy display-space path.
            glDisable(GL_FRAMEBUFFER_SRGB);
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, transparencyAccumulationTexture_);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, transparencyWeightTexture_);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_3D,
                          agxDisplayAvailable ? agxDisplayTexture_ : 0);
            transparencyResolveProgram_.bind();
            transparencyResolveProgram_.setUniformValue("uAccumulation", 0);
            transparencyResolveProgram_.setUniformValue("uWeight", 1);
            transparencyResolveProgram_.setUniformValue("uAgxDisplayTexture", 2);
            transparencyResolveProgram_.setUniformValue(
                "uAgxDisplayAvailable", agxDisplayAvailable);
            transparencyResolveProgram_.setUniformValue(
                "uAgxDisplayLutSize",
                settings.colorTransform == 4
                    ? settings.colorLutMetadata.size : workbenchAgxDisplayLutSize);
            transparencyResolveProgram_.setUniformValue(
                "uAgxDisplayLutDomain",
                QVector2D(static_cast<float>(settings.colorTransform == 4
                                  ? settings.colorLutMetadata.domainMin : 0.0),
                          static_cast<float>(settings.colorTransform == 4
                                  ? settings.colorLutMetadata.domainMax
                                  : workbenchAgxDisplayLutMaximum)));
            transparencyResolveProgram_.setUniformValue(
                "uAgxDisplayLutShaper",
                settings.colorTransform == 4 &&
                        settings.colorLutMetadata.shaper == QStringLiteral("log2_1p")
                    ? 1 : 0);
            transparencyResolveProgram_.setUniformValue(
                "uAgxDisplayLutShaperScale",
                static_cast<float>(settings.colorTransform == 4
                                       ? settings.colorLutMetadata.shaperScale : 1.0));
            transparencyResolveProgram_.setUniformValue(
                "uColorTransform", settings.colorTransform);
            transparencyResolveProgram_.setUniformValue(
                "uColorExposure", static_cast<float>(settings.colorExposure));
            transparencyResolveProgram_.setUniformValue(
                "uColorGamma", static_cast<float>(settings.colorGamma));
            transparencyResolveProgram_.setUniformValue(
                "uSceneLinearOutput", sceneLinearOutput);
            glActiveTexture(GL_TEXTURE0);
            vertexArray_.bind();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            transparencyResolveProgram_.release();
            if (frontWeightedOitAvailable) {
                // Blender resolves regular transparency first, then performs
                // a second accumulation against depth_in_front and composites
                // that result over the regular scene.
                glBindFramebuffer(GL_FRAMEBUFFER,
                                  frontTransparencyFramebuffer_);
                glViewport(0, 0, transparencyPixelSize.width(),
                           transparencyPixelSize.height());
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_FALSE);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDisable(GL_STENCIL_TEST);
                glEnable(GL_BLEND);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_ONE, GL_ONE,
                                    GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
                program_.setUniformValue("uExcludeFrontCoverage", false);
                glClearBufferfv(GL_COLOR, 0, clearAccumulation);
                glClearBufferfv(GL_COLOR, 1, clearWeight);
                program_.bind();
                program_.setUniformValue("uTransparencyAccumulation", true);
                for (const int rangeIndex : frontTransparentRangeIndices) {
                    const DrawRange &range = ranges_[rangeIndex];
                    drawRange(range, false, false, range.first, range.count,
                              true);
                }
                program_.setUniformValue("uTransparencyAccumulation", false);
                program_.release();
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                                  static_cast<GLuint>(previousDrawFramebuffer));
                glBindFramebuffer(GL_READ_FRAMEBUFFER,
                                  static_cast<GLuint>(previousReadFramebuffer));
                glViewport(previousViewport[0], previousViewport[1],
                           previousViewport[2], previousViewport[3]);
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_FRAMEBUFFER_SRGB);
                glEnable(GL_BLEND);
                glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D,
                              frontTransparencyAccumulationTexture_);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, frontTransparencyWeightTexture_);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_3D,
                              agxDisplayAvailable ? agxDisplayTexture_ : 0);
                transparencyResolveProgram_.bind();
                transparencyResolveProgram_.setUniformValue("uAccumulation", 0);
                transparencyResolveProgram_.setUniformValue("uWeight", 1);
                transparencyResolveProgram_.setUniformValue(
                    "uAgxDisplayTexture", 2);
                transparencyResolveProgram_.setUniformValue(
                    "uAgxDisplayAvailable", agxDisplayAvailable);
                transparencyResolveProgram_.setUniformValue(
                    "uAgxDisplayLutSize",
                    settings.colorTransform == 4
                        ? settings.colorLutMetadata.size
                        : workbenchAgxDisplayLutSize);
                transparencyResolveProgram_.setUniformValue(
                    "uAgxDisplayLutDomain",
                    QVector2D(static_cast<float>(settings.colorTransform == 4
                                      ? settings.colorLutMetadata.domainMin : 0.0),
                              static_cast<float>(settings.colorTransform == 4
                                      ? settings.colorLutMetadata.domainMax
                                      : workbenchAgxDisplayLutMaximum)));
                transparencyResolveProgram_.setUniformValue(
                    "uAgxDisplayLutShaper",
                    settings.colorTransform == 4 &&
                            settings.colorLutMetadata.shaper == QStringLiteral("log2_1p")
                        ? 1 : 0);
                transparencyResolveProgram_.setUniformValue(
                    "uAgxDisplayLutShaperScale",
                    static_cast<float>(settings.colorTransform == 4
                        ? settings.colorLutMetadata.shaperScale : 1.0));
                transparencyResolveProgram_.setUniformValue(
                    "uColorTransform", settings.colorTransform);
                transparencyResolveProgram_.setUniformValue(
                    "uColorExposure", static_cast<float>(settings.colorExposure));
                transparencyResolveProgram_.setUniformValue(
                    "uColorGamma", static_cast<float>(settings.colorGamma));
                transparencyResolveProgram_.setUniformValue(
                    "uSceneLinearOutput", sceneLinearOutput);
                glActiveTexture(GL_TEXTURE0);
                vertexArray_.bind();
                glDrawArrays(GL_TRIANGLES, 0, 3);
                transparencyResolveProgram_.release();
                if (passReport != nullptr) {
                    passReport->markPassCompleted(
                        ViewportRenderPass::FrontTransparentAccumulation);
                    passReport->markPassCompleted(
                        ViewportRenderPass::FrontTransparentResolve);
                    passReport->populatedAttachments |=
                        ViewportRenderTargets::CurrentSampleColor;
                }
            }
            if (passReport != nullptr) {
                passReport->markPassCompleted(
                    ViewportRenderPass::TransparentResolve);
                passReport->markPassCompleted(
                    ViewportRenderPass::TransparentColor);
            }

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,
                          static_cast<GLuint>(previousTexture0));
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D,
                          static_cast<GLuint>(previousTexture1));
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_3D,
                          static_cast<GLuint>(previousTexture2D3D));
            glActiveTexture(GL_TEXTURE10);
            glBindTexture(GL_TEXTURE_2D,
                          static_cast<GLuint>(previousFrontDepthTexture));
            glActiveTexture(static_cast<GLenum>(previousActiveTexture));
            if (previousFramebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB);
            else glDisable(GL_FRAMEBUFFER_SRGB);
            glColorMask(previousColorMask[0], previousColorMask[1],
                        previousColorMask[2], previousColorMask[3]);
            glDepthMask(previousDepthMask);
            glDepthFunc(static_cast<GLenum>(previousDepthFunction));
            if (previousDepthTest) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
            if (previousBlend) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                                    static_cast<GLenum>(previousBlendEquationAlpha));
            glBlendFuncSeparate(static_cast<GLenum>(previousBlendSourceRgb),
                                static_cast<GLenum>(previousBlendDestinationRgb),
                                static_cast<GLenum>(previousBlendSourceAlpha),
                                static_cast<GLenum>(previousBlendDestinationAlpha));
            if (previousStencilTest) glEnable(GL_STENCIL_TEST);
            else glDisable(GL_STENCIL_TEST);
            program_.bind();
        };
        const auto drawSurfaceRanges = [&](bool volumeShadowed,
                                           bool includeTransparent) {
            if (hasPartialAlpha) {
                // Render opaque surfaces first to establish depth, then blend
                // translucent surfaces back-to-front without updating depth.
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                for (int index : opaqueRangeIndices) {
                    const DrawRange &range = ranges_[index];
                    drawRange(range, volumeShadowed, true, range.first,
                              range.count);
                }
                if (passReport != nullptr && !opaqueRangeIndices.isEmpty()) {
                    passReport->markPassCompleted(
                        ViewportRenderPass::OpaqueColor);
                }
                if (includeTransparent) drawTransparentTriangles();
                return;
            }
            for (int index : opaqueRangeIndices) {
                const DrawRange &range = ranges_[index];
                drawRange(range, volumeShadowed, true, range.first,
                          range.count);
            }
            if (passReport != nullptr && !opaqueRangeIndices.isEmpty()) {
                passReport->markPassCompleted(
                    ViewportRenderPass::OpaqueColor);
            }
        };
        const auto drawFrontOpaqueRanges = [&]() {
            if (frontOpaqueRangeIndices.isEmpty() || sharedTargets == nullptr ||
                !frontDomainReady) {
                return;
            }
            GLint previousFramebuffer = 0;
            GLint previousReadFramebuffer = 0;
            GLint previousViewport[4] = {};
            GLint previousDepthFunction = GL_LESS;
            GLint previousStencilFunc = GL_ALWAYS;
            GLint previousStencilRef = 0;
            GLint previousStencilValueMask = -1;
            GLint previousStencilWriteMask = -1;
            GLint previousStencilFail = GL_KEEP;
            GLint previousStencilDepthFail = GL_KEEP;
            GLint previousStencilDepthPass = GL_KEEP;
            GLint previousStencilBackFunc = GL_ALWAYS;
            GLint previousStencilBackRef = 0;
            GLint previousStencilBackValueMask = -1;
            GLint previousStencilBackWriteMask = -1;
            GLint previousStencilBackFail = GL_KEEP;
            GLint previousStencilBackDepthFail = GL_KEEP;
            GLint previousStencilBackDepthPass = GL_KEEP;
            GLboolean previousDepthMask = GL_TRUE;
            GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE,
                                               GL_TRUE, GL_TRUE};
            const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
            const GLboolean previousBlend = glIsEnabled(GL_BLEND);
            const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
            glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
            glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunc);
            glGetIntegerv(GL_STENCIL_REF, &previousStencilRef);
            glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
            glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
            glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL,
                          &previousStencilDepthFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS,
                          &previousStencilDepthPass);
            glGetIntegerv(GL_STENCIL_BACK_FUNC, &previousStencilBackFunc);
            glGetIntegerv(GL_STENCIL_BACK_REF, &previousStencilBackRef);
            glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK,
                          &previousStencilBackValueMask);
            glGetIntegerv(GL_STENCIL_BACK_WRITEMASK,
                          &previousStencilBackWriteMask);
            glGetIntegerv(GL_STENCIL_BACK_FAIL, &previousStencilBackFail);
            glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                          &previousStencilBackDepthFail);
            glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS,
                          &previousStencilBackDepthPass);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
            glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,
                          &previousReadFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glBindFramebuffer(GL_FRAMEBUFFER, sharedTargets->frontFramebuffer);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glViewport(0, 0, sharedTargets->pixelSize.width(),
                       sharedTargets->pixelSize.height());
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilMask(0u);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            const auto drawFrontColorPass = [&](bool volumeShadowed) {
                for (int index : frontOpaqueRangeIndices) {
                    const DrawRange &range = ranges_[index];
                    drawRange(range, volumeShadowed, true, range.first,
                              range.count);
                }
            };
            if (frontShadowVolumesAvailable) {
                glEnable(GL_STENCIL_TEST);
                glStencilFunc(GL_EQUAL, 0, 0xFFu);
                drawFrontColorPass(false);
                glStencilFunc(GL_NOTEQUAL, 0, 0xFFu);
                drawFrontColorPass(true);
            } else {
                glDisable(GL_STENCIL_TEST);
                drawFrontColorPass(false);
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              static_cast<GLuint>(previousFramebuffer));
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(previousReadFramebuffer));
            glViewport(previousViewport[0], previousViewport[1],
                       previousViewport[2], previousViewport[3]);
            glDepthFunc(static_cast<GLenum>(previousDepthFunction));
            glDepthMask(previousDepthMask);
            glColorMask(previousColorMask[0], previousColorMask[1],
                        previousColorMask[2], previousColorMask[3]);
            if (previousDepthTest) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
            if (previousBlend) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            if (previousStencilTest) glEnable(GL_STENCIL_TEST);
            else glDisable(GL_STENCIL_TEST);
            glStencilFuncSeparate(GL_FRONT,
                                  static_cast<GLenum>(previousStencilFunc),
                                  previousStencilRef,
                                  static_cast<GLuint>(previousStencilValueMask));
            glStencilMaskSeparate(GL_FRONT,
                                  static_cast<GLuint>(previousStencilWriteMask));
            glStencilOpSeparate(GL_FRONT,
                                static_cast<GLenum>(previousStencilFail),
                                static_cast<GLenum>(previousStencilDepthFail),
                                static_cast<GLenum>(previousStencilDepthPass));
            glStencilFuncSeparate(
                GL_BACK, static_cast<GLenum>(previousStencilBackFunc),
                previousStencilBackRef,
                static_cast<GLuint>(previousStencilBackValueMask));
            glStencilMaskSeparate(
                GL_BACK, static_cast<GLuint>(previousStencilBackWriteMask));
            glStencilOpSeparate(
                GL_BACK, static_cast<GLenum>(previousStencilBackFail),
                static_cast<GLenum>(previousStencilBackDepthFail),
                static_cast<GLenum>(previousStencilBackDepthPass));
            if (!mergeWorkbenchFrontAttachments(*sharedTargets, true)) {
                qWarning() << "Workbench front color merge failed";
            } else if (passReport != nullptr) {
                passReport->markPassCompleted(
                    ViewportRenderPass::FrontOpaqueColor);
                passReport->populatedAttachments |=
                    ViewportRenderTargets::FrontColor;
            }
        };
        if (shadowVolumesAvailable) {
            GLint previousStencilFunc = GL_ALWAYS;
            GLint previousStencilRef = 0;
            GLint previousStencilValueMask = 0xFFFFFFFFu;
            GLint previousStencilWriteMask = 0xFFFFFFFFu;
            GLint previousStencilFail = GL_KEEP;
            GLint previousStencilDepthFail = GL_KEEP;
            GLint previousStencilDepthPass = GL_KEEP;
            GLint previousStencilBackFunc = GL_ALWAYS;
            GLint previousStencilBackRef = 0;
            GLint previousStencilBackValueMask = 0xFFFFFFFFu;
            GLint previousStencilBackWriteMask = 0xFFFFFFFFu;
            GLint previousStencilBackFail = GL_KEEP;
            GLint previousStencilBackDepthFail = GL_KEEP;
            GLint previousStencilBackDepthPass = GL_KEEP;
            const GLboolean previousStencilTest = glIsEnabled(GL_STENCIL_TEST);
            glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunc);
            glGetIntegerv(GL_STENCIL_REF, &previousStencilRef);
            glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
            glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
            glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL,
                          &previousStencilDepthFail);
            glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS,
                          &previousStencilDepthPass);
            glGetIntegerv(GL_STENCIL_BACK_FUNC, &previousStencilBackFunc);
            glGetIntegerv(GL_STENCIL_BACK_REF, &previousStencilBackRef);
            glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK,
                          &previousStencilBackValueMask);
            glGetIntegerv(GL_STENCIL_BACK_WRITEMASK,
                          &previousStencilBackWriteMask);
            glGetIntegerv(GL_STENCIL_BACK_FAIL, &previousStencilBackFail);
            glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                          &previousStencilBackDepthFail);
            glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS,
                          &previousStencilBackDepthPass);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0u);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilFunc(GL_EQUAL, 0, 0xFFu);
            drawSurfaceRanges(false, false);
            glStencilFunc(GL_NOTEQUAL, 0, 0xFFu);
            drawSurfaceRanges(true, false);
            drawFrontOpaqueRanges();
            if (hasPartialAlpha) {
                glDisable(GL_STENCIL_TEST);
                drawTransparentTriangles();
            }

            glStencilFuncSeparate(GL_FRONT,
                                  static_cast<GLenum>(previousStencilFunc),
                                  previousStencilRef,
                                  static_cast<GLuint>(previousStencilValueMask));
            glStencilMaskSeparate(GL_FRONT,
                                  static_cast<GLuint>(previousStencilWriteMask));
            glStencilOpSeparate(GL_FRONT,
                                static_cast<GLenum>(previousStencilFail),
                                static_cast<GLenum>(previousStencilDepthFail),
                                static_cast<GLenum>(previousStencilDepthPass));
            glStencilFuncSeparate(
                GL_BACK, static_cast<GLenum>(previousStencilBackFunc),
                previousStencilBackRef,
                static_cast<GLuint>(previousStencilBackValueMask));
            glStencilMaskSeparate(
                GL_BACK, static_cast<GLuint>(previousStencilBackWriteMask));
            glStencilOpSeparate(
                GL_BACK, static_cast<GLenum>(previousStencilBackFail),
                static_cast<GLenum>(previousStencilBackDepthFail),
                static_cast<GLenum>(previousStencilBackDepthPass));
            if (previousStencilTest) glEnable(GL_STENCIL_TEST);
            else glDisable(GL_STENCIL_TEST);
        } else {
            drawSurfaceRanges(false, false);
            drawFrontOpaqueRanges();
            if (hasPartialAlpha) drawTransparentTriangles();
        }
        vertexBuffer_.release();
        vertexArray_.release();
        program_.release();
        if (matcapAvailable) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY,
                          static_cast<GLuint>(previousMatcapTextureBinding));
        }
        if (agxDisplayAvailable) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_3D,
                          static_cast<GLuint>(previousAgxTextureBinding));
        }
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousShadowTextureBinding));
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityNormalTextureBinding));
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityObjectIdTextureBinding));
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityDepthTextureBinding));
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousImageTextureBinding));
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousCavityJitterTextureBinding));
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D_ARRAY,
                      static_cast<GLuint>(previousUdimArrayTextureBinding));
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D,
                      static_cast<GLuint>(previousUdimLookupTextureBinding));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));

        // Blender's TransparentDepthPass runs after transparent color has
        // been resolved. Reuse the shared scene depth and ID attachments so
        // DOF and Workbench outlines see the nearest transparent surface.
        // Do not run this on the sorted fallback path: it has no compatible
        // shared depth target to update.
        if (weightedOitAvailable && sharedTargets != nullptr) {
            bool frontDepthMerged = false;
            bool regularDepthRendered = transparentRangeIndices.isEmpty();
            if (frontWeightedOitAvailable &&
                !frontTransparentRangeIndices.isEmpty()) {
                ViewportRenderTargets frontTargets = *sharedTargets;
                frontTargets.depthTexture = sharedTargets->frontDepthTexture;
                frontTargets.viewSpaceNormalTexture =
                    sharedTargets->frontViewSpaceNormalTexture;
                frontTargets.objectIdTexture =
                    sharedTargets->frontObjectIdTexture;
                frontTargets.frontOutlineIdTexture =
                    sharedTargets->frontOutlineIdTexture;
                const bool frontDepthRendered =
                    renderWorkbenchTransparentDepthPass(
                        transform, viewportSize, devicePixelRatio, settings,
                        frontTargets, frontTransparentRangeIndices);
                frontDepthMerged = frontDepthRendered &&
                    mergeWorkbenchFrontAttachments(*sharedTargets, false,
                                                   false);
                if (frontDepthMerged && passReport != nullptr) {
                    passReport->markPassCompleted(
                        ViewportRenderPass::FrontTransparentDepth);
                    passReport->markPassCompleted(
                        ViewportRenderPass::FrontTransparentMerge);
                    passReport->populatedAttachments |=
                        ViewportRenderTargets::FrontDepth |
                        ViewportRenderTargets::FrontObjectId |
                        ViewportRenderTargets::FrontOutlineId |
                        ViewportRenderTargets::FrontTransparentDepthAttachment |
                        ViewportRenderTargets::Depth |
                        ViewportRenderTargets::ObjectId;
                }
            }
            if (!transparentRangeIndices.isEmpty()) {
                const bool excludeFrontCoverage = frontDomainReady &&
                    (!frontOpaqueRangeIndices.isEmpty() ||
                     (hasFrontTransparentRanges && frontDepthMerged));
                regularDepthRendered =
                    renderWorkbenchTransparentDepthPass(
                        transform, viewportSize, devicePixelRatio, settings,
                        *sharedTargets, transparentRangeIndices,
                        excludeFrontCoverage);
                if (regularDepthRendered && passReport != nullptr) {
                    passReport->markPassCompleted(
                        ViewportRenderPass::TransparentDepth);
                    passReport->populatedAttachments |=
                        ViewportRenderTargets::Depth |
                        ViewportRenderTargets::ViewSpaceNormal |
                        ViewportRenderTargets::ObjectId;
                }
            }
            transparentDepthCoverageReady = regularDepthRendered &&
                (!hasFrontTransparentRanges ||
                 (frontWeightedOitAvailable && frontDepthMerged));
        }
    }

    if (!hasPartialAlpha) {
        transparentDepthCoverageReady = true;
    }

    if (settings.mode == ViewportShadingMode::Solid &&
        settings.outline && !previewOverlay && !selectionOverlay &&
        !settings.xrayEnabled() && !vertices_.isEmpty()) {
        const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
        const QSize pixelSize(qRound(viewportSize.width() * dpr),
                              qRound(viewportSize.height() * dpr));
        const bool sharedOutlineIdsReady = sharedPrepassReady &&
                                           transparentDepthCoverageReady;
        const bool useSharedOutlineIds = sharedOutlineIdsReady &&
                                         outlineRequested;
        const bool outlineFramebufferReady = useSharedOutlineIds ||
            ensureObjectIdFramebuffer(pixelSize);
        if (outlineFramebufferReady) {
            GLint previousFramebuffer = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            GLint previousActiveTexture = GL_TEXTURE0;
            GLint previousTextureBinding = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);

            if (!useSharedOutlineIds) {
                glBindFramebuffer(GL_FRAMEBUFFER, objectIdFramebuffer_);
                glViewport(0, 0, pixelSize.width(), pixelSize.height());
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDisable(GL_BLEND);
                if (settings.backfaceCulling) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                } else {
                    glDisable(GL_CULL_FACE);
                }
                const GLuint emptyId = 0;
                glClearBufferuiv(GL_COLOR, 0, &emptyId);
                glClearDepth(1.0);
                glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

                objectIdProgram_.bind();
                const GLint objectIdUniformLocation =
                    objectIdProgram_.uniformLocation("uObjectId");
                objectIdProgram_.setUniformValue("uWorkbenchImage", 0);
                objectIdProgram_.setUniformValue("uWorkbenchUdimImage", 1);
                objectIdProgram_.setUniformValue("uWorkbenchUdimLookup", 2);
                objectIdProgram_.setUniformValue(
                    "uViewProjection",
                    viewportViewProjection(transform, viewportSize));
                GLint previousObjectIdTexture0 = 0;
                GLint previousObjectIdTexture1 = 0;
                GLint previousObjectIdTexture2 = 0;
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D,
                              &previousObjectIdTexture0);
                glActiveTexture(GL_TEXTURE1);
                glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,
                              &previousObjectIdTexture1);
                glActiveTexture(GL_TEXTURE2);
                glGetIntegerv(GL_TEXTURE_BINDING_2D,
                              &previousObjectIdTexture2);
                glActiveTexture(GL_TEXTURE0);
                vertexArray_.bind();
                vertexBuffer_.bind();
                for (int rangeIndex = 0; rangeIndex < ranges_.size();
                     ++rangeIndex) {
                    const DrawRange &range = ranges_[rangeIndex];
                    if (range.count <= 0) {
                        continue;
                    }
                    GLuint objectId = range.effectObjectId;
                    if (range.outlineHandledBySceneStroke) {
                        // Keep depth from suppressing outlines behind this
                        // surface, while leaving its own perimeter to the
                        // shared scene-stroke renderer.
                        objectId = 0u;
                    } else if (range.selected) {
                        objectId |= 0x80000000u;
                    }
                    glUniform1ui(objectIdUniformLocation, objectId);
                    const QString alphaMode =
                        range.workbenchMaterial.textureImageAlphaMode;
                    const bool ignoreTextureAlpha =
                        alphaMode.compare(QStringLiteral("NONE"),
                                          Qt::CaseInsensitive) == 0 ||
                        alphaMode.compare(QStringLiteral("IGNORE"),
                                          Qt::CaseInsensitive) == 0 ||
                        alphaMode.compare(QStringLiteral("CHANNEL_PACKED"),
                                          Qt::CaseInsensitive) == 0;
                    const bool textureColorEnabled =
                        settings.colorMode == ViewportColorMode::Texture &&
                        range.useTextureColor;
                    const GLuint imageTexture = textureColorEnabled
                        ? ensureImageTexture(
                              range.textureImagePath,
                              range.workbenchMaterial
                                  .textureImagePackedDataBase64,
                              range.textureImageSrgb)
                        : 0;
                    const UdimTextureSet udimTexture = textureColorEnabled
                        ? ensureUdimTexture(range.textureImageTiles,
                                            range.textureImageSrgb)
                        : UdimTextureSet{};
                    glUniform1i(
                        objectIdProgram_.uniformLocation(
                            "uUseTextureAlphaMask"),
                        imageTexture != 0 || udimTexture.imageArray != 0);
                    glUniform1i(
                        objectIdProgram_.uniformLocation(
                            "uTextureAlphaCutoff"),
                        !ignoreTextureAlpha);
                    objectIdProgram_.setUniformValue(
                        "uUseUdimImage", udimTexture.imageArray != 0);
                    objectIdProgram_.setUniformValue(
                        "uTextureInterpolation",
                        workbenchTextureInterpolation(
                            range.workbenchMaterial
                                .textureImageInterpolation));
                    objectIdProgram_.setUniformValue(
                        "uTextureExtension", workbenchTextureExtension(
                            range.workbenchMaterial.textureImageExtension));
                    configureWorkbenchImageTexture(
                        imageTexture, range.workbenchMaterial);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, imageTexture);
                    configureWorkbenchUdimTexture(
                        udimTexture.imageArray, range.workbenchMaterial);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D_ARRAY,
                                  udimTexture.imageArray);
                    glActiveTexture(GL_TEXTURE2);
                    glBindTexture(GL_TEXTURE_2D, udimTexture.tileLookup);
                    glActiveTexture(GL_TEXTURE0);
                    objectIdProgram_.setUniformValue(
                        "uWorldOffset",
                        QVector3D(static_cast<float>(range.offset.x),
                                  static_cast<float>(range.offset.y),
                                  static_cast<float>(range.offset.z)));
                    glDrawArrays(GL_TRIANGLES, range.first, range.count);
                }
                vertexBuffer_.release();
                vertexArray_.release();
                objectIdProgram_.release();
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D,
                              static_cast<GLuint>(previousObjectIdTexture0));
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D_ARRAY,
                              static_cast<GLuint>(previousObjectIdTexture1));
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D,
                              static_cast<GLuint>(previousObjectIdTexture2));
                glActiveTexture(static_cast<GLenum>(previousActiveTexture));

                glBindFramebuffer(GL_FRAMEBUFFER,
                                  static_cast<GLuint>(previousFramebuffer));
                glViewport(previousViewport[0], previousViewport[1],
                           previousViewport[2], previousViewport[3]);
            }
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextureBinding);
            glBindTexture(GL_TEXTURE_2D,
                          useSharedOutlineIds
                              ? workbenchOutlineIdTexture_
                              : objectIdTexture_);

            outlineProgram_.bind();
            outlineProgram_.setUniformValue("uObjectIdBuffer", 0);
            outlineProgram_.setUniformValue(
                "uOutlineColor",
                QVector4D(settings.outlineColor.redF(),
                          settings.outlineColor.greenF(),
                          settings.outlineColor.blueF(), 1.0f));
            const QColor selectionColor = viewportSelectionColor();
            outlineProgram_.setUniformValue(
                "uSelectionColor",
                QVector4D(selectionColor.redF(), selectionColor.greenF(),
                          selectionColor.blueF(), 1.0f));
            outlineProgram_.setUniformValue(
                "uUiScale", static_cast<float>(dpr));
            outlineVertexArray_.bind();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            outlineVertexArray_.release();
            outlineProgram_.release();
            glBindTexture(GL_TEXTURE_2D,
                          static_cast<GLuint>(previousTextureBinding));
            glActiveTexture(static_cast<GLenum>(previousActiveTexture));
        }
    }

    glColorMask(previousColorMask[0], previousColorMask[1],
                previousColorMask[2], previousColorMask[3]);
    glDepthMask(previousDepthMask);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    if (previousDepthTest) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (previousCullFace) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (previousBlend) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    glBlendEquationSeparate(static_cast<GLenum>(previousBlendEquationRgb),
                            static_cast<GLenum>(previousBlendEquationAlpha));
    glBlendFuncSeparate(static_cast<GLenum>(previousBlendSourceRgb),
                        static_cast<GLenum>(previousBlendDestinationRgb),
                        static_cast<GLenum>(previousBlendSourceAlpha),
                        static_cast<GLenum>(previousBlendDestinationAlpha));
    glUseProgram(static_cast<GLuint>(previousProgram));
    return true;
}

} // namespace classiCAD
