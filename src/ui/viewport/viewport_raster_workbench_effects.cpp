/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_effects.h"

#include "viewport_depth_geometry.h"

#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

struct RasterSample {
    float depth = -std::numeric_limits<float>::infinity();
    QVector3D normal;
    quint64 objectId = 0;
};

QVector3D normalized(const QVector3D &value)
{
    const float length = value.length();
    return length > 1.0e-12f ? value / length : QVector3D();
}

float saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void applyRasterWorkbenchCavity(QImage *colorImage,
                                const QVector<ViewportRenderObject> &objects,
                                const ViewportTransform &transform,
                                const QSize &viewportSize,
                                qreal devicePixelRatio,
                                const ViewportShadingSettings &settings)
{
    if (colorImage == nullptr || colorImage->isNull() || !settings.cavity ||
        settings.mode != ViewportShadingMode::Solid || settings.xrayEnabled() ||
        viewportSize.isEmpty()) {
        return;
    }

    // Workbench's opaque resolve runs cavity after lighting. Preserve its
    // scene-wide transparent-scene restriction in this fallback as well.
    for (const ViewportRenderObject &object : objects) {
        if (object.shape.geometryType != GeometryType::NurbsSurface &&
            object.shape.geometryType != GeometryType::NurbsSolid) {
            continue;
        }
        const float alpha = settings.colorMode == ViewportColorMode::Custom
                                ? 1.0f
                            : settings.colorMode == ViewportColorMode::Material ||
                                    settings.colorMode == ViewportColorMode::Attribute ||
                                    settings.colorMode == ViewportColorMode::Texture
                                ? object.workbenchMaterial.alpha
                                : object.displayColorSceneLinear.w();
        if (alpha < 0.999f) return;
    }

    const int outputWidth = colorImage->width();
    const int outputHeight = colorImage->height();
    const qreal colorDpr = std::max<qreal>(colorImage->devicePixelRatio(), 1.0);
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const int width = qRound(viewportSize.width() * dpr);
    const int height = qRound(viewportSize.height() * dpr);
    if (width <= 0 || height <= 0 || outputWidth <= 0 || outputHeight <= 0)
        return;
    QVector<RasterSample> samples(width * height);
    const Point3D towardCamera = transform.viewDirection();

    for (const ViewportRenderObject &object : objects) {
        if (object.shape.geometryType != GeometryType::NurbsSurface &&
            object.shape.geometryType != GeometryType::NurbsSolid) {
            continue;
        }
        ViewportDepthGeometry fallback;
        const ViewportDepthGeometry *geometry =
            object.preparedDepthGeometry.data();
        if (geometry == nullptr) {
            fallback = buildViewportDepthGeometry(object);
            geometry = &fallback;
        }
        const QVector<QVector3D> &vertices = geometry->surfaceVertices;
        const QVector<QVector3D> &normals = geometry->surfaceNormals;
        for (int triangle = 0; triangle + 2 < vertices.size(); triangle += 3) {
            QVector3D world[3];
            QVector3D normal[3];
            QPointF screen[3];
            float depth[3];
            bool valid = true;
            for (int corner = 0; corner < 3; ++corner) {
                world[corner] = vertices[triangle + corner] + QVector3D(
                    object.preparedGeometryOffset.x,
                    object.preparedGeometryOffset.y,
                    object.preparedGeometryOffset.z);
                normal[corner] = normals.size() > triangle + corner
                                     ? normalized(normals[triangle + corner])
                                     : QVector3D();
                const Point3D point(world[corner].x(), world[corner].y(),
                                    world[corner].z());
                valid = valid && transform.worldPointToScreenUnclipped(
                                     point, viewportSize, &screen[corner]);
                depth[corner] = static_cast<float>(
                    point.x * towardCamera.x + point.y * towardCamera.y +
                    point.z * towardCamera.z);
            }
            if (!valid) continue;
            const QPointF p0 = screen[0] * dpr;
            const QPointF p1 = screen[1] * dpr;
            const QPointF p2 = screen[2] * dpr;
            const qreal area = (p1.x() - p0.x()) * (p2.y() - p0.y()) -
                               (p1.y() - p0.y()) * (p2.x() - p0.x());
            if (std::abs(area) < 1.0e-10) continue;
            const int minX = std::clamp(static_cast<int>(std::floor(
                std::min({p0.x(), p1.x(), p2.x()}))), 0, width - 1);
            const int maxX = std::clamp(static_cast<int>(std::ceil(
                std::max({p0.x(), p1.x(), p2.x()}))), 0, width - 1);
            const int minY = std::clamp(static_cast<int>(std::floor(
                std::min({p0.y(), p1.y(), p2.y()}))), 0, height - 1);
            const int maxY = std::clamp(static_cast<int>(std::ceil(
                std::max({p0.y(), p1.y(), p2.y()}))), 0, height - 1);
            const quint64 id = object.objectId.value();
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const qreal px = x + 0.5;
                    const qreal py = y + 0.5;
                    const qreal w0 = ((p1.x() - px) * (p2.y() - py) -
                                      (p1.y() - py) * (p2.x() - px)) / area;
                    const qreal w1 = ((p2.x() - px) * (p0.y() - py) -
                                      (p2.y() - py) * (p0.x() - px)) / area;
                    const qreal w2 = 1.0 - w0 - w1;
                    if (w0 < -1.0e-6 || w1 < -1.0e-6 || w2 < -1.0e-6)
                        continue;
                    const float z = static_cast<float>(w0 * depth[0] +
                                                       w1 * depth[1] +
                                                       w2 * depth[2]);
                    RasterSample &sample = samples[y * width + x];
                    if (sample.objectId != 0 && z <= sample.depth) continue;
                    sample.depth = z;
                    sample.normal = normalized(
                        normal[0] * static_cast<float>(w0) +
                        normal[1] * static_cast<float>(w1) +
                        normal[2] * static_cast<float>(w2));
                    sample.objectId = id;
                }
            }
        }
    }

    const bool ssao = settings.cavityType == ViewportCavityType::World ||
                      settings.cavityType == ViewportCavityType::Both;
    const bool curvature = settings.cavityType == ViewportCavityType::Screen ||
                           settings.cavityType == ViewportCavityType::Both;
    const int requested = std::clamp(settings.cavitySamples, 1, 32);
    const float radius = std::clamp(
        static_cast<float>(settings.cavityDistance *
                           transform.viewScalePixelsPerWorldUnit(viewportSize) * dpr),
        1.0f, 64.0f);
    const float attenuation = std::max(0.01f,
        static_cast<float>(settings.cavityAttenuation));
    const float worldRadius = std::max(
        1.0e-4f, static_cast<float>(settings.cavityDistance));
    const float step = std::max(1.0f, radius / 4.0f);
    const std::array<QPoint, 8> directions = {{{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                               {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};
    QVector<float> factors(width * height, 1.0f);
    const float ridge = std::max(0.0f,
        static_cast<float>(settings.cavityRidgeFactor));
    const float valley = std::max(0.0f,
        static_cast<float>(settings.cavityValleyFactor));
    const float curvatureRidge = std::max(0.0f,
        static_cast<float>(settings.curvatureRidgeFactor));
    const float curvatureValley = std::max(0.0f,
        static_cast<float>(settings.curvatureValleyFactor));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const RasterSample &center = samples[y * width + x];
            if (center.objectId == 0) continue;
            float occlusion = 0.0f;
            float ridgeResponse = 0.0f;
            float valleyResponse = 0.0f;
            int validNeighbors = 0;
            for (int i = 0; i < requested; ++i) {
                const QPoint direction = directions[static_cast<size_t>(i % 8)];
                const int ring = 1 + i / 8;
                const int sx = x + qRound(direction.x() * step * ring);
                const int sy = y + qRound(direction.y() * step * ring);
                if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                const RasterSample &neighbor = samples[sy * width + sx];
                if (neighbor.objectId == 0) continue;
                ++validNeighbors;
                const float delta = neighbor.depth - center.depth;
                const float depthWeight = saturate(
                    (delta - worldRadius * 0.01f) * attenuation / worldRadius);
                if (ssao) occlusion += depthWeight;
                if (curvature) {
                    const float normalDelta = 1.0f - std::clamp(
                        QVector3D::dotProduct(center.normal, neighbor.normal),
                        -1.0f, 1.0f);
                    const float sign = delta >= 0.0f ? 1.0f : -1.0f;
                    if (sign > 0.0f) ridgeResponse += normalDelta;
                    else valleyResponse += normalDelta;
                    if (neighbor.objectId != center.objectId)
                        ridgeResponse += 0.25f;
                }
            }
            if (validNeighbors == 0) continue;
            const float invCount = 1.0f / static_cast<float>(validNeighbors);
            occlusion = saturate(occlusion * invCount * valley);
            ridgeResponse = saturate(ridgeResponse * invCount *
                                     curvatureRidge * ridge);
            valleyResponse = saturate(valleyResponse * invCount *
                                      curvatureValley * valley);
            factors[y * width + x] = std::clamp(
                (1.0f - occlusion) * (1.0f + ridgeResponse + valleyResponse),
                0.0f, 4.0f);
        }
    }

    QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
    QImage result = source.copy();
    for (int y = 0; y < outputHeight; ++y) {
        QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        const int depthY = std::clamp(
            qRound((y + 0.5) / colorDpr * dpr), 0, height - 1);
        for (int x = 0; x < outputWidth; ++x) {
            const int depthX = std::clamp(
                qRound((x + 0.5) / colorDpr * dpr), 0, width - 1);
            const float factor = factors[depthY * width + depthX];
            if (factor == 1.0f) continue;
            const QRgb pixel = source.pixel(x, y);
            out[x] = qRgba(
                std::clamp(qRound(qRed(pixel) * factor), 0, 255),
                std::clamp(qRound(qGreen(pixel) * factor), 0, 255),
                std::clamp(qRound(qBlue(pixel) * factor), 0, 255),
                qAlpha(pixel));
        }
    }
    *colorImage = result.convertToFormat(colorImage->format());
}

} // namespace classiCAD
