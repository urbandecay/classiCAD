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

bool coveredFragments(const RasterWorkbenchFragmentSink &fragments,
                      int width, int height, quint64 sourceId,
                      float expectedDepth, float expectedAlpha)
{
    if (fragments.size() < 100) return false;
    bool sawHdr = false;
    bool sawDepth = false;
    bool sawAlpha = false;
    for (const auto &fragment : fragments) {
        if (fragment.x < 0 || fragment.x >= width ||
            fragment.y < 0 || fragment.y >= height ||
            fragment.sourceId != sourceId ||
            !std::isfinite(fragment.cameraDepth) ||
            !std::isfinite(fragment.sceneLinearRgb[0]) ||
            !std::isfinite(fragment.sceneLinearRgb[1]) ||
            !std::isfinite(fragment.sceneLinearRgb[2])) {
            return false;
        }
        sawHdr = sawHdr || fragment.sceneLinearRgb[2] > 1.0f;
        sawDepth = sawDepth ||
            std::abs(fragment.cameraDepth - expectedDepth) < 1.0e-3f;
        sawAlpha = sawAlpha || std::abs(fragment.alpha - expectedAlpha) < 0.002f;
    }
    return sawHdr && sawDepth && sawAlpha;
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
    const float meshDepth = static_cast<float>(
        -((-1.0 - camera.z) * view.z));
    passed &= check(sameImage(meshBaseline, meshObserved),
                    "mesh fragment observation must preserve rendered pixels");
    passed &= check(coveredFragments(meshFragments, size.width(), size.height(),
                                     meshId, meshDepth, materialAlpha),
                    "mesh fragments must retain coverage, linear HDR color, material alpha, source ID, and forward depth");

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
                                     surfaceId, surfaceDepth, materialAlpha),
                    "NURBS fragments must retain coverage, linear HDR color, material alpha, source ID, and forward depth");
    passed &= check(std::abs(meshDepth - surfaceDepth) > 0.5f,
                    "camera depth must vary with surface position");

    return passed ? 0 : 1;
}
