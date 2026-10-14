/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "ui/viewport/viewport_raster_workbench_linear.h"
#include "ui/viewport/viewport_renderer.h"
#include "services/hit_testing/curve_hit_tester.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

bool sameImage(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()) return false;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<size_t>(a.width()) * 4) != 0) {
            return false;
        }
    }
    return true;
}

bool sameOuterAlphaProfile(const QImage &a, const QImage &b)
{
    // Compare the exterior silhouette away from the four vertices. Mesh and
    // NURBS tessellators can use different shared-edge fill rules along their
    // internal diagonals and exact corners, which is separate from the DPR
    // sampling footprint this contract exercises.
    if (a.size() != b.size()) return false;
    int minX = a.width(), minY = a.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (qAlpha(a.pixel(x, y)) > 0 || qAlpha(b.pixel(x, y)) > 0) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < minY) return false;
    const auto onOutsideEdge = [&](const QImage &image, int x, int y) {
        if (qAlpha(image.pixel(x, y)) == 0) return false;
        return x == 0 || y == 0 || x + 1 == image.width() ||
               y + 1 == image.height() ||
               qAlpha(image.pixel(x - 1, y)) == 0 ||
               qAlpha(image.pixel(x + 1, y)) == 0 ||
               qAlpha(image.pixel(x, y - 1)) == 0 ||
               qAlpha(image.pixel(x, y + 1)) == 0;
    };
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            if (!onOutsideEdge(a, x, y) && !onOutsideEdge(b, x, y)) continue;
            const bool nearHorizontalCorner =
                (x - minX <= 1 || maxX - x <= 1) &&
                (y - minY <= 1 || maxY - y <= 1);
            if (nearHorizontalCorner) continue;
            if (qAlpha(a.pixel(x, y)) != qAlpha(b.pixel(x, y))) return false;
        }
    }
    return true;
}

bool imagesWithinOneCodeValue(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()) return false;
    for (int y = 0; y < a.height(); ++y) {
        const auto *left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (std::abs(qRed(left[x]) - qRed(right[x])) > 1 ||
                std::abs(qGreen(left[x]) - qGreen(right[x])) > 1 ||
                std::abs(qBlue(left[x]) - qBlue(right[x])) > 1 ||
                std::abs(qAlpha(left[x]) - qAlpha(right[x])) > 1) {
                return false;
            }
        }
    }
    return true;
}

bool sameFragmentProbes(const RasterWorkbenchFragmentSink &a,
                        const RasterWorkbenchFragmentSink &b)
{
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); ++index) {
        const auto &left = a[index];
        const auto &right = b[index];
        if (left.x != right.x || left.y != right.y ||
            std::abs(left.cameraDepth - right.cameraDepth) > 1.0e-5f ||
            std::abs(left.oitLinearDepth - right.oitLinearDepth) > 1.0e-5f ||
            std::abs(left.alpha - right.alpha) > 1.0e-5f ||
            left.sourceId != right.sourceId) {
            return false;
        }
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(left.sceneLinearRgb[static_cast<size_t>(channel)] -
                         right.sceneLinearRgb[static_cast<size_t>(channel)]) >
                1.0e-5f) {
                return false;
            }
        }
    }
    return true;
}

QImage render(ViewportRenderer &renderer, const Shape &shape,
              const QSize &size, ObjectId id,
              RasterWorkbenchFragmentSink *sink,
              std::optional<WorkbenchMaterialData> material = std::nullopt,
              const QVector<WorkbenchMaterialData> &meshMaterials = {})
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.drawShape(painter, shape, size, false, false, false,
                       QColor(Qt::white), {}, 0.0, id, 1, nullptr, {},
                       std::nullopt, material, {}, {},
                       identityMeshObjectTransform, meshMaterials, sink);
    painter.end();
    return image;
}

QImage renderAtDpr(ViewportRenderer &renderer, const Shape &shape,
                   const QSize &logicalSize, qreal dpr, ObjectId id,
                   std::optional<WorkbenchMaterialData> material = std::nullopt,
                   const QVector<WorkbenchMaterialData> &meshMaterials = {})
{
    QImage image(QSize(qRound(logicalSize.width() * dpr),
                       qRound(logicalSize.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.drawShape(painter, shape, logicalSize, false, false, false,
                       QColor(Qt::white), {}, 0.0, id, 1, nullptr, {},
                       std::nullopt, material, {}, {},
                       identityMeshObjectTransform, meshMaterials);
    painter.end();
    return image;
}

bool coveredFragments(const RasterWorkbenchFragmentSink &fragments,
                      int width, int height, quint64 sourceId,
                      float expectedDepth, float expectedOitDepth,
                      float expectedAlpha)
{
    if (fragments.size() < 100) return false;
    bool sawHdr = false;
    bool sawDepth = false;
    bool sawOitDepth = false;
    bool sawAlpha = false;
    for (const auto &fragment : fragments) {
        if (fragment.x < 0 || fragment.x >= width ||
            fragment.y < 0 || fragment.y >= height ||
            fragment.sourceId != sourceId ||
            !std::isfinite(fragment.cameraDepth) ||
            !std::isfinite(fragment.oitLinearDepth) ||
            !std::isfinite(fragment.sceneLinearRgb[0]) ||
            !std::isfinite(fragment.sceneLinearRgb[1]) ||
            !std::isfinite(fragment.sceneLinearRgb[2])) {
            return false;
        }
        sawHdr = sawHdr || fragment.sceneLinearRgb[2] > 1.0f;
        sawDepth = sawDepth ||
            std::abs(fragment.cameraDepth - expectedDepth) < 1.0e-3f;
        sawOitDepth = sawOitDepth ||
            std::abs(fragment.oitLinearDepth - expectedOitDepth) < 1.0e-3f;
        sawAlpha = sawAlpha || std::abs(fragment.alpha - expectedAlpha) < 0.002f;
    }
    return sawHdr && sawDepth && sawOitDepth && sawAlpha;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    bool passed = true;
    const QSize size(128, 128);
    ViewportTransform transform;
    transform.zoom() = 32.0;
    transform.setViewPreset(ViewportViewPreset::Top);
    transform.setPerspectiveEnabled(false);
    CurveHitTester hitTester;
    ViewportRenderer renderer(transform, hitTester);
    ViewportShadingSettings shading;
    shading.mode = ViewportShadingMode::Solid;
    shading.lightingMode = ViewportLightingMode::Flat;
    shading.colorMode = ViewportColorMode::Material;
    shading.colorTransform = 0;
    renderer.setShadingSettings(shading);

    QImage matcapTexture(2, 2, QImage::Format_RGB32);
    matcapTexture.setPixelColor(0, 0, QColor(0, 0, 0));
    matcapTexture.setPixelColor(1, 0, QColor(255, 0, 0));
    matcapTexture.setPixelColor(0, 1, QColor(0, 255, 0));
    matcapTexture.setPixelColor(1, 1, QColor(0, 0, 255));
    const QVector3D linearTextureCenter =
        workbenchMatcapSampleLinear(matcapTexture, 0.5f, 0.5f);
    const QVector3D linearTextureCorner =
        workbenchMatcapSampleLinear(matcapTexture, 0.25f, 0.25f);
    passed &= check((linearTextureCenter - QVector3D(0.25f, 0.25f, 0.25f))
                            .length() < 1.0e-5f &&
                        linearTextureCorner.length() < 1.0e-5f &&
                        (workbenchMatcapSampleLinear(matcapTexture, 1.0f, 1.0f) -
                         QVector3D(0.0f, 0.0f, 1.0f)).length() < 1.0e-5f,
                    "QPainter MatCap filtering must use normalized texel centers and clamp to edge texels");

    QByteArray halfMatcap(0, Qt::Uninitialized);
    for (int layer = 0; layer < 2; ++layer) {
        for (int texel = 0; texel < 4; ++texel) {
            const float value = float((layer == 0 ? 0 : 10) + texel);
            for (int channel = 0; channel < 3; ++channel) {
                const qfloat16 half(value);
                quint16 bits = 0;
                std::memcpy(&bits, &half, sizeof(bits));
                bits = qToLittleEndian(bits);
                halfMatcap.append(reinterpret_cast<const char *>(&bits),
                                  sizeof(bits));
            }
        }
    }
    const QVector3D halfTextureCenter = workbenchMatcapSampleLinearHalf(
        halfMatcap, 2, 2, 0, 0.5f, 0.5f);
    const QVector3D halfTextureCorner = workbenchMatcapSampleLinearHalf(
        halfMatcap, 2, 2, 1, 1.0f, 1.0f);
    passed &= check((halfTextureCenter - QVector3D(1.5f, 1.5f, 1.5f))
                            .length() < 1.0e-5f &&
                        (halfTextureCorner - QVector3D(13.0f, 13.0f, 13.0f))
                                .length() < 1.0e-5f,
                    "half-float MatCap filtering must use normalized texel centers and clamp to edge texels");

    constexpr quint64 meshId = 7201;
    constexpr quint64 surfaceId = 7202;
    constexpr float materialAlpha = 0.45f;
    const QVector3D sourceColor(0.3f, 0.6f, 1.75f);
    Shape mesh;
    mesh.geometryType = GeometryType::PolygonMesh;
    mesh.polygonMesh.vertices = {{-1.0, -1.0, -1.0},
                                 {1.0, -1.0, -1.0},
                                 {1.0, 1.0, -1.0},
                                 {-1.0, 1.0, -1.0}};
    mesh.polygonMesh.faces = {{0, 1, 2, 3}};
    mesh.polygonMesh.faceMaterialIndices = {0};
    QVector<WorkbenchMaterialData> meshMaterials(1);
    meshMaterials[0].baseColorSceneLinear = sourceColor;
    meshMaterials[0].alpha = materialAlpha;

    RasterWorkbenchFragmentSink meshFragments;
    const QImage meshBaseline = render(renderer, mesh, size,
                                       ObjectId::fromValue(meshId), nullptr,
                                       std::nullopt, meshMaterials);
    const QImage meshObserved = [&]() {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.drawShape(painter, mesh, size, false, false, false,
                           QColor(Qt::white), {}, 0.0,
                           ObjectId::fromValue(meshId), 1, nullptr, {},
                           std::nullopt, std::nullopt, {}, {},
                           identityMeshObjectTransform, meshMaterials,
                           &meshFragments);
        painter.end();
        return image;
    }();
    const Point3D camera = transform.cameraPosition(size);
    const Point3D view = transform.viewDirection();
    const ViewportCameraState cameraState = transform.cameraState();
    const ViewportCameraPreferences cameraPreferences =
        transform.cameraPreferences();
    const float oitDepthOffset = static_cast<float>(
        std::min(cameraPreferences.clipEnd,
                 std::max<qreal>(1000.0, cameraState.gridViewDistance)) -
        cameraPreferences.clipStart);
    const float meshDepth = static_cast<float>(
        -((-1.0 - camera.z) * view.z));
    passed &= check(sameImage(meshBaseline, meshObserved),
                    "mesh fragment observation must preserve rendered pixels");
    passed &= check(coveredFragments(meshFragments, size.width(), size.height(),
                                     meshId, meshDepth,
                                     oitDepthOffset + meshDepth, materialAlpha),
                    "mesh fragments must retain coverage, linear HDR color, material alpha, source ID, camera depth, and Blender orthographic OIT depth");
    Shape surface;
    surface.geometryType = GeometryType::NurbsSurface;
    surface.nurbsSurface.degreeU = surface.nurbsSurface.degreeV = 1;
    surface.nurbsSurface.orderU = surface.nurbsSurface.orderV = 2;
    surface.nurbsSurface.controlVertexCountU = 2;
    surface.nurbsSurface.controlVertexCountV = 2;
    surface.nurbsSurface.controlPoints = {{-1.0, -1.0, -2.0},
                                         {-1.0, 1.0, -2.0},
                                         {1.0, -1.0, -2.0},
                                         {1.0, 1.0, -2.0}};
    surface.nurbsSurface.weights = {1.0, 1.0, 1.0, 1.0};
    surface.nurbsSurface.knotsU = {0.0, 1.0};
    surface.nurbsSurface.knotsV = {0.0, 1.0};
    WorkbenchMaterialData surfaceMaterial;
    surfaceMaterial.baseColorSceneLinear = sourceColor;
    surfaceMaterial.alpha = materialAlpha;
    RasterWorkbenchFragmentSink surfaceFragments;
    const QImage surfaceBaseline = render(renderer, surface, size,
                                          ObjectId::fromValue(surfaceId), nullptr,
                                          surfaceMaterial);
    const QImage surfaceObserved = [&]() {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.drawShape(painter, surface, size, false, false, false,
                           QColor(Qt::white), {}, 0.0,
                           ObjectId::fromValue(surfaceId), 1, nullptr, {},
                           std::nullopt, surfaceMaterial, {}, {},
                           identityMeshObjectTransform, {}, &surfaceFragments);
        painter.end();
        return image;
    }();
    const float surfaceDepth = static_cast<float>(
        -((-2.0 - camera.z) * view.z));
    passed &= check(sameImage(surfaceBaseline, surfaceObserved),
                    "NURBS fragment observation must preserve rendered pixels");
    passed &= check(coveredFragments(surfaceFragments, size.width(), size.height(),
                                     surfaceId, surfaceDepth,
                                     oitDepthOffset + surfaceDepth,
                                     materialAlpha),
                    "NURBS fragments must retain coverage, linear HDR color, material alpha, source ID, camera depth, and Blender orthographic OIT depth");
    Shape alternateDiagonalSurface = surface;
    std::swap(alternateDiagonalSurface.nurbsSurface.controlPoints[0],
              alternateDiagonalSurface.nurbsSurface.controlPoints[2]);
    std::swap(alternateDiagonalSurface.nurbsSurface.controlPoints[1],
              alternateDiagonalSurface.nurbsSurface.controlPoints[3]);
    const QImage firstDiagonalSurfaceImage = render(
        renderer, surface, size, ObjectId::fromValue(surfaceId), nullptr,
        surfaceMaterial);
    const QImage alternateDiagonalSurfaceImage = render(
        renderer, alternateDiagonalSurface, size,
        ObjectId::fromValue(surfaceId), nullptr, surfaceMaterial);
    passed &= check(
        imagesWithinOneCodeValue(firstDiagonalSurfaceImage,
                                 alternateDiagonalSurfaceImage),
        "transparent planar NURBS tessellation must remain within one display code value when its triangulation diagonal changes");
    WorkbenchMaterialData opaqueSurfaceMaterial = surfaceMaterial;
    opaqueSurfaceMaterial.alpha = 1.0;
    const QImage firstOpaqueDiagonalImage = render(
        renderer, surface, size, ObjectId::fromValue(surfaceId), nullptr,
        opaqueSurfaceMaterial);
    const QImage alternateOpaqueDiagonalImage = render(
        renderer, alternateDiagonalSurface, size,
        ObjectId::fromValue(surfaceId), nullptr, opaqueSurfaceMaterial);
    passed &= check(
        sameImage(firstOpaqueDiagonalImage, alternateOpaqueDiagonalImage),
        "opaque planar NURBS tessellation must not change when its triangulation diagonal changes");
    RasterWorkbenchFragmentSink firstDiagonalFragments;
    RasterWorkbenchFragmentSink alternateDiagonalFragments;
    render(renderer, surface, size, ObjectId::fromValue(surfaceId),
           &firstDiagonalFragments, surfaceMaterial);
    render(renderer, alternateDiagonalSurface, size,
           ObjectId::fromValue(surfaceId), &alternateDiagonalFragments,
           surfaceMaterial);
    passed &= check(
        sameFragmentProbes(firstDiagonalFragments,
                           alternateDiagonalFragments),
        "scene-linear transparent fragments must be invariant under planar NURBS tessellation diagonal changes");
    Shape coverageMesh = mesh;
    Shape coverageSurface = surface;
    constexpr double coverageOffsetX = 0.013;
    constexpr double coverageOffsetY = -0.021;
    for (Point3D &point : coverageMesh.polygonMesh.vertices) {
        point.x += coverageOffsetX;
        point.y += coverageOffsetY;
        point.z = -2.0;
    }
    for (Point3D &point : coverageSurface.nurbsSurface.controlPoints) {
        point.x += coverageOffsetX;
        point.y += coverageOffsetY;
        point.z = -2.0;
    }
    QVector<WorkbenchMaterialData> coverageMeshMaterials(1);
    coverageMeshMaterials[0] = opaqueSurfaceMaterial;
    bool coverageProfilesMatch = true;
    bool sawPartialCoverage = false;
    bool coverageIsSixteenthBased = true;
    for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        const QImage meshCoverage = renderAtDpr(
            renderer, coverageMesh, size, dpr, ObjectId::fromValue(meshId),
            std::nullopt, coverageMeshMaterials);
        const QImage nurbsCoverage = renderAtDpr(
            renderer, coverageSurface, size, dpr,
            ObjectId::fromValue(surfaceId), opaqueSurfaceMaterial);
        const bool profilesMatch =
            sameOuterAlphaProfile(meshCoverage, nurbsCoverage);
        coverageProfilesMatch &= profilesMatch;
        for (int y = 0; y < nurbsCoverage.height(); ++y) {
            for (int x = 0; x < nurbsCoverage.width(); ++x) {
                const int alpha = qAlpha(nurbsCoverage.pixel(x, y));
                if (alpha > 0 && alpha < 255) {
                    sawPartialCoverage = true;
                    coverageIsSixteenthBased &=
                        std::abs(alpha - qRound(alpha / 255.0 * 16.0) *
                                                 (255.0 / 16.0)) <= 1;
                }
            }
        }
    }
    passed &= check(coverageProfilesMatch,
                    "QPainter mesh and NURBS edge coverage must match at DPR 1, 1.25, 1.5, and 2");
    passed &= check(sawPartialCoverage && coverageIsSixteenthBased,
                    "QPainter fractional triangle-edge alpha must retain the bounded 4x4 coverage steps");
    passed &= check(std::abs(meshDepth - surfaceDepth) > 0.5f,
                    "camera depth must vary with surface position");

    return passed ? 0 : 1;
}
