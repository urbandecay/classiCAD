/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_effects.h"
#include "viewport_workbench_effect_scale.h"
#include "workbench_lighting.h"

#include "viewport_depth_geometry.h"
#include "viewport_cavity_blue_noise.h"
#include "core/geometry/polygon_mesh.h"

#include <QColor>
#include <QByteArray>
#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QSet>
#include <QVector3D>
#include <QtCore/qfloat16.h>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace classiCAD {
namespace {

struct RasterSample {
    float depth = -std::numeric_limits<float>::infinity();
    QVector3D position;
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

float rasterCavityNoise(int x, int y, int channel)
{
    static const QByteArray blueNoiseRg = QByteArray::fromBase64(QByteArray(
        detail::kWorkbenchCavityBlueNoiseRgBase64,
        static_cast<qsizetype>(
            sizeof(detail::kWorkbenchCavityBlueNoiseRgBase64) - 1)));
    constexpr int size = 64;
    constexpr int channels = 2;
    if (blueNoiseRg.size() != size * size * channels) return 0.5f;
    const int index = ((y & (size - 1)) * size + (x & (size - 1))) * channels +
                      std::clamp(channel, 0, channels - 1);
    return static_cast<unsigned char>(blueNoiseRg.at(index)) / 256.0f;
}

float radicalInverseVdc(unsigned int bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

struct TextureAlphaTriangle {
    QImage image;
    QHash<int, QImage> udimImages;
    struct FloatImage { int width = 0; int height = 0; QVector<float> rgba; };
    FloatImage floatImage;
    QHash<int, FloatImage> floatUdimImages;
    bool udim = false;
    std::array<QPointF, 3> uvs{};
    QString interpolation = QStringLiteral("Linear");
    QString extension = QStringLiteral("REPEAT");
    bool cutout = false;
};

TextureAlphaTriangle::FloatImage loadFloatAlphaPayload(const QString &encoded,
                                                        int width, int height)
{
    TextureAlphaTriangle::FloatImage result;
    if (encoded.isEmpty() || width <= 0 || height <= 0 ||
        quint64(width) * quint64(height) * 16ULL > 32ULL * 1024ULL * 1024ULL ||
        encoded.size() > 44 * 1024 * 1024) return result;
    const QByteArray bytes = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (quint64(bytes.size()) != quint64(width) * quint64(height) * 16ULL)
        return result;
    result.width = width;
    result.height = height;
    result.rgba.resize(width * height * 4);
    for (int i = 0; i < result.rgba.size(); ++i) {
        const auto *source = reinterpret_cast<const uchar *>(bytes.constData() + i * 4);
        result.rgba[i] = std::bit_cast<float>(qFromLittleEndian<quint32>(source));
        if (!std::isfinite(result.rgba[i])) return {};
    }
    return result;
}

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
        return object.hasWorkbenchMaterial
                   ? object.workbenchMaterial.alpha
                   : 1.0f;
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

QImage loadTextureAlphaTile(const DocumentImageTextureTile &tile,
                            quint64 *decodedPixels,
                            quint64 *packedBytes)
{
    WorkbenchMaterialData material;
    material.textureImagePath = tile.filepath;
    material.textureImagePackedDataBase64 = tile.packedDataBase64;
    const QByteArray encoded = tile.packedDataBase64.toLatin1();
    const quint64 payloadSize = static_cast<quint64>(encoded.size());
    if (payloadSize > 44ULL * 1024 * 1024 ||
        *packedBytes + payloadSize > 44ULL * 1024 * 1024) return {};
    QImage image = loadTextureAlphaImage(material);
    if (image.isNull()) return {};
    const quint64 pixels = static_cast<quint64>(image.width()) * image.height();
    if (*decodedPixels + pixels > 67108864ULL) return {};
    *decodedPixels += pixels;
    *packedBytes += payloadSize;
    return image;
}

QHash<int, QImage> loadTextureAlphaTiles(
    const WorkbenchMaterialData &material)
{
    QHash<int, QImage> images;
    quint64 decodedPixels = 0;
    quint64 packedBytes = 0;
    for (const DocumentImageTextureTile &tile : material.textureImageTiles) {
        if (tile.number < 1001 || tile.number > 1999 ||
            images.contains(tile.number)) continue;
        const QImage image = loadTextureAlphaTile(tile, &decodedPixels,
                                                   &packedBytes);
        if (!image.isNull()) images.insert(tile.number, image);
    }
    return images;
}

QHash<int, TextureAlphaTriangle::FloatImage> loadFloatTextureAlphaTiles(
    const WorkbenchMaterialData &material)
{
    QHash<int, TextureAlphaTriangle::FloatImage> images;
    quint64 totalBytes = 0;
    for (const DocumentImageTextureTile &tile : material.textureImageTiles) {
        if (tile.number < 1001 || tile.number > 1999 || images.contains(tile.number))
            continue;
        auto image = loadFloatAlphaPayload(tile.sceneLinearFloatDataBase64,
            tile.sceneLinearFloatWidth, tile.sceneLinearFloatHeight);
        const quint64 bytes = quint64(image.rgba.size()) * sizeof(float);
        if (image.width > 0 && totalBytes + bytes <= 32ULL * 1024ULL * 1024ULL) {
            totalBytes += bytes;
            images.insert(tile.number, std::move(image));
        }
    }
    return images;
}

float sampleTextureAlpha(const TextureAlphaTriangle &triangle,
                         const QPointF &uv)
{
    QPointF sampleUv = uv;
    const QImage *imagePointer = &triangle.image;
    const TextureAlphaTriangle::FloatImage *floatPointer =
        triangle.floatImage.width > 0 ? &triangle.floatImage : nullptr;
    if (triangle.udim) {
        if (!std::isfinite(uv.x()) || !std::isfinite(uv.y()) ||
            uv.x() < 0.0 || uv.y() < 0.0 || uv.x() >= 100000.0 ||
            uv.y() >= 100000.0) return 1.0f;
        const int tileU = static_cast<int>(std::floor(uv.x()));
        const int tileV = static_cast<int>(std::floor(uv.y()));
        if (tileU < 0 || tileU > 9 || tileV < 0 || tileV > 99)
            return 1.0f;
        const qint64 number = 1001LL + tileU + 10LL * tileV;
        if (number < 1001 || number > 1999) return 1.0f;
        const auto found = triangle.udimImages.constFind(static_cast<int>(number));
        const auto floatFound = triangle.floatUdimImages.constFind(static_cast<int>(number));
        if (floatFound != triangle.floatUdimImages.cend()) {
            floatPointer = &floatFound.value();
        } else if (found != triangle.udimImages.cend()) {
            imagePointer = &found.value();
            floatPointer = nullptr;
        } else return 1.0f;
        sampleUv = QPointF(uv.x() - tileU, 1.0 - (uv.y() - tileV));
    }
    const int imageWidth = floatPointer != nullptr ? floatPointer->width : imagePointer->width();
    const int imageHeight = floatPointer != nullptr ? floatPointer->height : imagePointer->height();
    if (imageWidth <= 0 || imageHeight <= 0 ||
        !std::isfinite(uv.x()) || !std::isfinite(uv.y())) {
        return 1.0f;
    }
    const bool repeat = triangle.extension.compare(
        QStringLiteral("REPEAT"), Qt::CaseInsensitive) == 0;
    const bool mirror = triangle.extension.compare(
        QStringLiteral("MIRROR"), Qt::CaseInsensitive) == 0;
    const bool clip = triangle.extension.compare(
        QStringLiteral("CLIP"), Qt::CaseInsensitive) == 0;
    qreal u = sampleUv.x();
    qreal v = sampleUv.y();
    if (repeat) {
        u -= std::floor(u);
        v -= std::floor(v);
    } else if (mirror) {
        // Reduce to one mirrored pair before converting coordinates to
        // integer texels; this both preserves the extension mode at arbitrary
        // UV magnitudes and avoids overflowing the texel index conversion.
        u = std::fmod(u, 2.0);
        v = std::fmod(v, 2.0);
        if (u < 0.0) u += 2.0;
        if (v < 0.0) v += 2.0;
    } else if (!clip && !mirror) {
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
    } else if (clip && (u <= -2.0 / imageWidth ||
               u >= 1.0 + 2.0 / imageWidth ||
               v <= -2.0 / imageHeight ||
               v >= 1.0 + 2.0 / imageHeight)) {
        return 0.0f;
    }
    const qreal x = u * imageWidth - 0.5;
    const qreal sourceV = floatPointer != nullptr ? 1.0 - v : v;
    const qreal y = sourceV * imageHeight - 0.5;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const qreal tx = x - x0;
    const qreal ty = y - y0;
    const bool closest = triangle.interpolation.compare(
        QStringLiteral("Closest"), Qt::CaseInsensitive) == 0;
    const auto alphaAt = [&](int px, int py) -> qreal {
        if (clip && (px < 0 || px >= imageWidth ||
                     py < 0 || py >= imageHeight)) return 0.0;
        if (mirror) {
            px = (px % (2 * imageWidth) + 2 * imageWidth) % (2 * imageWidth);
            py = (py % (2 * imageHeight) + 2 * imageHeight) % (2 * imageHeight);
            if (px >= imageWidth) px = 2 * imageWidth - 1 - px;
            if (py >= imageHeight) py = 2 * imageHeight - 1 - py;
        } else if (repeat) {
            px = (px % imageWidth + imageWidth) % imageWidth;
            py = (py % imageHeight + imageHeight) % imageHeight;
        } else {
            px = std::clamp(px, 0, imageWidth - 1);
            py = std::clamp(py, 0, imageHeight - 1);
        }
        return floatPointer != nullptr
            ? floatPointer->rgba[(py * imageWidth + px) * 4 + 3]
            : imagePointer->constScanLine(py)[px * 4 + 3] / 255.0;
    };
    if (closest) {
        return static_cast<float>(alphaAt(
            static_cast<int>(std::floor(u * imageWidth)),
            static_cast<int>(std::floor(sourceV * imageHeight))));
    }
    // Workbench only distinguishes Closest from filtered sampling. Its image
    // material setup enables the GPU's linear sampler for Linear, Cubic, and
    // Smart, so effect-alpha must use the same bilinear result as color.
    const qreal top = alphaAt(x0, y0) * (1.0 - tx) +
                      alphaAt(x0 + 1, y0) * tx;
    const qreal bottom = alphaAt(x0, y0 + 1) * (1.0 - tx) +
                         alphaAt(x0 + 1, y0 + 1) * tx;
    return static_cast<float>(std::clamp(
        top * (1.0 - ty) + bottom * ty, 0.0, 1.0));
}

} // namespace

float rasterWorkbenchCameraDepth(const QVector3D &position,
                                 const Point3D &cameraPosition,
                                 const Point3D &viewDirection) noexcept
{
    return -static_cast<float>(
        (position.x() - cameraPosition.x) * viewDirection.x +
        (position.y() - cameraPosition.y) * viewDirection.y +
        (position.z() - cameraPosition.z) * viewDirection.z);
}

QVector3D rasterWorkbenchPrepassNormal(const QVector3D &worldNormal,
                                       bool frontFacing,
                                       const QVector3D &viewRight,
                                       const QVector3D &viewUp,
                                       const QVector3D &viewFacing) noexcept
{
    QVector3D viewNormal(QVector3D::dotProduct(worldNormal, viewRight),
                         QVector3D::dotProduct(worldNormal, viewUp),
                         QVector3D::dotProduct(worldNormal, viewFacing));
    if (viewNormal.lengthSquared() <= 1.0e-20f) return {};
    viewNormal.normalize();
    if (!frontFacing) viewNormal = -viewNormal;

    // workbench_common.bsl.hh normal_encode(), with RG16F render-target
    // precision applied before the matching normal_decode().
    const float p = std::sqrt(std::max(0.0f, viewNormal.z() * 8.0f + 8.0f));
    if (p <= 1.0e-10f) return {};
    const float encodedX = float(qfloat16(viewNormal.x() / p + 0.5f));
    const float encodedY = float(qfloat16(viewNormal.y() / p + 0.5f));
    const float fx = encodedX * 4.0f - 2.0f;
    const float fy = encodedY * 4.0f - 2.0f;
    const float f = fx * fx + fy * fy;
    const float g = std::sqrt(std::max(0.0f, 1.0f - f / 4.0f));
    const QVector3D decodedView(fx * g, fy * g, 1.0f - f / 2.0f);
    return viewRight * decodedView.x() + viewUp * decodedView.y() +
           viewFacing * decodedView.z();
}

QPointF rasterWorkbenchApertureTap(QPointF tap, int apertureBlades,
                                   qreal apertureRotationRadians,
                                   qreal apertureRatio) noexcept
{
    if (apertureBlades > 1) {
        const qreal bladeAngle =
            6.283185307179586 / apertureBlades;
        const qreal angle = std::atan2(tap.y(), tap.x());
        const qreal sector = std::remainder(angle, bladeAngle);
        const qreal polygonRadius = std::cos(0.5 * bladeAngle) /
                                    std::cos(sector);
        tap *= polygonRadius;
    }
    const qreal ratio = std::max<qreal>(apertureRatio, 0.01);
    const qreal angle = std::atan2(tap.y(), tap.x()) +
                        apertureRotationRadians;
    const qreal radius = std::hypot(tap.x(), tap.y());
    return QPointF(std::cos(angle) * radius / ratio,
                   std::sin(angle) * radius);
}

bool compositeRasterWorkbenchSurfaces(
    QImage *colorImage,
    const QVector<ViewportRenderObject> &objects,
    const ViewportTransform &transform,
    const QSize &viewportSize,
    qreal devicePixelRatio,
    bool antialias,
    bool useWeightedOit,
    const ViewportShadingSettings &shadingSettings,
    const std::optional<QVector3D> &sceneLinearBackground,
    const std::function<void(QPainter &, const ViewportRenderObject &,
                             RasterWorkbenchFragmentSink *)> &drawObject,
    RasterWorkbenchSurfaceLayers *surfaceLayers,
    const std::vector<QVector3D> &sceneLinearBackgroundPixels)
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
    if (surfaceLayers != nullptr) {
        surfaceLayers->cavityAppliedBeforeTransparency = false;
        surfaceLayers->opaqueColor = QImage(
            colorImage->size(), QImage::Format_ARGB32_Premultiplied);
        surfaceLayers->opaqueColor.setDevicePixelRatio(
            colorImage->devicePixelRatioF());
        surfaceLayers->opaqueColor.fill(Qt::transparent);
        surfaceLayers->transparentColor = QImage(
            colorImage->size(), QImage::Format_ARGB32_Premultiplied);
        surfaceLayers->transparentColor.setDevicePixelRatio(
            colorImage->devicePixelRatioF());
        surfaceLayers->transparentColor.fill(Qt::transparent);
    }
    const float noDepth = -std::numeric_limits<float>::infinity();
    QVector<float> opaqueDepth(pixelCount, noDepth);
    QVector<QRgb> opaqueColor(pixelCount, qRgba(0, 0, 0, 0));
    QVector<int> opaqueObject(pixelCount, -1);
    QVector<float> opaqueSceneDepth(pixelCount, noDepth);
    std::vector<RasterWorkbenchLinearPixel> opaqueScenePixels(
        static_cast<std::size_t>(pixelCount));
    QVector<float> frontOpaqueDepth(pixelCount, noDepth);
    QVector<QRgb> frontOpaqueColor(pixelCount, qRgba(0, 0, 0, 0));
    QVector<int> frontOpaqueObject(pixelCount, -1);
    QVector<float> frontOpaqueSceneDepth(pixelCount, noDepth);
    std::vector<RasterWorkbenchLinearPixel> frontOpaqueScenePixels(
        static_cast<std::size_t>(pixelCount));
    struct TransparentFragment {
        int pixelIndex = 0;
        float depth = 0.0f;
        int objectOrder = 0;
        bool inFront = false;
        QRgb color = 0;
    };
    // Bound storage independently from viewport size. If the visible layer
    // count exceeds this budget, let the caller use its existing sorted path.
    constexpr qsizetype kMaximumTransparentFragments = 4'000'000;
    QVector<TransparentFragment> regularTransparentFragments;
    QVector<TransparentFragment> frontTransparentFragments;
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
                                  RasterWorkbenchFragmentSink *fragmentSink,
                                  auto &&consume) {
        QImage layer(colorImage->size(), QImage::Format_ARGB32_Premultiplied);
        layer.setDevicePixelRatio(colorImage->devicePixelRatioF());
        layer.fill(Qt::transparent);
        QPainter layerPainter(&layer);
        layerPainter.setRenderHint(QPainter::Antialiasing, antialias);
        drawObject(layerPainter, object, fragmentSink);
        layerPainter.end();
        rasterDepth(object, &objectDepth);
        consume(layer);
    };

    // Compute cavity while opaque depth and normals are available. Weighted
    // OIT consumes this in its scene-linear opaque buffer; the sorted fallback
    // applies it to its retained opaque layer before transparency composition.
    std::vector<float> regularCavityFactors;
    std::vector<float> inFrontCavityFactors;
    if (shadingSettings.cavity &&
        (useWeightedOit || surfaceLayers != nullptr)) {
        ViewportShadingSettings cavityOnly = shadingSettings;
        cavityOnly.outline = false;
        cavityOnly.depthOfField = false;
        QVector<ViewportRenderObject> regularObjects;
        QVector<ViewportRenderObject> inFrontObjects;
        regularObjects.reserve(objects.size());
        inFrontObjects.reserve(objects.size());
        for (const ViewportRenderObject &object : objects) {
            (object.showInFront ? inFrontObjects : regularObjects)
                .append(object);
        }
        const auto computeCavityFactors = [&](
            const QVector<ViewportRenderObject> &domainObjects,
            std::vector<float> *factors) {
            if (domainObjects.isEmpty()) return;
            QImage cavityProbe(colorImage->size(),
                               QImage::Format_ARGB32_Premultiplied);
            cavityProbe.setDevicePixelRatio(colorImage->devicePixelRatioF());
            cavityProbe.fill(Qt::white);
            (void)applyRasterWorkbenchEffects(
                &cavityProbe, domainObjects, transform, viewportSize,
                devicePixelRatio, cavityOnly, nullptr, factors);
        };
        computeCavityFactors(regularObjects, &regularCavityFactors);
        computeCavityFactors(inFrontObjects, &inFrontCavityFactors);
    }

    // Opaque color/depth is established before any translucent fragment.
    for (int objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ViewportRenderObject &object = objects[objectIndex];
        QVector<float> &domainOpaqueDepth = object.showInFront
            ? frontOpaqueDepth : opaqueDepth;
        QVector<QRgb> &domainOpaqueColor = object.showInFront
            ? frontOpaqueColor : opaqueColor;
        QVector<int> &domainOpaqueObject = object.showInFront
            ? frontOpaqueObject : opaqueObject;
        std::vector<RasterWorkbenchLinearPixel> &domainOpaqueScenePixels =
            object.showInFront ? frontOpaqueScenePixels : opaqueScenePixels;
        QVector<float> &domainOpaqueSceneDepth = object.showInFront
            ? frontOpaqueSceneDepth : opaqueSceneDepth;
        RasterWorkbenchFragmentSink objectFragments;
        renderObject(object, useWeightedOit ? &objectFragments : nullptr,
                     [&](const QImage &layer) {
            for (int y = 0; y < height; ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                for (int x = 0; x < width; ++x) {
                    const int index = y * width + x;
                    const QRgb pixel = row[x];
                    if (qAlpha(pixel) < 254 || objectDepth[index] == noDepth)
                        continue;
                    if (objectDepth[index] > domainOpaqueDepth[index]) {
                        domainOpaqueDepth[index] = objectDepth[index];
                        domainOpaqueColor[index] = pixel;
                        domainOpaqueObject[index] = objectIndex;
                    }
                }
            }
            if (useWeightedOit) {
                QVector<int> nearestOpaqueFragment(pixelCount, -1);
                for (int fragmentIndex = 0;
                     fragmentIndex < static_cast<int>(objectFragments.size());
                     ++fragmentIndex) {
                    const RasterWorkbenchLinearFragment &fragment =
                        objectFragments[static_cast<std::size_t>(fragmentIndex)];
                    if (fragment.alpha < 0.999f || fragment.x < 0 ||
                        fragment.x >= width || fragment.y < 0 ||
                        fragment.y >= height) continue;
                    const int index = fragment.y * width + fragment.x;
                    const int previous = nearestOpaqueFragment[index];
                    if (previous < 0 || fragment.cameraDepth >
                            objectFragments[static_cast<std::size_t>(previous)]
                                .cameraDepth) {
                        nearestOpaqueFragment[index] = fragmentIndex;
                    }
                }
                for (int index = 0; index < pixelCount; ++index) {
                    const int fragmentIndex = nearestOpaqueFragment[index];
                    if (fragmentIndex < 0 || objectDepth[index] == noDepth ||
                        domainOpaqueObject[index] != objectIndex) continue;
                    const RasterWorkbenchLinearFragment &fragment =
                        objectFragments[static_cast<std::size_t>(fragmentIndex)];
                    const int x = index % width;
                    const int y = index / width;
                    const QRgb coveragePixel = reinterpret_cast<const QRgb *>(
                        layer.constScanLine(y))[x];
                    const float coverage = qAlpha(coveragePixel) / 255.0f;
                    if (coverage <= 0.0f) continue;
                    domainOpaqueSceneDepth[index] = fragment.cameraDepth;
                    domainOpaqueScenePixels[static_cast<std::size_t>(index)] =
                        RasterWorkbenchLinearPixel::fromStraight(
                            fragment.sceneLinearRgb[0],
                            fragment.sceneLinearRgb[1],
                            fragment.sceneLinearRgb[2], coverage);
                }
            }
        });
        if (useWeightedOit && objectFragments.size() >
                static_cast<std::size_t>(kMaximumTransparentFragments)) {
            return false;
        }
    }

    if (useWeightedOit) {
        std::vector<RasterWorkbenchLinearFragment> fragments;
        std::vector<RasterWorkbenchLinearFragment> frontFragments;
        bool fragmentBudgetExceeded = false;
        for (const ViewportRenderObject &object : objects) {
            RasterWorkbenchFragmentSink objectFragments;
            renderObject(object, &objectFragments, [&](const QImage &) {
            });
            if (objectFragments.size() >
                    static_cast<std::size_t>(kMaximumTransparentFragments)) {
                return false;
            }
            // A shared triangle edge can be rasterized by both adjacent
            // triangles. Sort the object's samples so those duplicate
            // coplanar edge samples can be collapsed while retaining every
            // depth-separated surface belonging to the same object.
            std::sort(objectFragments.begin(), objectFragments.end(),
                      [width](const auto &left, const auto &right) {
                          const int leftPixel = left.y * width + left.x;
                          const int rightPixel = right.y * width + right.x;
                          if (leftPixel != rightPixel) return leftPixel < rightPixel;
                          return left.cameraDepth < right.cameraDepth;
                      });
            int previousPixel = -1;
            float previousDepth = 0.0f;
            std::array<float, 3> previousColor{};
            float previousAlpha = 0.0f;
            bool havePrevious = false;
            for (const RasterWorkbenchLinearFragment &fragment : objectFragments) {
                if (fragment.alpha <= 0.0f || fragment.alpha >= 0.999f ||
                    fragment.x < 0 || fragment.x >= width ||
                    fragment.y < 0 || fragment.y >= height) continue;
                const int index = fragment.y * width + fragment.x;
                if (havePrevious && index == previousPixel &&
                    std::abs(fragment.cameraDepth - previousDepth) <= 1.0e-5f &&
                    fragment.sceneLinearRgb == previousColor &&
                    std::abs(fragment.alpha - previousAlpha) <= 1.0e-6f) {
                    continue;
                }
                previousPixel = index;
                previousDepth = fragment.cameraDepth;
                previousColor = fragment.sceneLinearRgb;
                previousAlpha = fragment.alpha;
                havePrevious = true;
                // Test the visible fragment against the nearest opaque
                // fragment from every object before adding it to OIT.
                const bool inFront = object.showInFront;
                const QVector<float> &domainOpaqueDepth = inFront
                    ? frontOpaqueSceneDepth : opaqueSceneDepth;
                if (fragment.cameraDepth <= domainOpaqueDepth[index]) continue;
                std::vector<RasterWorkbenchLinearFragment> &domainFragments =
                    inFront ? frontFragments : fragments;
                if (fragments.size() + frontFragments.size() >=
                    static_cast<std::size_t>(kMaximumTransparentFragments)) {
                    fragmentBudgetExceeded = true;
                    break;
                }
                domainFragments.push_back(fragment);
            }
            if (fragmentBudgetExceeded) return false;
        }

        const QVector3D background = sceneLinearBackground.value_or(
            QVector3D(0.0f, 0.0f, 0.0f));
        const bool hasPixelBackground =
            sceneLinearBackgroundPixels.size() ==
            static_cast<std::size_t>(pixelCount);
        const bool haveRegularCavityFactors =
            regularCavityFactors.size() == static_cast<std::size_t>(pixelCount);
        const bool haveInFrontCavityFactors =
            inFrontCavityFactors.size() == static_cast<std::size_t>(pixelCount);
        if (haveRegularCavityFactors || haveInFrontCavityFactors) {
            for (int index = 0; index < pixelCount; ++index) {
                RasterWorkbenchLinearPixel &regular =
                    opaqueScenePixels[static_cast<std::size_t>(index)];
                RasterWorkbenchLinearPixel &front =
                    frontOpaqueScenePixels[static_cast<std::size_t>(index)];
                if (haveRegularCavityFactors) {
                    const float factor = regularCavityFactors[
                        static_cast<std::size_t>(index)];
                    regular.red *= factor;
                    regular.green *= factor;
                    regular.blue *= factor;
                }
                if (haveInFrontCavityFactors) {
                    const float factor = inFrontCavityFactors[
                        static_cast<std::size_t>(index)];
                    front.red *= factor;
                    front.green *= factor;
                    front.blue *= factor;
                }
            }
            if (surfaceLayers != nullptr) {
                surfaceLayers->cavityAppliedBeforeTransparency = true;
            }
        }
        std::vector<RasterWorkbenchLinearPixel> basePixels(
            static_cast<std::size_t>(pixelCount));
        for (int index = 0; index < pixelCount; ++index) {
            const QVector3D pixelBackground = hasPixelBackground
                ? sceneLinearBackgroundPixels[static_cast<std::size_t>(index)]
                : background;
            RasterWorkbenchLinearPixel base =
                RasterWorkbenchLinearPixel::fromStraight(
                    pixelBackground.x(), pixelBackground.y(),
                    pixelBackground.z(), hasPixelBackground ||
                        sceneLinearBackground.has_value() ? 1.0f : 0.0f);
            const RasterWorkbenchLinearPixel &opaque =
                opaqueScenePixels[static_cast<std::size_t>(index)];
            const float remain = 1.0f - opaque.alpha;
            base.red = opaque.red + base.red * remain;
            base.green = opaque.green + base.green * remain;
            base.blue = opaque.blue + base.blue * remain;
            base.alpha = opaque.alpha + base.alpha * remain;
            basePixels[static_cast<std::size_t>(index)] = base;
        }
        std::vector<RasterWorkbenchLinearPixel> resolved;
        if (!resolveRasterWorkbenchWeightedOitFragments(
                width, height, basePixels, fragments, &resolved)) {
            return false;
        }
        std::vector<RasterWorkbenchLinearPixel> transparentResolved;
        if (!resolveRasterWorkbenchWeightedOitFragments(
                width, height, RasterWorkbenchLinearPixel{}, fragments,
                &transparentResolved)) {
            return false;
        }
        std::vector<RasterWorkbenchLinearPixel> frontBasePixels(
            static_cast<std::size_t>(pixelCount));
        for (int index = 0; index < pixelCount; ++index) {
            const RasterWorkbenchLinearPixel &regularResolved =
                resolved[static_cast<std::size_t>(index)];
            const RasterWorkbenchLinearPixel &frontOpaque =
                frontOpaqueScenePixels[static_cast<std::size_t>(index)];
            const float remain = 1.0f - frontOpaque.alpha;
            frontBasePixels[static_cast<std::size_t>(index)] = {
                frontOpaque.red + regularResolved.red * remain,
                frontOpaque.green + regularResolved.green * remain,
                frontOpaque.blue + regularResolved.blue * remain,
                frontOpaque.alpha + regularResolved.alpha * remain};

            // Retain the opaque receiver and transparent overlay separately
            // for the later cavity/outline effect compositor.
            RasterWorkbenchLinearPixel &opaque =
                opaqueScenePixels[static_cast<std::size_t>(index)];
            opaque = {
                frontOpaque.red + opaque.red * remain,
                frontOpaque.green + opaque.green * remain,
                frontOpaque.blue + opaque.blue * remain,
                frontOpaque.alpha + opaque.alpha * remain};
        }
        std::vector<RasterWorkbenchLinearPixel> frontResolved;
        if (!resolveRasterWorkbenchWeightedOitFragments(
                width, height, frontBasePixels, frontFragments,
                &frontResolved)) {
            return false;
        }
        std::vector<RasterWorkbenchLinearPixel> frontTransparentResolved;
        if (!resolveRasterWorkbenchWeightedOitFragments(
                width, height, RasterWorkbenchLinearPixel{}, frontFragments,
                &frontTransparentResolved)) {
            return false;
        }
        resolved = std::move(frontResolved);
        for (int index = 0; index < pixelCount; ++index) {
            RasterWorkbenchLinearPixel &transparent =
                transparentResolved[static_cast<std::size_t>(index)];
            const RasterWorkbenchLinearPixel &frontOpaque =
                frontOpaqueScenePixels[static_cast<std::size_t>(index)];
            const RasterWorkbenchLinearPixel &frontTransparent =
                frontTransparentResolved[static_cast<std::size_t>(index)];
            const float remainOpaque = 1.0f - frontOpaque.alpha;
            const float remainFrontTransparent = 1.0f - frontTransparent.alpha;
            transparent = {
                frontTransparent.red + transparent.red * remainOpaque * remainFrontTransparent,
                frontTransparent.green + transparent.green * remainOpaque * remainFrontTransparent,
                frontTransparent.blue + transparent.blue * remainOpaque * remainFrontTransparent,
                frontTransparent.alpha + transparent.alpha * remainOpaque * remainFrontTransparent};
        }
        for (int y = 0; y < height; ++y) {
            auto *destination = reinterpret_cast<QRgb *>(colorImage->scanLine(y));
            auto *opaqueRow = surfaceLayers != nullptr
                ? reinterpret_cast<QRgb *>(surfaceLayers->opaqueColor.scanLine(y))
                : nullptr;
            auto *transparentRow = surfaceLayers != nullptr
                ? reinterpret_cast<QRgb *>(surfaceLayers->transparentColor.scanLine(y))
                : nullptr;
            for (int x = 0; x < width; ++x) {
                const int index = y * width + x;
                QRgb surface = opaqueColor[index];
                const QRgb frontSurface = frontOpaqueColor[index];
                if (qAlpha(frontSurface) > 0) {
                    const float frontAlpha = qAlpha(frontSurface) / 255.0f;
                    const float remain = 1.0f - frontAlpha;
                    surface = qRgba(
                        std::clamp(qRed(frontSurface) +
                                       qRound(qRed(surface) * remain), 0, 255),
                        std::clamp(qGreen(frontSurface) +
                                       qRound(qGreen(surface) * remain), 0, 255),
                        std::clamp(qBlue(frontSurface) +
                                       qRound(qBlue(surface) * remain), 0, 255),
                        std::clamp(qAlpha(frontSurface) +
                                       qRound(qAlpha(surface) * remain), 0, 255));
                }
                if (opaqueRow != nullptr) opaqueRow[x] = surface;
                const RasterWorkbenchLinearPixel &linear = resolved[index];
                QRgb transparent = qRgba(0, 0, 0, 0);
                const RasterWorkbenchLinearPixel &transparentLinear =
                    transparentResolved[static_cast<std::size_t>(index)];
                const RasterWorkbenchLinearPixel &opaquePixel =
                    opaqueScenePixels[static_cast<std::size_t>(index)];
                // The retained cavity layers remain individually transformed;
                // the final scene pixel below is transformed only once.
                if (opaquePixel.alpha > 0.0f) {
                    const QVector3D opaqueStraight(
                        opaquePixel.red / opaquePixel.alpha,
                        opaquePixel.green / opaquePixel.alpha,
                        opaquePixel.blue / opaquePixel.alpha);
                    const QVector3D opaqueDisplay =
                        workbenchSceneLinearToDisplaySrgb(
                            opaqueStraight, shadingSettings.colorTransform,
                            shadingSettings.colorExposure,
                            shadingSettings.colorGamma,
                            shadingSettings.colorLutData,
                            shadingSettings.colorLutMetadata);
                    const int opaqueAlpha = std::clamp(
                        qRound(opaquePixel.alpha * 255.0f), 0, 255);
                    surface = qPremultiply(qRgba(
                        std::clamp(qRound(opaqueDisplay.x() * 255.0f), 0, 255),
                        std::clamp(qRound(opaqueDisplay.y() * 255.0f), 0, 255),
                        std::clamp(qRound(opaqueDisplay.z() * 255.0f), 0, 255),
                        opaqueAlpha));
                    if (opaqueRow != nullptr) opaqueRow[x] = surface;
                }
                if (transparentLinear.alpha > 0.0f) {
                    const float inverseAlpha = 1.0f / transparentLinear.alpha;
                    const QVector3D straightLinear(
                        transparentLinear.red * inverseAlpha,
                        transparentLinear.green * inverseAlpha,
                        transparentLinear.blue * inverseAlpha);
                    const QVector3D display = workbenchSceneLinearToDisplaySrgb(
                        straightLinear, shadingSettings.colorTransform,
                        shadingSettings.colorExposure,
                        shadingSettings.colorGamma,
                        shadingSettings.colorLutData,
                        shadingSettings.colorLutMetadata);
                    const int alpha = std::clamp(
                        qRound(transparentLinear.alpha * 255.0f), 0, 255);
                    transparent = qPremultiply(qRgba(
                        std::clamp(qRound(display.x() * 255.0f), 0, 255),
                        std::clamp(qRound(display.y() * 255.0f), 0, 255),
                        std::clamp(qRound(display.z() * 255.0f), 0, 255),
                        alpha));
                }
                if (transparentRow != nullptr) transparentRow[x] = transparent;
                QRgb combined = qRgba(0, 0, 0, 0);
                if (linear.alpha > 0.0f) {
                    const QVector3D straightLinear(
                        linear.red / linear.alpha,
                        linear.green / linear.alpha,
                        linear.blue / linear.alpha);
                    const QVector3D display = workbenchSceneLinearToDisplaySrgb(
                        straightLinear, shadingSettings.colorTransform,
                        shadingSettings.colorExposure,
                        shadingSettings.colorGamma,
                        shadingSettings.colorLutData,
                        shadingSettings.colorLutMetadata);
                    combined = qPremultiply(qRgba(
                        std::clamp(qRound(display.x() * 255.0f), 0, 255),
                        std::clamp(qRound(display.y() * 255.0f), 0, 255),
                        std::clamp(qRound(display.z() * 255.0f), 0, 255),
                        std::clamp(qRound(linear.alpha * 255.0f), 0, 255)));
                }
                const QRgb old = destination[x];
                const float combinedAlpha = qAlpha(combined) / 255.0f;
                const float oldRemain = 1.0f - combinedAlpha;
                destination[x] = qRgba(
                    std::clamp(qRed(combined) + qRound(qRed(old) * oldRemain),
                               0, 255),
                    std::clamp(qGreen(combined) + qRound(qGreen(old) * oldRemain),
                               0, 255),
                    std::clamp(qBlue(combined) + qRound(qBlue(old) * oldRemain),
                               0, 255),
                    std::clamp(qAlpha(combined) +
                                   qRound(qAlpha(old) * oldRemain),
                               0, 255));
            }
        }
        return true;
    }

    // The sorted fallback stores its opaque surface as a premultiplied
    // QPainter layer. Fold cavity into that layer before transparent surfaces
    // are alpha-composited so edge coverage and the transparent contribution
    // remain independent. Unlike weighted OIT, this layer is display RGB; the
    // scene-linear weighted path above remains the higher-fidelity resolve.
    if (surfaceLayers != nullptr && shadingSettings.cavity) {
        const bool haveRegularFactors =
            regularCavityFactors.size() == static_cast<std::size_t>(pixelCount);
        const bool haveFrontFactors =
            inFrontCavityFactors.size() == static_cast<std::size_t>(pixelCount);
        if (haveRegularFactors || haveFrontFactors) {
            for (int index = 0; index < pixelCount; ++index) {
                const auto applyFactor = [](QRgb *pixel, float factor) {
                    if (pixel == nullptr || qAlpha(*pixel) == 0 || factor == 1.0f)
                        return;
                    *pixel = qRgba(
                        std::clamp(qRound(qRed(*pixel) * factor), 0, 255),
                        std::clamp(qRound(qGreen(*pixel) * factor), 0, 255),
                        std::clamp(qRound(qBlue(*pixel) * factor), 0, 255),
                        qAlpha(*pixel));
                };
                if (haveRegularFactors) {
                    applyFactor(&opaqueColor[index], regularCavityFactors[
                        static_cast<std::size_t>(index)]);
                }
                if (haveFrontFactors) {
                    applyFactor(&frontOpaqueColor[index], inFrontCavityFactors[
                        static_cast<std::size_t>(index)]);
                }
            }
            surfaceLayers->cavityAppliedBeforeTransparency = true;
        }
    }

    // Keep QPainter's per-object layer shading, then retain that layer's
    // nearest surface depth at each covered pixel. This lets intersecting
    // transparent objects composite in a local far-to-near order rather than
    // inheriting one global average-depth order.
    bool transparentFragmentBudgetExceeded = false;
    for (int objectOrder = 0; objectOrder < objects.size(); ++objectOrder) {
        const ViewportRenderObject &object = objects[objectOrder];
        renderObject(object, nullptr, [&](const QImage &layer) {
            for (int y = 0; y < height; ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                for (int x = 0; x < width; ++x) {
                    const int index = y * width + x;
                    const QRgb pixel = row[x];
                    const int alphaByte = qAlpha(pixel);
                    if (alphaByte == 0 || alphaByte >= 254 ||
                        objectDepth[index] == noDepth ||
                        objectDepth[index] <= (object.showInFront
                            ? frontOpaqueDepth[index] : opaqueDepth[index])) {
                        continue;
                    }
                    if (regularTransparentFragments.size() +
                            frontTransparentFragments.size() >=
                        kMaximumTransparentFragments) {
                        transparentFragmentBudgetExceeded = true;
                        return;
                    }
                    (object.showInFront ? frontTransparentFragments
                                        : regularTransparentFragments).append(
                        {index, objectDepth[index], objectOrder,
                         object.showInFront, pixel});
                }
                if (transparentFragmentBudgetExceeded) return;
            }
        });
        if (transparentFragmentBudgetExceeded) return false;
    }

    const auto sortTransparentFragments =
        [](QVector<TransparentFragment> *fragments) {
        std::sort(fragments->begin(), fragments->end(),
              [](const TransparentFragment &left,
                 const TransparentFragment &right) {
                  if (left.pixelIndex != right.pixelIndex) {
                      return left.pixelIndex < right.pixelIndex;
                  }
                  if (left.depth != right.depth) {
                      return left.depth < right.depth;
                  }
                  return left.objectOrder < right.objectOrder;
              });
    };
    sortTransparentFragments(&regularTransparentFragments);
    sortTransparentFragments(&frontTransparentFragments);
    QVector<float> regularTransparentRed(pixelCount, 0.0f);
    QVector<float> regularTransparentGreen(pixelCount, 0.0f);
    QVector<float> regularTransparentBlue(pixelCount, 0.0f);
    QVector<float> regularTransparentAlpha(pixelCount, 0.0f);
    QVector<float> frontTransparentRed(pixelCount, 0.0f);
    QVector<float> frontTransparentGreen(pixelCount, 0.0f);
    QVector<float> frontTransparentBlue(pixelCount, 0.0f);
    QVector<float> frontTransparentAlpha(pixelCount, 0.0f);
    const auto accumulateTransparentFragments =
        [&](const QVector<TransparentFragment> &domainFragments,
            QVector<float> *red, QVector<float> *green,
            QVector<float> *blue, QVector<float> *alphaBuffer) {
    for (const TransparentFragment &fragment : domainFragments) {
        const int index = fragment.pixelIndex;
        const QRgb pixel = fragment.color;
        const int alphaByte = qAlpha(pixel);
        const float alpha = alphaByte / 255.0f;
        const float remain = 1.0f - alpha;
        const float sourceRed = static_cast<float>(qRed(pixel)) / alphaByte;
        const float sourceGreen = static_cast<float>(qGreen(pixel)) / alphaByte;
        const float sourceBlue = static_cast<float>(qBlue(pixel)) / alphaByte;
        (*red)[index] = sourceRed * alpha + (*red)[index] * remain;
        (*green)[index] = sourceGreen * alpha + (*green)[index] * remain;
        (*blue)[index] = sourceBlue * alpha + (*blue)[index] * remain;
        (*alphaBuffer)[index] = alpha + (*alphaBuffer)[index] * remain;
    }
    };
    accumulateTransparentFragments(regularTransparentFragments,
        &regularTransparentRed, &regularTransparentGreen,
        &regularTransparentBlue, &regularTransparentAlpha);
    accumulateTransparentFragments(frontTransparentFragments,
        &frontTransparentRed, &frontTransparentGreen,
        &frontTransparentBlue, &frontTransparentAlpha);
    const auto makeTransparentPixel = [](float red, float green, float blue,
                                         float alpha) {
        return qRgba(std::clamp(qRound(red * 255.0f), 0, 255),
                     std::clamp(qRound(green * 255.0f), 0, 255),
                     std::clamp(qRound(blue * 255.0f), 0, 255),
                     std::clamp(qRound(alpha * 255.0f), 0, 255));
    };
    const auto over = [](QRgb foreground, QRgb background) {
        const float remain = 1.0f - qAlpha(foreground) / 255.0f;
        return qRgba(
            std::clamp(qRed(foreground) + qRound(qRed(background) * remain),
                       0, 255),
            std::clamp(qGreen(foreground) + qRound(qGreen(background) * remain),
                       0, 255),
            std::clamp(qBlue(foreground) + qRound(qBlue(background) * remain),
                       0, 255),
            std::clamp(qAlpha(foreground) +
                           qRound(qAlpha(background) * remain), 0, 255));
    };

    for (int y = 0; y < height; ++y) {
        auto *destination = reinterpret_cast<QRgb *>(colorImage->scanLine(y));
        auto *opaqueRow = surfaceLayers != nullptr
            ? reinterpret_cast<QRgb *>(surfaceLayers->opaqueColor.scanLine(y))
            : nullptr;
        auto *transparentRow = surfaceLayers != nullptr
            ? reinterpret_cast<QRgb *>(surfaceLayers->transparentColor.scanLine(y))
            : nullptr;
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            const QRgb regularTransparent = makeTransparentPixel(
                regularTransparentRed[index], regularTransparentGreen[index],
                regularTransparentBlue[index], regularTransparentAlpha[index]);
            const QRgb frontTransparent = makeTransparentPixel(
                frontTransparentRed[index], frontTransparentGreen[index],
                frontTransparentBlue[index], frontTransparentAlpha[index]);
            const QRgb regularSurface = over(regularTransparent,
                                             opaqueColor[index]);
            const QRgb opaqueSurface = over(frontOpaqueColor[index],
                                            opaqueColor[index]);
            const float transparentRemain =
                1.0f - qAlpha(frontOpaqueColor[index]) / 255.0f;
            const QRgb retainedRegularTransparent = qRgba(
                qRound(qRed(regularTransparent) * transparentRemain),
                qRound(qGreen(regularTransparent) * transparentRemain),
                qRound(qBlue(regularTransparent) * transparentRemain),
                qRound(qAlpha(regularTransparent) * transparentRemain));
            const QRgb retainedTransparent = over(
                frontTransparent, retainedRegularTransparent);
            QRgb surface = over(frontTransparent,
                                over(frontOpaqueColor[index], regularSurface));
            if (opaqueRow != nullptr) opaqueRow[x] = opaqueSurface;
            if (transparentRow != nullptr) {
                transparentRow[x] = retainedTransparent;
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

float rasterWorkbenchCurvatureResponse(
    quint64 objectUp, quint64 objectDown, quint64 objectRight,
    quint64 objectLeft, float normalUp, float normalDown,
    float normalRight, float normalLeft, float ridgeFactor,
    float valleyFactor) noexcept
{
    if (objectUp != objectDown || objectRight != objectLeft ||
        (objectUp == 0 && objectRight == 0)) {
        return 0.0f;
    }
    const float normalDifference = (normalUp - normalDown) +
                                   (normalRight - normalLeft);
    const bool valley = normalDifference < 0.0f;
    const float factor = std::max(valley ? valleyFactor : ridgeFactor, 0.0f);
    const float control = (valley ? 0.7f : 0.5f) /
        std::max(factor * factor, 1.0e-4f);
    const float value = std::abs(normalDifference);
    const float softClamp = value < 0.5f / control
        ? value * (1.0f - value * control)
        : 0.25f / control;
    return (valley ? -2.0f : 2.0f) * softClamp;
}

void applyRasterWorkbenchEffects(QImage *colorImage,
                                 const QVector<ViewportRenderObject> &objects,
                                 const ViewportTransform &transform,
                                 const QSize &viewportSize,
                                 qreal devicePixelRatio,
                                 const ViewportShadingSettings &settings,
                                 const RasterWorkbenchSurfaceLayers *surfaceLayers,
                                 std::vector<float> *cavityFactors)
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
    const bool cavityEnabled = settings.cavity &&
        !(surfaceLayers != nullptr &&
          surfaceLayers->cavityAppliedBeforeTransparency);
    const ViewportCameraPreferences cameraOptics = transform.cameraPreferences();
    const bool perspective = transform.isPerspectiveEnabled();
    const bool depthOfFieldEnabled = settings.depthOfField &&
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
    QVector<RasterSample> inFrontSamples(width * height);
    QVector<RasterSample> opaqueSamples;
    QVector<RasterSample> inFrontOpaqueSamples;
    if (cavityEnabled) {
        opaqueSamples.resize(width * height);
        inFrontOpaqueSamples.resize(width * height);
    }
    const Point3D towardCamera = transform.viewDirection();
    const Point3D viewUp = transform.viewUp();
    const Point3D viewRight{
        viewUp.y * towardCamera.z - viewUp.z * towardCamera.y,
        viewUp.z * towardCamera.x - viewUp.x * towardCamera.z,
        viewUp.x * towardCamera.y - viewUp.y * towardCamera.x};
    const QVector3D viewUpVector(float(viewUp.x), float(viewUp.y),
                                 float(viewUp.z));
    const QVector3D viewRightVector(float(viewRight.x), float(viewRight.y),
                                    float(viewRight.z));
    const QVector3D viewFacingVector(float(towardCamera.x),
                                     float(towardCamera.y),
                                     float(towardCamera.z));
    const Point3D camera = perspective
                               ? transform.cameraPosition(viewportSize)
                               : transform.viewTarget();

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
                    if (uvMap.name == workbenchUvMapName(mesh)) {
                        activeUvMap = &uvMap;
                        break;
                    }
                }
            }
            QHash<int, QImage> alphaImagesBySlot;
            QHash<int, QHash<int, QImage>> alphaUdimImagesBySlot;
            QHash<int, TextureAlphaTriangle::FloatImage> floatAlphaImagesBySlot;
            QHash<int, QHash<int, TextureAlphaTriangle::FloatImage>> floatAlphaUdimImagesBySlot;
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
                    if (!material.textureImageTiles.isEmpty()) {
                        if (!alphaUdimImagesBySlot.contains(faceSlot)) {
                            alphaUdimImagesBySlot.insert(
                                faceSlot, loadTextureAlphaTiles(material));
                        }
                        if (!floatAlphaUdimImagesBySlot.contains(faceSlot))
                            floatAlphaUdimImagesBySlot.insert(
                                faceSlot, loadFloatTextureAlphaTiles(material));
                        textureAlphaCutout =
                            (!alphaUdimImagesBySlot.value(faceSlot).isEmpty() ||
                             !floatAlphaUdimImagesBySlot.value(faceSlot).isEmpty()) &&
                            !ignoresTextureAlpha(
                                material.textureImageAlphaMode);
                    } else if (!material.textureImagePath.isEmpty() ||
                               !material.textureImagePackedDataBase64.isEmpty() ||
                               !material.textureImageSceneLinearFloatDataBase64.isEmpty()) {
                        if (!alphaImagesBySlot.contains(faceSlot)) {
                            alphaImagesBySlot.insert(
                                faceSlot, loadTextureAlphaImage(material));
                        }
                        alphaImage = alphaImagesBySlot.value(faceSlot);
                        if (!floatAlphaImagesBySlot.contains(faceSlot))
                            floatAlphaImagesBySlot.insert(faceSlot,
                                loadFloatAlphaPayload(
                                    material.textureImageSceneLinearFloatDataBase64,
                                    material.textureImageSceneLinearFloatWidth,
                                    material.textureImageSceneLinearFloatHeight));
                        textureAlphaCutout = (!alphaImage.isNull() ||
                            floatAlphaImagesBySlot.value(faceSlot).width > 0) &&
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
                        if (!alphaUdimImagesBySlot.value(faceSlot).isEmpty()) {
                            textureAlphaTriangle.udim = true;
                            textureAlphaTriangle.udimImages =
                                alphaUdimImagesBySlot.value(faceSlot);
                        }
                        if (!floatAlphaUdimImagesBySlot.value(faceSlot).isEmpty()) {
                            textureAlphaTriangle.udim = true;
                            textureAlphaTriangle.floatUdimImages =
                                floatAlphaUdimImagesBySlot.value(faceSlot);
                        }
                        if (!alphaImage.isNull()) {
                            textureAlphaTriangle.image = alphaImage;
                        }
                        textureAlphaTriangle.floatImage =
                            floatAlphaImagesBySlot.value(faceSlot);
                        textureAlphaTriangle.interpolation =
                            object.meshMaterials[faceSlot].textureImageInterpolation;
                        textureAlphaTriangle.extension =
                            object.meshMaterials[faceSlot].textureImageExtension;
                        textureAlphaTriangle.cutout = true;
                        for (int corner = 0; corner < 3; ++corner) {
                            const QPointF uv = activeUvMap->cornerValues[
                                faceFirstCorner + triangle[corner]];
                            textureAlphaTriangle.uvs[corner] =
                                textureAlphaTriangle.udim
                                    ? QPointF(uv.x(), uv.y())
                                    : QPointF(uv.x(), 1.0 - uv.y());
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
        // Surface tessellation carries normalized parameter UVs for trimmed
        // triangles and every derived NurbsSolid face, so alpha cutouts must
        // participate in CPU cavity/outline coverage for both geometry types.
        if (settings.colorMode == ViewportColorMode::Texture &&
            (object.shape.geometryType == GeometryType::NurbsSurface ||
             object.shape.geometryType == GeometryType::NurbsSolid) &&
            geometry->surfaceTextureCoordinates.size() == vertices.size() &&
            (!object.workbenchMaterial.textureImageTiles.isEmpty() ||
             !object.workbenchMaterial.textureImagePath.isEmpty() ||
             !object.workbenchMaterial.textureImagePackedDataBase64.isEmpty() ||
             !object.workbenchMaterial.textureImageSceneLinearFloatDataBase64.isEmpty()) &&
            !ignoresTextureAlpha(
                object.workbenchMaterial.textureImageAlphaMode)) {
            const bool hasUdim =
                !object.workbenchMaterial.textureImageTiles.isEmpty();
            const QHash<int, QImage> udimImages = hasUdim
                ? loadTextureAlphaTiles(object.workbenchMaterial)
                : QHash<int, QImage>{};
            const QImage alphaImage = hasUdim ? QImage{}
                : loadTextureAlphaImage(object.workbenchMaterial);
            const auto floatAlphaImage = hasUdim
                ? TextureAlphaTriangle::FloatImage{}
                : loadFloatAlphaPayload(
                    object.workbenchMaterial.textureImageSceneLinearFloatDataBase64,
                    object.workbenchMaterial.textureImageSceneLinearFloatWidth,
                    object.workbenchMaterial.textureImageSceneLinearFloatHeight);
            const auto floatAlphaUdimImages = hasUdim
                ? loadFloatTextureAlphaTiles(object.workbenchMaterial)
                : QHash<int, TextureAlphaTriangle::FloatImage>{};
            if ((hasUdim && (!udimImages.isEmpty() || !floatAlphaUdimImages.isEmpty())) ||
                !alphaImage.isNull() || floatAlphaImage.width > 0) {
                textureAlphaTriangles.resize(vertices.size() / 3);
                for (int triangle = 0; triangle + 2 < vertices.size();
                     triangle += 3) {
                    TextureAlphaTriangle &alphaTriangle =
                        textureAlphaTriangles[triangle / 3];
                    if (hasUdim) {
                        alphaTriangle.udim = true;
                        alphaTriangle.udimImages = udimImages;
                        alphaTriangle.floatUdimImages = floatAlphaUdimImages;
                    } else if (!alphaImage.isNull()) {
                        alphaTriangle.image = alphaImage;
                    }
                    alphaTriangle.floatImage = floatAlphaImage;
                    alphaTriangle.interpolation =
                        object.workbenchMaterial.textureImageInterpolation;
                    alphaTriangle.extension =
                        object.workbenchMaterial.textureImageExtension;
                    alphaTriangle.cutout = true;
                    for (int corner = 0; corner < 3; ++corner) {
                        const QPointF uv =
                            geometry->surfaceTextureCoordinates[triangle + corner];
                        alphaTriangle.uvs[corner] = hasUdim
                            ? QPointF(uv.x(), uv.y())
                            : QPointF(uv.x(), 1.0 - uv.y());
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
                if (perspective) {
                    const qreal cameraDepth = -(
                        (point.x - camera.x) * towardCamera.x +
                        (point.y - camera.y) * towardCamera.y +
                        (point.z - camera.z) * towardCamera.z);
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
                    QVector<RasterSample> &domainSamples = object.showInFront
                        ? inFrontSamples : samples;
                    RasterSample &sample = domainSamples[y * width + x];
                    qreal positionWeight0 = w0;
                    qreal positionWeight1 = w1;
                    qreal positionWeight2 = w2;
                    if (perspective) {
                        positionWeight0 *= reciprocalDepth[0];
                        positionWeight1 *= reciprocalDepth[1];
                        positionWeight2 *= reciprocalDepth[2];
                        const qreal positionWeightSum = positionWeight0 +
                                                        positionWeight1 +
                                                        positionWeight2;
                        if (std::abs(positionWeightSum) > 1.0e-12) {
                            positionWeight0 /= positionWeightSum;
                            positionWeight1 /= positionWeightSum;
                            positionWeight2 /= positionWeightSum;
                        }
                    }
                    const QVector3D interpolatedPosition =
                        world[0] * static_cast<float>(positionWeight0) +
                        world[1] * static_cast<float>(positionWeight1) +
                        world[2] * static_cast<float>(positionWeight2);
                    const float z = rasterWorkbenchCameraDepth(
                        interpolatedPosition, camera, towardCamera);
                    const QVector3D interpolatedNormal = rasterWorkbenchPrepassNormal(
                        normalized(
                        normal[0] * static_cast<float>(positionWeight0) +
                        normal[1] * static_cast<float>(positionWeight1) +
                        normal[2] * static_cast<float>(positionWeight2)),
                        area < 0.0, viewRightVector, viewUpVector,
                        viewFacingVector);
                    const auto writeNearest = [&](RasterSample &target) {
                        if (target.objectId != 0 && z <= target.depth) return;
                        target.depth = z;
                        target.position = interpolatedPosition;
                        target.normal = interpolatedNormal;
                        target.objectId = id;
                    };
                    writeNearest(sample);
                    const float sourceAlpha = triangleIndex < triangleAlphas.size()
                                                  ? triangleAlphas[triangleIndex]
                                                  : objectAlpha;
                    if (cavityEnabled && sourceAlpha >= 0.999f) {
                        QVector<RasterSample> &domainOpaqueSamples =
                            object.showInFront ? inFrontOpaqueSamples
                                               : opaqueSamples;
                        writeNearest(domainOpaqueSamples[y * width + x]);
                    }
                }
            }
        }
    }

    // Workbench keeps depth_in_front separate through its effects inputs.
    // Select the nearest sample inside each domain first, then let covered
    // in-front samples replace regular scene samples regardless of geometric
    // depth. Cavity remains opaque-only, so a transparent in-front sample
    // does not remove the regular opaque receiver underneath it.
    for (int index = 0; index < samples.size(); ++index) {
        if (inFrontSamples[index].objectId != 0) {
            samples[index] = inFrontSamples[index];
        }
        if (cavityEnabled && inFrontOpaqueSamples[index].objectId != 0) {
            opaqueSamples[index] = inFrontOpaqueSamples[index];
        }
    }

    const auto applyOutline = [&]() {
        if (!settings.outline) return;
        QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage result = source.copy();
        constexpr quint64 selectedIdMask = 0x80000000ull;
        const QColor outline = settings.outlineColor;
        const QColor selectionOutline = viewportSelectionColor();
        // Blender offsets its four object-ID taps by UI scale in normalized
        // viewport coordinates. The QPainter ID buffer is allocated at the
        // device-pixel resolution, so this is the equivalent texel distance.
        // Integer ID sampling resolves the fractional scale to the nearest
        // texel, as the GPU texture lookup does.
        const int outlineSampleOffset = workbenchEffectUiScale(dpr);
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
                const std::array<QPoint, 4> cardinalNeighbors = {
                    QPoint(outlineSampleOffset, 0),
                    QPoint(-outlineSampleOffset, 0),
                    QPoint(0, outlineSampleOffset),
                    QPoint(0, -outlineSampleOffset),
                };
                for (const QPoint &offset : cardinalNeighbors) {
                    const int sx = std::clamp(depthX + offset.x(), 0, width - 1);
                    const int sy = std::clamp(depthY + offset.y(), 0, height - 1);
                    const quint64 neighborId =
                        samples[sy * width + sx].objectId;
                    differentNeighbors += neighborId != objectId;
                    selectedEdge = selectedEdge ||
                                   (neighborId & selectedIdMask) != 0;
                }
                const float edgeOpacity = differentNeighbors / 4.0f;
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
    };

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
    const float step = std::max(1.0f, radius / 2.0f);
    QVector<float> factors(width * height, 1.0f);
    const float ridge = std::max(0.0f,
        static_cast<float>(settings.cavityRidgeFactor));
    const float valley = std::max(0.0f,
        static_cast<float>(settings.cavityValleyFactor));
    const float curvatureRidge = std::max(0.0f,
        static_cast<float>(settings.curvatureRidgeFactor));
    const float curvatureValley = std::max(0.0f,
        static_cast<float>(settings.curvatureValleyFactor));
    const float cavityDistance = std::max(
        0.0f, static_cast<float>(settings.cavityDistance));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const RasterSample &center = opaqueSamples[y * width + x];
            if (center.objectId == 0) continue;
            float cavityAmount = 0.0f;
            float edgeAmount = 0.0f;
            float ridgeResponse = 0.0f;
            float valleyResponse = 0.0f;
            int validNeighbors = 0;
            bool hadBackgroundCavitySample = false;
            if (curvature) {
                // Blender's curvature pass samples four cardinal neighbors
                // at ui_scale, rejects object-ID outline pixels, and takes
                // signed differences of view-space normal components.
                const int curvatureStep = workbenchEffectUiScale(dpr);
                const auto sampleAt = [&](int sx, int sy)
                    -> const RasterSample & {
                    sx = std::clamp(sx, 0, width - 1);
                    sy = std::clamp(sy, 0, height - 1);
                    return opaqueSamples[sy * width + sx];
                };
                const RasterSample &up = sampleAt(x, y - curvatureStep);
                const RasterSample &down = sampleAt(x, y + curvatureStep);
                const RasterSample &right = sampleAt(x + curvatureStep, y);
                const RasterSample &left = sampleAt(x - curvatureStep, y);
                const float response = rasterWorkbenchCurvatureResponse(
                    up.objectId, down.objectId, right.objectId, left.objectId,
                    QVector3D::dotProduct(up.normal, viewUpVector),
                    QVector3D::dotProduct(down.normal, viewUpVector),
                    QVector3D::dotProduct(right.normal, viewRightVector),
                    QVector3D::dotProduct(left.normal, viewRightVector),
                    curvatureRidge, curvatureValley);
                if (response < 0.0f) valleyResponse = -response;
                else ridgeResponse = response;
            }
            const float noiseAngle = rasterCavityNoise(x, y, 0) *
                                     6.28318530718f;
            const float rotationX = std::cos(noiseAngle);
            const float rotationY = std::sin(noiseAngle);
            const float noiseRadius = std::clamp(
                rasterCavityNoise(x, y, 1) - 0.5f, -0.499f, 0.499f) /
                static_cast<float>(requested);
            for (int i = 0; i < requested; ++i) {
                // Match Workbench's deterministic Hammersley-like disk sample
                // sequence and per-pixel blue-noise rotation/jitter.
                const float sampleRadius = std::fmod(
                    (static_cast<float>(i) + 0.5f) /
                        static_cast<float>(requested), 1.0f);
                const float angle = radicalInverseVdc(
                    static_cast<unsigned int>(i)) * 6.28318530718f;
                const float directionX = std::cos(angle);
                const float directionY = std::sin(angle);
                const float jitteredX = directionX * rotationX -
                                        directionY * rotationY;
                const float jitteredY = directionX * rotationY +
                                        directionY * rotationX;
                const float sampleDistance = std::max(
                    0.0f, sampleRadius + noiseRadius);
                const int sx = x + qRound(jitteredX * sampleDistance * step);
                const int sy = y + qRound(jitteredY * sampleDistance * step);
                if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                const RasterSample &neighbor = opaqueSamples[sy * width + sx];
                if (neighbor.objectId == 0) {
                    if (ssao) {
                        // Blender treats a background depth read as a point
                        // at the receiver's depth, then moves it farther from
                        // the camera by cavity_distance. Reconstruct that
                        // point on the camera-facing plane through this pixel.
                        QPointF planeOffset;
                        const WorkPlaneFrame viewPlane =
                            makeWorkPlaneFrameFromNormal(
                                {center.position.x(), center.position.y(),
                                 center.position.z()},
                                towardCamera, viewRight);
                        const QPointF sampleScreen(
                            (sx + 0.5) / dpr, (sy + 0.5) / dpr);
                        if (isValidWorkPlaneFrame(viewPlane) &&
                            transform.screenToWorkPlane(
                                sampleScreen, viewportSize, viewPlane,
                                &planeOffset)) {
                            const Point3D sampleOnPlane =
                                workPlaneFramePointToWorld(planeOffset,
                                                           viewPlane);
                            const QVector3D backgroundPosition(
                                static_cast<float>(sampleOnPlane.x -
                                    towardCamera.x * cavityDistance),
                                static_cast<float>(sampleOnPlane.y -
                                    towardCamera.y * cavityDistance),
                                static_cast<float>(sampleOnPlane.z -
                                    towardCamera.z * cavityDistance));
                            const QVector3D direction = backgroundPosition -
                                                       center.position;
                            const float distance = direction.length();
                            const float cavityDot = QVector3D::dotProduct(
                                direction, center.normal);
                            const float edgeDot = -cavityDot;
                            const float bias = 0.05f * distance + 0.0001f;
                            const float distanceAttenuation = 1.0f /
                                std::max(distance * (1.0f + distance *
                                    distance * attenuation), 1.0e-8f);
                            if (cavityDot > -bias)
                                cavityAmount += cavityDot *
                                                distanceAttenuation;
                            if (edgeDot > bias)
                                edgeAmount += edgeDot *
                                              distanceAttenuation;
                            hadBackgroundCavitySample = true;
                        }
                    }
                    continue;
                }
                ++validNeighbors;
                if (ssao) {
                    // The shader evaluates these values in view space. A
                    // rigid view rotation preserves vector length and dot
                    // products, so the retained world positions/normals give
                    // the same geometric response without inverse projection.
                    const QVector3D direction = neighbor.position -
                                                center.position;
                    const float distance = direction.length();
                    const float cavityDot = QVector3D::dotProduct(
                        direction, center.normal);
                    const float edgeDot = -cavityDot;
                    const float bias = 0.05f * distance + 0.0001f;
                    const float distanceAttenuation = 1.0f /
                        std::max(distance * (1.0f + distance * distance *
                                             attenuation), 1.0e-8f);
                    if (cavityDot > -bias)
                        cavityAmount += cavityDot * distanceAttenuation;
                    if (edgeDot > bias)
                        edgeAmount += edgeDot * distanceAttenuation;
                }
            }
            if (validNeighbors == 0 && !hadBackgroundCavitySample) continue;
            cavityAmount = ssao
                ? saturate(cavityAmount / static_cast<float>(requested) * valley)
                : 0.0f;
            edgeAmount = ssao
                ? edgeAmount / static_cast<float>(requested) * ridge
                : 0.0f;
            factors[y * width + x] = std::clamp(
                (1.0f - cavityAmount) * (1.0f + edgeAmount) *
                    (1.0f + ridgeResponse - valleyResponse),
                0.0f, 4.0f);
        }
    }

    if (cavityFactors != nullptr) {
        *cavityFactors = cavityEnabled
            ? std::vector<float>(factors.begin(), factors.end())
            : std::vector<float>{};
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
            // Blender shades its opaque pass before compositing transparent
            // surfaces. Retain that pass's color so cavity changes only the
            // opaque contribution when a translucent surface is in front.
            const bool hasTransparentTop =
                std::abs(visibleTop.depth - opaqueReceiver.depth) >
                depthTolerance;
            const bool hasRetainedLayers = surfaceLayers != nullptr &&
                !surfaceLayers->opaqueColor.isNull() &&
                !surfaceLayers->transparentColor.isNull() &&
                surfaceLayers->opaqueColor.size() == colorImage->size() &&
                surfaceLayers->transparentColor.size() == colorImage->size();
            if (hasTransparentTop && !hasRetainedLayers) {
                continue;
            }
            const float factor = factors[sampleIndex];
            if (factor == 1.0f) continue;
            const QRgb pixel = source.pixel(x, y);
            if (hasTransparentTop && hasRetainedLayers) {
                const int layerX = std::clamp(
                    x, 0, surfaceLayers->opaqueColor.width() - 1);
                const int layerY = std::clamp(
                    y, 0, surfaceLayers->opaqueColor.height() - 1);
                const QRgb opaque = surfaceLayers->opaqueColor.pixel(layerX, layerY);
                const QRgb transparent = surfaceLayers->transparentColor.pixel(layerX, layerY);
                const float remain = 1.0f - qAlpha(transparent) / 255.0f;
                const auto applyUnderlyingCavity = [&](int channel, int opaqueChannel) {
                    const int delta = qRound(opaqueChannel * (factor - 1.0f) * remain);
                    return std::clamp(channel + delta, 0, 255);
                };
                out[x] = qRgba(
                    applyUnderlyingCavity(qRed(pixel), qRed(opaque)),
                    applyUnderlyingCavity(qGreen(pixel), qGreen(opaque)),
                    applyUnderlyingCavity(qBlue(pixel), qBlue(opaque)),
                    qAlpha(pixel));
                continue;
            }
            out[x] = qRgba(
                std::clamp(qRound(qRed(pixel) * factor), 0, 255),
                std::clamp(qRound(qGreen(pixel) * factor), 0, 255),
                std::clamp(qRound(qBlue(pixel) * factor), 0, 255),
                qAlpha(pixel));
        }
    }
    *colorImage = result.convertToFormat(colorImage->format());
    }

    applyOutline();

    // QPainter fallback approximation: use the software-resolved visible
    // surface depth to estimate a physical circle of confusion, then gather
    // a small disk of same-or-behind-depth colors. This is intentionally
    // camera-gated and uses camera-relative depth for either projection.
    if (depthOfFieldEnabled) {
        const QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage dofResult = source.copy();
        constexpr std::array<QPointF, 12> disk{{
            {0.0, 0.0}, {0.527, 0.085}, {-0.040, 0.536},
            {-0.670, -0.179}, {0.244, -0.647}, {0.765, 0.429},
            {-0.315, 0.847}, {-0.897, 0.310}, {0.493, 0.749},
            {0.901, -0.312}, {-0.527, -0.757}, {0.089, -0.956}}};
        // Blender Workbench converts lens and sensor dimensions from mm to
        // the scene's meter-based coordinates; depth and focus stay in scene
        // coordinates (also the convention of the imported Blender geometry).
        const qreal focal = cameraOptics.focalLengthMillimeters * 0.001;
        const qreal focus = cameraOptics.focusDistance;
        const qreal fStop = cameraOptics.fStop;
        const qreal sensorWidth =
            cameraOptics.opticalSensorSizeMillimeters * 0.001;
        if (std::isfinite(focal) && focal > 0.0 && std::isfinite(focus) &&
            focus > 0.0 && std::isfinite(fStop) && fStop > 0.0 &&
            std::isfinite(sensorWidth) && sensorWidth > 0.0 &&
            std::abs(focus - focal) > 1.0e-6) {
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
                    // Blender's aperture radius is 0.5 * focal / f-stop;
                    // multiplying by focal / focus-distance produces this
                    // factor for both perspective and orthographic view depth.
                    const qreal cocMm = 0.5 * std::abs(
                        (focal * focal /
                         (fStop * std::abs(focus - focal))) *
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
                        const QPointF tap = rasterWorkbenchApertureTap(
                            rotatedTap, cameraOptics.apertureBlades,
                            cameraOptics.apertureRotationRadians,
                            cameraOptics.apertureRatio);
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
