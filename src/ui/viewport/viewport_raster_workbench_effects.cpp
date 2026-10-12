/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_effects.h"

#include "viewport_depth_geometry.h"
#include "viewport_cavity_blue_noise.h"
#include "core/geometry/polygon_mesh.h"

#include <QByteArray>
#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QSet>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace classiCAD {
namespace {

struct RasterSample {
    float depth = -std::numeric_limits<float>::infinity();
    QVector3D normal;
    quint64 objectId = 0;
};

qreal rasterDofKernelRotation(int x, int y)
{
    static const QByteArray blueNoiseRg = QByteArray::fromBase64(QByteArray(
        detail::kWorkbenchCavityBlueNoiseRgBase64,
        static_cast<qsizetype>(
            sizeof(detail::kWorkbenchCavityBlueNoiseRgBase64) - 1)));
    constexpr int size = 64;
    constexpr int channels = 2;
    constexpr qreal twoPi = 6.283185307179586;
    if (blueNoiseRg.size() != size * size * channels) return 0.0;
    const int index = ((y & (size - 1)) * size + (x & (size - 1))) * channels;
    const qreal noise = static_cast<unsigned char>(blueNoiseRg.at(index)) / 256.0;
    return noise * twoPi;
}

struct TextureAlphaTriangle {
    QImage image;
    std::array<QPointF, 3> uvs{};
    QString interpolation = QStringLiteral("Linear");
    QString extension = QStringLiteral("REPEAT");
    bool cutout = false;
};

QVector3D normalized(const QVector3D &value)
{
    const float length = value.length();
    return std::isfinite(length) && length > 1.0e-12f
               ? value / length
               : QVector3D();
}

QVector3D transformMeshNormal(const std::array<double, 16> &m,
                              const Point3D &normal)
{
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    const double cofactors[9] = {
        e * i - f * h, f * g - d * i, d * h - e * g,
        c * h - b * i, a * i - c * g, b * g - a * h,
        b * f - c * e, c * d - a * f, a * e - b * d};
    const double determinant = a * cofactors[0] + b * cofactors[1] +
                               c * cofactors[2];
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-15)
        return {};
    const QVector3D transformed(
        static_cast<float>((cofactors[0] * normal.x +
                            cofactors[1] * normal.y +
                            cofactors[2] * normal.z) / determinant),
        static_cast<float>((cofactors[3] * normal.x +
                            cofactors[4] * normal.y +
                            cofactors[5] * normal.z) / determinant),
        static_cast<float>((cofactors[6] * normal.x +
                            cofactors[7] * normal.y +
                            cofactors[8] * normal.z) / determinant));
    return normalized(transformed);
}

float saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float objectSourceAlpha(const ViewportRenderObject &object,
                        ViewportColorMode colorMode)
{
    if (colorMode == ViewportColorMode::Custom) return 1.0f;
    if (colorMode == ViewportColorMode::Material ||
        colorMode == ViewportColorMode::Attribute ||
        colorMode == ViewportColorMode::Texture) {
        return object.workbenchMaterial.alpha;
    }
    return object.displayColorSceneLinear.w();
}

bool ignoresTextureAlpha(const QString &alphaMode)
{
    return alphaMode.compare(QStringLiteral("NONE"), Qt::CaseInsensitive) == 0 ||
           alphaMode.compare(QStringLiteral("IGNORE"), Qt::CaseInsensitive) == 0 ||
           alphaMode.compare(QStringLiteral("CHANNEL_PACKED"),
                             Qt::CaseInsensitive) == 0;
}

QImage loadTextureAlphaImage(const WorkbenchMaterialData &material)
{
    QImage image;
    const QString &packed = material.textureImagePackedDataBase64;
    if (!packed.isEmpty()) {
        const QByteArray encoded = packed.toLatin1();
        if (encoded.size() > 44 * 1024 * 1024) return {};
        const QByteArray bytes = QByteArray::fromBase64(encoded);
        if (bytes.size() > 32 * 1024 * 1024) return {};
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) return {};
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
            static_cast<quint64>(size.width()) * size.height() > 67108864ULL) {
            return {};
        }
        image = reader.read();
    } else {
        const QFileInfo info(material.textureImagePath);
        static const QSet<QByteArray> allowedFormats{
            "png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"};
        const QByteArray suffix = info.suffix().toLower().toLatin1();
        if (!info.isFile() || !allowedFormats.contains(suffix) ||
            info.size() <= 0 || info.size() > 256LL * 1024LL * 1024LL) {
            return {};
        }
        QImageReader reader(info.filePath());
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
            static_cast<quint64>(size.width()) * size.height() > 67108864ULL) {
            return {};
        }
        image = reader.read();
    }
    if (image.isNull() || image.width() > 16384 || image.height() > 16384 ||
        static_cast<quint64>(image.width()) * image.height() > 67108864ULL) {
        return {};
    }
    return image.convertToFormat(QImage::Format_RGBA8888);
}

float sampleTextureAlpha(const TextureAlphaTriangle &triangle,
                         const QPointF &uv)
{
    const QImage &image = triangle.image;
    if (image.isNull() || !std::isfinite(uv.x()) || !std::isfinite(uv.y())) {
        return 1.0f;
    }
    const bool repeat = triangle.extension.compare(
        QStringLiteral("REPEAT"), Qt::CaseInsensitive) == 0;
    const bool clip = triangle.extension.compare(
        QStringLiteral("CLIP"), Qt::CaseInsensitive) == 0;
    qreal u = uv.x();
    qreal v = uv.y();
    if (repeat) {
        u -= std::floor(u);
        v -= std::floor(v);
    } else if (!clip) {
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
    } else if (u <= -2.0 / image.width() || u >= 1.0 + 2.0 / image.width() ||
               v <= -2.0 / image.height() || v >= 1.0 + 2.0 / image.height()) {
        return 0.0f;
    }
    const qreal x = u * image.width() - 0.5;
    const qreal y = v * image.height() - 0.5;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const qreal tx = x - x0;
    const qreal ty = y - y0;
    const bool closest = triangle.interpolation.compare(
        QStringLiteral("Closest"), Qt::CaseInsensitive) == 0;
    const auto alphaAt = [&](int px, int py) -> qreal {
        if (clip && (px < 0 || px >= image.width() ||
                     py < 0 || py >= image.height())) return 0.0;
        if (repeat) {
            px = (px % image.width() + image.width()) % image.width();
            py = (py % image.height() + image.height()) % image.height();
        } else {
            px = std::clamp(px, 0, image.width() - 1);
            py = std::clamp(py, 0, image.height() - 1);
        }
        return image.constScanLine(py)[px * 4 + 3] / 255.0;
    };
    if (closest) {
        return static_cast<float>(alphaAt(
            static_cast<int>(std::floor(u * image.width())),
            static_cast<int>(std::floor(v * image.height()))));
    }
    const bool cubic = triangle.interpolation.compare(
        QStringLiteral("Cubic"), Qt::CaseInsensitive) == 0 ||
        triangle.interpolation.compare(
            QStringLiteral("Smart"), Qt::CaseInsensitive) == 0;
    if (cubic) {
        const auto weight = [](qreal t, int offset) {
            const qreal distance = std::abs(t - offset);
            if (distance < 1.0)
                return (4.0 - 6.0 * distance * distance +
                        3.0 * distance * distance * distance) / 6.0;
            if (distance < 2.0) {
                const qreal remainder = 2.0 - distance;
                return remainder * remainder * remainder / 6.0;
            }
            return 0.0;
        };
        qreal result = 0.0;
        for (int j = -1; j <= 2; ++j) {
            for (int i = -1; i <= 2; ++i) {
                result += alphaAt(x0 + i, y0 + j) *
                          weight(tx, i) * weight(ty, j);
            }
        }
        return static_cast<float>(std::clamp(result, 0.0, 1.0));
    }
    const qreal top = alphaAt(x0, y0) * (1.0 - tx) +
                      alphaAt(x0 + 1, y0) * tx;
    const qreal bottom = alphaAt(x0, y0 + 1) * (1.0 - tx) +
                         alphaAt(x0 + 1, y0 + 1) * tx;
    return static_cast<float>(std::clamp(
        top * (1.0 - ty) + bottom * ty, 0.0, 1.0));
}

} // namespace

bool compositeRasterWorkbenchSurfaces(
    QImage *colorImage,
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    bool antialias,
    const std::function<void(QPainter &, const ViewportRenderObject &)> &drawObject)
{
    if (colorImage == nullptr || colorImage->isNull() || objects.isEmpty() ||
        !drawObject || !std::isfinite(devicePixelRatio) ||
        devicePixelRatio <= 0.0) {
        return false;
    }
    const int width = colorImage->width();
    const int height = colorImage->height();
    const qint64 pixelCount64 = qint64(width) * height;
    // Bound the transient CPU buffers. Existing painter rendering remains the
    // fallback if a large supersampled viewport exceeds this budget.
    if (width <= 0 || height <= 0 || pixelCount64 > 1000000) return false;
    for (const ViewportRenderObject &object : objects) {
        if (!object.preparedDepthGeometry ||
            object.preparedDepthGeometry->surfaceVertices.size() < 3 ||
            object.preparedDepthGeometry->surfaceVertices.size() % 3 != 0) {
            return false;
        }
    }
    const int pixelCount = static_cast<int>(pixelCount64);
    const float noDepth = -std::numeric_limits<float>::infinity();
    QVector<float> opaqueDepth(pixelCount, noDepth);
    QVector<QRgb> opaqueColor(pixelCount, qRgba(0, 0, 0, 0));
    QVector<float> transparentRed(pixelCount, 0.0f);
    QVector<float> transparentGreen(pixelCount, 0.0f);
    QVector<float> transparentBlue(pixelCount, 0.0f);
    QVector<float> transparentAlpha(pixelCount, 0.0f);
    const qreal dpr = devicePixelRatio;
    const Point3D cameraPosition = transform.cameraPosition(viewportSize);
    const Point3D viewDirection = transform.viewDirection();

    const auto rasterDepth = [&](const ViewportRenderObject &object,
                                 QVector<float> *depthBuffer) {
        depthBuffer->fill(noDepth, pixelCount);
        if (!object.preparedDepthGeometry) return;
        const QVector<QVector3D> &vertices =
            object.preparedDepthGeometry->surfaceVertices;
        for (int tri = 0; tri + 2 < vertices.size(); tri += 3) {
            QPointF screen[3];
            float viewDepth[3];
            bool valid = true;
            for (int corner = 0; corner < 3; ++corner) {
                const QVector3D &v = vertices[tri + corner];
                const Point3D world(v.x() + object.preparedGeometryOffset.x,
                                    v.y() + object.preparedGeometryOffset.y,
                                    v.z() + object.preparedGeometryOffset.z);
                if (!transform.worldPointToScreen(world, viewportSize,
                                                  &screen[corner])) {
                    valid = false;
                    break;
                }
                if (transform.isPerspectiveEnabled()) {
                    viewDepth[corner] = static_cast<float>(
                        (world.x - cameraPosition.x) * viewDirection.x +
                        (world.y - cameraPosition.y) * viewDirection.y +
                        (world.z - cameraPosition.z) * viewDirection.z);
                } else {
                    viewDepth[corner] = static_cast<float>(
                        transform.worldDirectionToView(world).towardCamera);
                }
            }
            if (!valid) continue;
            const qreal area = (screen[1].x() - screen[0].x()) *
                                   (screen[2].y() - screen[0].y()) -
                               (screen[1].y() - screen[0].y()) *
                                   (screen[2].x() - screen[0].x());
            if (std::abs(area) < 1.0e-12) continue;
            const qreal triangleMinX =
                std::min({screen[0].x(), screen[1].x(), screen[2].x()});
            const qreal triangleMaxX =
                std::max({screen[0].x(), screen[1].x(), screen[2].x()});
            const qreal triangleMinY =
                std::min({screen[0].y(), screen[1].y(), screen[2].y()});
            const qreal triangleMaxY =
                std::max({screen[0].y(), screen[1].y(), screen[2].y()});
            if (triangleMaxX < 0.0 || triangleMinX >= viewportSize.width() ||
                triangleMaxY < 0.0 || triangleMinY >= viewportSize.height()) {
                continue;
            }
            const int minX = std::clamp(
                qFloor(triangleMinX * dpr),
                0, width - 1);
            const int maxX = std::clamp(
                qCeil(triangleMaxX * dpr),
                0, width - 1);
            const int minY = std::clamp(
                qFloor(triangleMinY * dpr),
                0, height - 1);
            const int maxY = std::clamp(
                qCeil(triangleMaxY * dpr),
                0, height - 1);
            for (int y = minY; y <= maxY; ++y) {
                const qreal py = (y + 0.5) / dpr;
                for (int x = minX; x <= maxX; ++x) {
                    const qreal px = (x + 0.5) / dpr;
                    const qreal b1 = ((px - screen[0].x()) *
                                          (screen[2].y() - screen[0].y()) -
                                      (py - screen[0].y()) *
                                          (screen[2].x() - screen[0].x())) / area;
                    const qreal b2 = ((screen[1].x() - screen[0].x()) *
                                          (py - screen[0].y()) -
                                      (screen[1].y() - screen[0].y()) *
                                          (px - screen[0].x())) / area;
                    const qreal b0 = 1.0 - b1 - b2;
                    if (b0 < 0.0 || b1 < 0.0 || b2 < 0.0) continue;
                    qreal z;
                    if (transform.isPerspectiveEnabled()) {
                        const qreal reciprocal =
                            b0 / std::max<qreal>(viewDepth[0], 1.0e-9) +
                            b1 / std::max<qreal>(viewDepth[1], 1.0e-9) +
                            b2 / std::max<qreal>(viewDepth[2], 1.0e-9);
                        if (reciprocal <= 1.0e-12) continue;
                        z = 1.0 / reciprocal;
                    } else {
                        z = b0 * viewDepth[0] + b1 * viewDepth[1] +
                            b2 * viewDepth[2];
                    }
                    float &stored = (*depthBuffer)[y * width + x];
                    stored = std::max(stored, static_cast<float>(z));
                }
            }
        }
    };

    QVector<float> objectDepth(pixelCount, noDepth);
    const auto renderObject = [&](const ViewportRenderObject &object,
                                  auto &&consume) {
        QImage layer(colorImage->size(), QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(colorImage->devicePixelRatioF());
        layer.fill(Qt::transparent);
        QPainter layerPainter(&layer);
        layerPainter.setRenderHint(QPainter::Antialiasing, antialias);
        drawObject(layerPainter, object);
        layerPainter.end();
        rasterDepth(object, &objectDepth);
        consume(layer);
    };

    // Opaque color/depth is established before any translucent fragment.
    for (const ViewportRenderObject &object : objects) {
        renderObject(object, [&](const QImage &layer) {
            for (int y = 0; y < height; ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                for (int x = 0; x < width; ++x) {
                    const int index = y * width + x;
                    const QRgb pixel = row[x];
                    if (qAlpha(pixel) < 254 || objectDepth[index] == noDepth)
                        continue;
                    if (objectDepth[index] > opaqueDepth[index]) {
                        opaqueDepth[index] = objectDepth[index];
                        opaqueColor[index] = pixel;
                    }
                }
            }
        });
    }

    // The caller supplies objects in average-depth back-to-front order. Use
    // source-over for the surviving transparent object layers. Per-pixel
    // depth sorting is unavailable here, so intersecting objects can still
    // disagree with their global average order.
    for (const ViewportRenderObject &object : objects) {
        renderObject(object, [&](const QImage &layer) {
            for (int y = 0; y < height; ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                for (int x = 0; x < width; ++x) {
                    const int index = y * width + x;
                    const QRgb pixel = row[x];
                    const int alphaByte = qAlpha(pixel);
                    if (alphaByte == 0 || alphaByte >= 254 ||
                        objectDepth[index] == noDepth ||
                        objectDepth[index] <= opaqueDepth[index]) {
                        continue;
                    }
                    const float alpha = alphaByte / 255.0f;
                    const float remain = 1.0f - alpha;
                    const float sourceRed = qRed(pixel) / alphaByte;
                    const float sourceGreen = qGreen(pixel) / alphaByte;
                    const float sourceBlue = qBlue(pixel) / alphaByte;
                    transparentRed[index] = sourceRed * alpha +
                        transparentRed[index] * remain;
                    transparentGreen[index] = sourceGreen * alpha +
                        transparentGreen[index] * remain;
                    transparentBlue[index] = sourceBlue * alpha +
                        transparentBlue[index] * remain;
                    transparentAlpha[index] = alpha +
                        transparentAlpha[index] * remain;
                }
            }
        });
    }

    for (int y = 0; y < height; ++y) {
        auto *destination = reinterpret_cast<QRgb *>(colorImage->scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            const float transAlpha = transparentAlpha[index];
            QRgb surface = opaqueColor[index];
            if (transAlpha > 0.0f) {
                const float opaqueAlpha = qAlpha(surface) / 255.0f;
                const float outAlpha = transAlpha + opaqueAlpha * (1.0f - transAlpha);
                const float inverseAlpha = outAlpha > 0.0f
                                               ? 1.0f / outAlpha
                                               : 0.0f;
                const int outR = qRound(
                    (transparentRed[index] * 255.0f +
                     qRed(surface) * (1.0f - transAlpha)) * inverseAlpha);
                const int outG = qRound(
                    (transparentGreen[index] * 255.0f +
                     qGreen(surface) * (1.0f - transAlpha)) * inverseAlpha);
                const int outB = qRound(
                    (transparentBlue[index] * 255.0f +
                     qBlue(surface) * (1.0f - transAlpha)) * inverseAlpha);
                surface = qPremultiply(qRgba(
                    std::clamp(outR, 0, 255),
                    std::clamp(outG, 0, 255),
                    std::clamp(outB, 0, 255),
                    std::clamp(qRound(outAlpha * 255.0f), 0, 255)));
            }
            if (qAlpha(surface) == 0) continue;
            const QRgb old = destination[x];
            const float a = qAlpha(surface) / 255.0f;
            const float oldAlpha = qAlpha(old) / 255.0f;
            const float remain = 1.0f - a;
            destination[x] = qRgba(
                std::clamp(qRound(qRed(surface) + qRed(old) * remain), 0, 255),
                std::clamp(qRound(qGreen(surface) + qGreen(old) * remain), 0, 255),
                std::clamp(qRound(qBlue(surface) + qBlue(old) * remain), 0, 255),
                std::clamp(qRound((a + oldAlpha * remain) * 255.0f), 0, 255));
        }
    }
    return true;
}

void applyRasterWorkbenchEffects(QImage *colorImage,
                                 const QVector<ViewportRenderObject> &objects,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 qreal devicePixelRatio,
                                 const ViewportShadingSettings &settings)
{
    if (colorImage == nullptr || colorImage->isNull() ||
        (!settings.cavity && !settings.outline && !settings.depthOfField) ||
        settings.mode != ViewportShadingMode::Solid || settings.xrayEnabled() ||
        viewportSize.isEmpty()) {
        return;
    }

    // Outlines use all visible geometry. Cavity uses a separate opaque-only
    // depth/normal buffer so transparent surfaces neither occlude its samples
    // nor receive its shading.
    const bool cavityEnabled = settings.cavity;
    const ViewportCameraPreferences cameraOptics = transform.cameraPreferences();
    const bool perspective = transform.isPerspectiveEnabled();
    const bool depthOfFieldEnabled = settings.depthOfField && perspective &&
                                     cameraOptics.sceneCameraEnabled;

    const int outputWidth = colorImage->width();
    const int outputHeight = colorImage->height();
    const qreal colorDpr = std::max<qreal>(colorImage->devicePixelRatio(), 1.0);
    const qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    const int width = qRound(viewportSize.width() * dpr);
    const int height = qRound(viewportSize.height() * dpr);
    if (width <= 0 || height <= 0 || outputWidth <= 0 || outputHeight <= 0)
        return;
    QVector<RasterSample> samples(width * height);
    QVector<RasterSample> opaqueSamples;
    if (cavityEnabled) opaqueSamples.resize(width * height);
    const Point3D towardCamera = transform.viewDirection();
    const Point3D camera = perspective
                               ? transform.cameraPosition(viewportSize)
                               : Point3D{};

    for (const ViewportRenderObject &object : objects) {
        if (object.shape.geometryType != GeometryType::NurbsSurface &&
            object.shape.geometryType != GeometryType::NurbsSolid &&
            !(object.shape.geometryType == GeometryType::PolygonMesh &&
              validatePolygonMesh(object.shape.polygonMesh))) {
            continue;
        }
        const bool perFaceMaterialAlpha =
            object.shape.geometryType == GeometryType::PolygonMesh &&
            (settings.colorMode == ViewportColorMode::Material ||
             settings.colorMode == ViewportColorMode::Attribute ||
             settings.colorMode == ViewportColorMode::Texture);
        if (!perFaceMaterialAlpha &&
            objectSourceAlpha(object, settings.colorMode) <= 0.0f) {
            continue;
        }
        const float objectAlpha = perFaceMaterialAlpha
                                      ? 1.0f
                                      : saturate(objectSourceAlpha(
                                            object, settings.colorMode));
        ViewportDepthGeometry fallback;
        QVector<TextureAlphaTriangle> textureAlphaTriangles;
        QVector<float> triangleAlphas;
        const ViewportDepthGeometry *geometry =
            object.preparedDepthGeometry.data();
        if (object.shape.geometryType == GeometryType::PolygonMesh) {
            // Imported meshes have no NURBS tessellation cache. Expand their
            // triangulated faces into the same per-corner buffers used by
            // the NURBS fallback, applying the scene-object transform first.
            const PolygonMesh3D &mesh = object.shape.polygonMesh;
            const auto &m = object.meshObjectTransform;
            const MeshUvMap *activeUvMap = nullptr;
            if (settings.colorMode == ViewportColorMode::Texture) {
                for (const MeshUvMap &uvMap : mesh.uvMaps) {
                    if (uvMap.name == mesh.activeUvMapName) {
                        activeUvMap = &uvMap;
                        break;
                    }
                }
            }
            QHash<int, QImage> alphaImagesBySlot;
            const auto worldPoint = [&](const Point3D &local) {
                const qreal w = m[12] * local.x + m[13] * local.y +
                                m[14] * local.z + m[15];
                const qreal divisor = std::abs(w) > 1.0e-12 ? w : 1.0;
                return QVector3D(
                    static_cast<float>((m[0] * local.x + m[1] * local.y +
                                        m[2] * local.z + m[3]) / divisor),
                    static_cast<float>((m[4] * local.x + m[5] * local.y +
                                        m[6] * local.z + m[7]) / divisor),
                    static_cast<float>((m[8] * local.x + m[9] * local.y +
                                        m[10] * local.z + m[11]) / divisor));
            };
            int firstCorner = 0;
            for (int faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
                const QVector<int> &face = mesh.faces[faceIndex];
                const int faceFirstCorner = firstCorner;
                const int faceSlot = mesh.faceMaterialIndices.value(faceIndex, 0);
                float triangleSourceAlpha = objectAlpha;
                if (settings.colorMode == ViewportColorMode::Material ||
                    settings.colorMode == ViewportColorMode::Attribute ||
                    settings.colorMode == ViewportColorMode::Texture) {
                    float faceAlpha = object.workbenchMaterial.alpha;
                    if (faceSlot >= 0 && faceSlot < object.meshMaterials.size()) {
                        faceAlpha = object.meshMaterials[faceSlot].alpha;
                    } else if (faceSlot >= 0 && faceSlot < mesh.materialSlots.size()) {
                        faceAlpha = mesh.materialSlots[faceSlot].alpha;
                    }
                    if (faceAlpha <= 0.0f) {
                        firstCorner += face.size();
                        continue;
                    }
                    faceAlpha = saturate(faceAlpha);
                    triangleSourceAlpha = faceAlpha;
                }
                QImage alphaImage;
                bool textureAlphaCutout = false;
                if (settings.colorMode == ViewportColorMode::Texture &&
                    activeUvMap != nullptr && faceSlot >= 0 &&
                    faceSlot < object.meshMaterials.size() &&
                    faceFirstCorner + face.size() <=
                        activeUvMap->cornerValues.size()) {
                    const WorkbenchMaterialData &material =
                        object.meshMaterials[faceSlot];
                    if (!material.textureImagePath.isEmpty() ||
                        !material.textureImagePackedDataBase64.isEmpty()) {
                        if (!alphaImagesBySlot.contains(faceSlot)) {
                            alphaImagesBySlot.insert(
                                faceSlot, loadTextureAlphaImage(material));
                        }
                        alphaImage = alphaImagesBySlot.value(faceSlot);
                        textureAlphaCutout = !alphaImage.isNull() &&
                            !ignoresTextureAlpha(
                                material.textureImageAlphaMode);
                    }
                }
                QVector<Point3D> points;
                points.reserve(face.size());
                for (int vertex : face) points.append(mesh.vertices[vertex]);
                for (const std::array<int, 3> &triangle :
                     triangulatePolygonLoop(points)) {
                    const QVector3D a = worldPoint(points[triangle[0]]);
                    const QVector3D b = worldPoint(points[triangle[1]]);
                    const QVector3D c = worldPoint(points[triangle[2]]);
                    const QVector3D normal = normalized(
                        QVector3D::crossProduct(b - a, c - a));
                    fallback.surfaceVertices << a << b << c;
                    for (int corner = 0; corner < 3; ++corner) {
                        QVector3D cornerNormal;
                        const int loopIndex = faceFirstCorner + triangle[corner];
                        if (loopIndex >= 0 && loopIndex < mesh.cornerNormals.size()) {
                            cornerNormal = transformMeshNormal(
                                m, mesh.cornerNormals[loopIndex]);
                        }
                        if (cornerNormal.lengthSquared() <= 1.0e-16f)
                            cornerNormal = normal;
                        fallback.surfaceNormals.append(cornerNormal);
                    }
                    TextureAlphaTriangle textureAlphaTriangle;
                    if (textureAlphaCutout) {
                        textureAlphaTriangle.image = alphaImage;
                        textureAlphaTriangle.interpolation =
                            object.meshMaterials[faceSlot].textureImageInterpolation;
                        textureAlphaTriangle.extension =
                            object.meshMaterials[faceSlot].textureImageExtension;
                        textureAlphaTriangle.cutout = true;
                        for (int corner = 0; corner < 3; ++corner) {
                            const QPointF uv = activeUvMap->cornerValues[
                                faceFirstCorner + triangle[corner]];
                            textureAlphaTriangle.uvs[corner] =
                                QPointF(uv.x(), 1.0 - uv.y());
                        }
                    }
                    textureAlphaTriangles.append(
                        std::move(textureAlphaTriangle));
                    triangleAlphas.append(triangleSourceAlpha);
                }
                firstCorner += face.size();
            }
            geometry = &fallback;
        }
        if (geometry == nullptr) {
            fallback = buildViewportDepthGeometry(object);
            geometry = &fallback;
        }
        const QVector<QVector3D> &vertices = geometry->surfaceVertices;
        const QVector<QVector3D> &normals = geometry->surfaceNormals;
        if (settings.colorMode == ViewportColorMode::Texture &&
            object.shape.geometryType == GeometryType::NurbsSurface &&
            object.shape.nurbsSurface.trimLoops.isEmpty() &&
            geometry->surfaceTextureCoordinates.size() == vertices.size() &&
            (!object.workbenchMaterial.textureImagePath.isEmpty() ||
             !object.workbenchMaterial.textureImagePackedDataBase64.isEmpty()) &&
            !ignoresTextureAlpha(
                object.workbenchMaterial.textureImageAlphaMode)) {
            const QImage alphaImage = loadTextureAlphaImage(
                object.workbenchMaterial);
            if (!alphaImage.isNull()) {
                textureAlphaTriangles.resize(vertices.size() / 3);
                for (int triangle = 0; triangle + 2 < vertices.size();
                     triangle += 3) {
                    TextureAlphaTriangle &alphaTriangle =
                        textureAlphaTriangles[triangle / 3];
                    alphaTriangle.image = alphaImage;
                    alphaTriangle.interpolation =
                        object.workbenchMaterial.textureImageInterpolation;
                    alphaTriangle.extension =
                        object.workbenchMaterial.textureImageExtension;
                    alphaTriangle.cutout = true;
                    for (int corner = 0; corner < 3; ++corner) {
                        const QPointF uv =
                            geometry->surfaceTextureCoordinates[triangle + corner];
                        alphaTriangle.uvs[corner] =
                            QPointF(uv.x(), 1.0 - uv.y());
                    }
                }
            }
        }
        if (object.shape.geometryType != GeometryType::PolygonMesh) {
            triangleAlphas.fill(objectAlpha, vertices.size() / 3);
        }
        for (int triangle = 0; triangle + 2 < vertices.size(); triangle += 3) {
            const int triangleIndex = triangle / 3;
            const TextureAlphaTriangle *textureAlphaTriangle =
                triangleIndex >= 0 &&
                        triangleIndex < textureAlphaTriangles.size()
                    ? &textureAlphaTriangles[triangleIndex]
                    : nullptr;
            QVector3D world[3];
            QVector3D normal[3];
            QPointF screen[3];
            float depth[3];
            qreal reciprocalDepth[3] = {1.0, 1.0, 1.0};
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
                if (perspective) {
                    const qreal cameraDepth =
                        (point.x - camera.x) * towardCamera.x +
                        (point.y - camera.y) * towardCamera.y +
                        (point.z - camera.z) * towardCamera.z;
                    reciprocalDepth[corner] =
                        1.0 / std::max<qreal>(cameraDepth, 1.0e-9);
                }
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
            const quint64 id = object.objectId.value() |
                               (object.selected ? 0x80000000ull : 0ull);
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
                    if (textureAlphaTriangle != nullptr &&
                        textureAlphaTriangle->cutout) {
                        const qreal corrected0 = w0 * reciprocalDepth[0];
                        const qreal corrected1 = w1 * reciprocalDepth[1];
                        const qreal corrected2 = w2 * reciprocalDepth[2];
                        const qreal denominator =
                            corrected0 + corrected1 + corrected2;
                        if (std::abs(denominator) <= 1.0e-12) continue;
                        const QPointF uv =
                            (textureAlphaTriangle->uvs[0] * corrected0 +
                             textureAlphaTriangle->uvs[1] * corrected1 +
                             textureAlphaTriangle->uvs[2] * corrected2) /
                            denominator;
                        if (sampleTextureAlpha(*textureAlphaTriangle, uv) < 0.1f) {
                            continue;
                        }
                    }
                    const float z = static_cast<float>(w0 * depth[0] +
                                                       w1 * depth[1] +
                                                       w2 * depth[2]);
                    RasterSample &sample = samples[y * width + x];
                    const QVector3D interpolatedNormal = normalized(
                        normal[0] * static_cast<float>(w0) +
                        normal[1] * static_cast<float>(w1) +
                        normal[2] * static_cast<float>(w2));
                    const auto writeNearest = [&](RasterSample &target) {
                        if (target.objectId != 0 && z <= target.depth) return;
                        target.depth = z;
                        target.normal = interpolatedNormal;
                        target.objectId = id;
                    };
                    writeNearest(sample);
                    const float sourceAlpha = triangleIndex < triangleAlphas.size()
                                                  ? triangleAlphas[triangleIndex]
                                                  : objectAlpha;
                    if (cavityEnabled && sourceAlpha >= 0.999f)
                        writeNearest(opaqueSamples[y * width + x]);
                }
            }
        }
    }

    if (settings.outline) {
        QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage result = source.copy();
        constexpr quint64 selectedIdMask = 0x80000000ull;
        const QColor outline = settings.outlineColor;
        const QColor selectionOutline = viewportSelectionColor();
        for (int y = 0; y < outputHeight; ++y) {
            QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
            const int depthY = std::clamp(
                static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
                0, height - 1);
            for (int x = 0; x < outputWidth; ++x) {
                const int depthX = std::clamp(
                    static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                    0, width - 1);
                const quint64 objectId = samples[depthY * width + depthX].objectId;
                int differentNeighbors = 0;
                bool selectedEdge = (objectId & selectedIdMask) != 0;
                constexpr std::array<QPoint, 8> neighbors = {
                    QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1),
                    QPoint(1, 1), QPoint(1, -1), QPoint(-1, 1), QPoint(-1, -1)};
                for (const QPoint &offset : neighbors) {
                    const int sx = std::clamp(depthX + offset.x(), 0, width - 1);
                    const int sy = std::clamp(depthY + offset.y(), 0, height - 1);
                    const quint64 neighborId =
                        samples[sy * width + sx].objectId;
                    differentNeighbors += neighborId != objectId;
                    selectedEdge = selectedEdge ||
                                   (neighborId & selectedIdMask) != 0;
                }
                const float edgeOpacity = differentNeighbors / 8.0f;
                const QColor &edgeColor = selectedEdge
                                              ? selectionOutline : outline;
                const float sourceAlpha =
                    static_cast<float>(edgeColor.alphaF()) * edgeOpacity;
                if (sourceAlpha <= 0.0f) continue;
                const float outlineRed = static_cast<float>(edgeColor.red());
                const float outlineGreen = static_cast<float>(edgeColor.green());
                const float outlineBlue = static_cast<float>(edgeColor.blue());
                const QRgb pixel = source.pixel(x, y);
                const float dstAlpha = qAlpha(pixel) / 255.0f;
                const float outAlpha = sourceAlpha +
                                       dstAlpha * (1.0f - sourceAlpha);
                const auto blend = [&](int dst, float src) {
                    if (outAlpha <= 0.0f) return 0;
                    return std::clamp(qRound(
                        (src * sourceAlpha + dst * dstAlpha *
                         (1.0f - sourceAlpha)) / outAlpha), 0, 255);
                };
                out[x] = qRgba(blend(qRed(pixel), outlineRed),
                               blend(qGreen(pixel), outlineGreen),
                               blend(qBlue(pixel), outlineBlue),
                               std::clamp(qRound(outAlpha * 255.0f), 0, 255));
            }
        }
        *colorImage = result.convertToFormat(colorImage->format());
    }

    if (cavityEnabled) {

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
            const RasterSample &center = opaqueSamples[y * width + x];
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
                const RasterSample &neighbor = opaqueSamples[sy * width + sx];
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
            static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
            0, height - 1);
        for (int x = 0; x < outputWidth; ++x) {
            const int depthX = std::clamp(
                static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                0, width - 1);
            const int sampleIndex = depthY * width + depthX;
            const RasterSample &opaqueReceiver = opaqueSamples[sampleIndex];
            const RasterSample &visibleTop = samples[sampleIndex];
            if (opaqueReceiver.objectId == 0 || visibleTop.objectId == 0)
                continue;
            const float depthTolerance = std::max(
                1.0e-5f, std::abs(opaqueReceiver.depth) * 1.0e-6f);
            // The source image has already composited transparency. Applying
            // an opaque cavity factor beneath a translucent visible surface
            // would also darken that transparent contribution, so only shade
            // when this opaque receiver is itself the visible top sample.
            if (std::abs(visibleTop.depth - opaqueReceiver.depth) >
                depthTolerance) {
                continue;
            }
            const float factor = factors[sampleIndex];
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

    // QPainter fallback approximation: use the software-resolved visible
    // surface depth to estimate a physical circle of confusion, then gather
    // a small disk of same-or-behind-depth colors. This is intentionally
    // camera-gated and runs only for perspective scene-camera optics.
    if (depthOfFieldEnabled) {
        const QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage dofResult = source.copy();
        constexpr std::array<QPointF, 12> disk{{
            {0.0, 0.0}, {0.527, 0.085}, {-0.040, 0.536},
            {-0.670, -0.179}, {0.244, -0.647}, {0.765, 0.429},
            {-0.315, 0.847}, {-0.897, 0.310}, {0.493, 0.749},
            {0.901, -0.312}, {-0.527, -0.757}, {0.089, -0.956}}};
        const qreal focal = cameraOptics.focalLengthMillimeters;
        const qreal focus = cameraOptics.focusDistance;
        const qreal fStop = cameraOptics.fStop;
        const qreal sensorWidth = std::max<qreal>(cameraOptics.sensorWidthMillimeters,
                                                   1.0e-6);
        const auto shapeApertureTap = [&cameraOptics](QPointF tap) {
            if (cameraOptics.apertureBlades >= 3) {
                const qreal bladeAngle =
                    6.283185307179586 / cameraOptics.apertureBlades;
                const qreal angle = std::atan2(tap.y(), tap.x()) -
                                    cameraOptics.apertureRotationRadians;
                const qreal sector = std::remainder(angle, bladeAngle);
                const qreal polygonRadius = std::cos(0.5 * bladeAngle) /
                    std::max<qreal>(std::cos(sector), 1.0e-5);
                tap *= polygonRadius;
            }
            const qreal ratio = std::max<qreal>(cameraOptics.apertureRatio, 0.01);
            if (ratio > 1.0) tap.setX(tap.x() * ratio);
            else tap.setY(tap.y() / ratio);
            return tap;
        };
        if (std::isfinite(focal) && focal > 0.0 && std::isfinite(focus) &&
            focus > focal && std::isfinite(fStop) && fStop > 0.0) {
            for (int y = 0; y < outputHeight; ++y) {
                QRgb *out = reinterpret_cast<QRgb *>(dofResult.scanLine(y));
                const int depthY = std::clamp(
                    static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
                    0, height - 1);
                for (int x = 0; x < outputWidth; ++x) {
                    const int depthX = std::clamp(
                        static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                        0, width - 1);
                    const RasterSample &center = samples[depthY * width + depthX];
                    if (center.objectId == 0 || center.depth <= 0.0f) continue;
                    const qreal z = center.depth;
                    const qreal cocMm = std::abs(
                        (focal * focal / (fStop * (focus - focal))) *
                        ((z - focus) / std::max<qreal>(z, 1.0e-6)));
                    const qreal radius = std::clamp(
                        cocMm * outputWidth / sensorWidth, 0.0, 12.0) * colorDpr;
                    if (radius < 0.75 * colorDpr) continue;
                    qreal red = 0.0, green = 0.0, blue = 0.0;
                    int count = 0;
                    // QPainter has no temporal AA sample sequence to rotate
                    // this kernel between accumulated frames. Use the shared
                    // Workbench blue-noise LUT for a stable per-pixel rotation.
                    const qreal kernelAngle = rasterDofKernelRotation(x, y);
                    const qreal kernelCos = std::cos(kernelAngle);
                    const qreal kernelSin = std::sin(kernelAngle);
                    for (const QPointF &diskTap : disk) {
                        const QPointF rotatedTap(
                            diskTap.x() * kernelCos - diskTap.y() * kernelSin,
                            diskTap.x() * kernelSin + diskTap.y() * kernelCos);
                        const QPointF tap = shapeApertureTap(rotatedTap);
                        const int sx = std::clamp(qRound(x + tap.x() * radius),
                                                  0, outputWidth - 1);
                        const int sy = std::clamp(qRound(y + tap.y() * radius),
                                                  0, outputHeight - 1);
                        const int sampleX = std::clamp(
                            static_cast<int>(std::floor((sx + 0.5) / colorDpr * dpr)),
                            0, width - 1);
                        const int sampleY = std::clamp(
                            static_cast<int>(std::floor((sy + 0.5) / colorDpr * dpr)),
                            0, height - 1);
                        const RasterSample &neighbor = samples[sampleY * width + sampleX];
                        if (neighbor.objectId != 0 &&
                            neighbor.depth < center.depth -
                                std::max(1.0e-4f, std::abs(center.depth) * 0.002f))
                            continue;
                        const QRgb pixel = source.pixel(sx, sy);
                        red += qRed(pixel); green += qGreen(pixel); blue += qBlue(pixel);
                        ++count;
                    }
                    if (count == 0) continue;
                    const QRgb original = source.pixel(x, y);
                    const qreal mix = std::clamp(radius / (6.0 * colorDpr), 0.0, 1.0);
                    out[x] = qRgba(
                        qRound(qRed(original) * (1.0 - mix) + red / count * mix),
                        qRound(qGreen(original) * (1.0 - mix) + green / count * mix),
                        qRound(qBlue(original) * (1.0 - mix) + blue / count * mix),
                        qAlpha(original));
                }
            }
            *colorImage = dofResult.convertToFormat(colorImage->format());
        }
    }
}

} // namespace classiCAD
