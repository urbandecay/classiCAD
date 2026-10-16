/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_effects.h"
#include "viewport_raster_workbench_volume.h"
#include "viewport_workbench_effect_scale.h"
#include "viewport_renderer.h"
#include "blender_grid_renderer.h"
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
    float sourceAlpha = 1.0f;
};

struct RasterOutlineSample {
    float depth = -std::numeric_limits<float>::infinity();
    quint64 objectId = 0;
};

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

struct ShadowTriangle {
    QVector3D vertices[3];
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

bool isOpaqueWorkbenchAlpha(float alpha, bool xrayEnabled = false)
{
    // Blender classifies material transparency after packing alpha to UNorm8.
    // X-Ray routes the source object through the transparent pass whenever
    // its setting is below one, even if the effective alpha rounds to 255.
    return !xrayEnabled && workbenchPackUnitFloatToUnorm8(alpha) >= 1.0f;
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

float rasterWorkbenchCavityRadius(float cavityDistance,
                                  float pixelsPerWorldUnit,
                                  float devicePixelRatio) noexcept
{
    return cavityDistance * pixelsPerWorldUnit * devicePixelRatio;
}

float rasterWorkbenchCavityStep(float baseRadiusPixels,
                                const QVector3D &receiverPosition,
                                const Point3D &cameraPosition,
                                const Point3D &viewTarget,
                                const Point3D &viewDirection,
                                bool perspective) noexcept
{
    float radius = baseRadiusPixels;
    if (perspective) {
        const QVector3D target(static_cast<float>(viewTarget.x),
                               static_cast<float>(viewTarget.y),
                               static_cast<float>(viewTarget.z));
        const float targetDepth = rasterWorkbenchCameraDepth(
            target, cameraPosition, viewDirection);
        const float receiverDepth = rasterWorkbenchCameraDepth(
            receiverPosition, cameraPosition, viewDirection);
        if (targetDepth > 1.0e-6f && receiverDepth > 1.0e-6f)
            radius *= targetDepth / receiverDepth;
    }
    // The caller supplies an already projected radius in pixels. Blender's
    // shader halves its UV-space offset before sampling; that conversion is
    // already accounted for when expressing this footprint in pixels.
    return radius;
}

int rasterWorkbenchCavitySampleCount(int requestedSamples,
                                     qsizetype targetPixels) noexcept
{
    constexpr quint64 maximumSampleWork = 32ull * 1024ull * 1024ull;
    const int requested = std::clamp(requestedSamples, 1, 500);
    if (targetPixels <= 0) return requested;
    const quint64 budgetedCount = maximumSampleWork /
        static_cast<quint64>(targetPixels);
    return std::min(requested, static_cast<int>(std::clamp<quint64>(
        budgetedCount, 1, static_cast<quint64>(requested))));
}

QPoint rasterWorkbenchNearestCavityPixel(QPointF samplePixel,
                                         QSize pixelSize) noexcept
{
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0 ||
        !std::isfinite(samplePixel.x()) || !std::isfinite(samplePixel.y()) ||
        samplePixel.x() < 0.0 || samplePixel.y() < 0.0 ||
        samplePixel.x() > pixelSize.width() ||
        samplePixel.y() > pixelSize.height()) {
        return QPoint(-1, -1);
    }

    // GL_NEAREST selects floor(uv * dimension). Convert bottom-up GL rows
    // into QPainter's top-down rows, retaining clamp-to-edge at UV 0 and 1.
    const int x = std::clamp(static_cast<int>(std::floor(samplePixel.x())),
                             0, pixelSize.width() - 1);
    const int y = std::clamp(static_cast<int>(std::ceil(samplePixel.y())) - 1,
                             0, pixelSize.height() - 1);
    return QPoint(x, y);
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

    // workbench_common.bsl.hh normal_encode()/normal_decode(), including
    // RG16F render-target precision.
    const QVector3D decodedView = workbenchNormalEncodeDecode(viewNormal);
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

std::array<RasterWorkbenchDofTap, 49> rasterWorkbenchDofKernel(
    int apertureBlades, qreal apertureRotationRadians,
    qreal apertureRatio) noexcept
{
    constexpr int kernelRadius = 3;
    std::array<RasterWorkbenchDofTap, 49> samples{};
    std::size_t sampleIndex = 0;
    for (int ring = 0; ring <= kernelRadius; ++ring) {
        for (int y = -kernelRadius; y <= kernelRadius; ++y) {
            for (int x = -kernelRadius; x <= kernelRadius; ++x) {
                if (std::abs(x) > ring || std::abs(y) > ring ||
                    (std::abs(x) < ring && std::abs(y) < ring)) {
                    continue;
                }

                const qreal squareX = qreal(x) / kernelRadius;
                const qreal squareY = qreal(y) / kernelRadius;
                qreal radius = 0.0;
                qreal angle = 0.0;
                if (squareX > -squareY) {
                    if (squareX > squareY) {
                        radius = squareX;
                        angle = 0.7853981633974483 * (squareY / squareX);
                    } else {
                        radius = squareY;
                        angle = 0.7853981633974483 *
                            (2.0 - squareX / squareY);
                    }
                } else if (squareX < squareY) {
                    radius = -squareX;
                    angle = 0.7853981633974483 *
                        (4.0 + squareY / squareX);
                } else {
                    radius = -squareY;
                    angle = squareY != 0.0
                        ? 0.7853981633974483 *
                            (6.0 - squareX / squareY)
                        : 0.0;
                }

                qreal shapedRadius = radius;
                if (apertureBlades > 1) {
                    const qreal bladeAngle =
                        6.283185307179586 / apertureBlades;
                    const qreal denominator = angle - bladeAngle * std::floor(
                        (apertureBlades * angle + 3.141592653589793) /
                        6.283185307179586);
                    shapedRadius *=
                        std::cos(3.141592653589793 / apertureBlades) /
                        std::cos(denominator);
                }
                angle += apertureRotationRadians;
                const qreal ratio = std::max<qreal>(apertureRatio, 0.01);
                samples[sampleIndex++] = {
                    QPointF(shapedRadius * std::cos(angle) / ratio,
                            shapedRadius * std::sin(angle)),
                    static_cast<float>(radius)};
            }
        }
    }
    return samples;
}

QPointF rasterWorkbenchDofGatherOriginOffset(
    int x, int y, int targetWidth, int targetHeight,
    float centerCocPixels) noexcept
{
    if (targetWidth <= 0 || targetHeight <= 0 ||
        !std::isfinite(centerCocPixels) ||
        centerCocPixels <= 0.0f) {
        return {};
    }

    constexpr float tau = 6.28318530718f;
    const int halfWidth = std::max(targetWidth / 2, 1);
    const int halfHeight = std::max(targetHeight / 2, 1);
    const int halfX = std::clamp(x / 2, 0, halfWidth - 1);
    const int halfYTopDown = std::clamp(y / 2, 0, halfHeight - 1);
    const int noiseY = halfHeight - 1 - halfYTopDown;
    const float blueNoise = rasterCavityNoise(halfX, noiseY, 1);
    const float pixelX = static_cast<float>(halfX) + 0.5f;
    const float pixelY = static_cast<float>(noiseY) + 0.5f;
    const float gradient = 0.06711056f * pixelX + 0.00583715f * pixelY;
    const float ign = std::fmod(
        52.9829189f * (gradient - std::floor(gradient)), 1.0f);
    const float anglePhase = blueNoise - std::floor(blueNoise);
    const float angle = tau * anglePhase;
    const float ramp = std::clamp(centerCocPixels * 0.2f - 4.0f,
                                  0.0f, 1.0f);
    const float jitterScale = centerCocPixels * 0.2f * ramp *
                              std::sqrt(std::clamp(ign, 0.0f, 1.0f));
    return QPointF(std::cos(angle) * jitterScale,
                   std::sin(angle) * jitterScale);
}

RasterWorkbenchDofPrefilter rasterWorkbenchDofPrefilter2x2(
    const std::array<float, 4> &signedCocs) noexcept
{
    RasterWorkbenchDofPrefilter result;
    std::array<float, 4> nearCocs{};
    std::array<float, 4> farCocs{};
    for (std::size_t i = 0; i < signedCocs.size(); ++i) {
        nearCocs[i] = std::max(signedCocs[i], 0.0f);
        farCocs[i] = std::max(-signedCocs[i], 0.0f);
        result.nearCoc = std::max(result.nearCoc, nearCocs[i]);
        result.farCoc = std::max(result.farCoc, farCocs[i]);
    }

    std::array<float, 4> nearWeights{};
    std::array<float, 4> farWeights{};
    bool hasFarWeight = false;
    for (std::size_t i = 0; i < signedCocs.size(); ++i) {
        nearWeights[i] = signedCocs[i] >= 0.0f
            ? std::clamp(1.0f - std::abs(result.nearCoc - nearCocs[i]),
                         0.0f, 1.0f)
            : 0.0f;
        farWeights[i] = signedCocs[i] <= 0.0f
            ? std::clamp(1.0f - std::abs(result.farCoc - farCocs[i]),
                         0.0f, 1.0f)
            : 0.0f;
        hasFarWeight = hasFarWeight || farWeights[i] != 0.0f;
    }
    result.colorWeights = hasFarWeight ? farWeights : nearWeights;
    return result;
}

RasterWorkbenchDofPrefilter rasterWorkbenchDofPrefilter2x2(
    const std::array<float, 4> &nearCocs,
    const std::array<float, 4> &farCocs) noexcept
{
    RasterWorkbenchDofPrefilter result;
    for (std::size_t i = 0; i < nearCocs.size(); ++i) {
        result.nearCoc = std::max(result.nearCoc, nearCocs[i]);
        result.farCoc = std::max(result.farCoc, farCocs[i]);
    }

    std::array<float, 4> nearWeights{};
    std::array<float, 4> farWeights{};
    bool hasFarWeight = false;
    for (std::size_t i = 0; i < nearCocs.size(); ++i) {
        nearWeights[i] = std::clamp(
            1.0f - std::abs(result.nearCoc - nearCocs[i]), 0.0f, 1.0f);
        farWeights[i] = std::clamp(
            1.0f - std::abs(result.farCoc - farCocs[i]), 0.0f, 1.0f);
        hasFarWeight = hasFarWeight || farWeights[i] != 0.0f;
    }
    result.colorWeights = hasFarWeight ? farWeights : nearWeights;
    return result;
}

float rasterWorkbenchDofGatherWeight(float sampleCoc, float maximumCoc,
                                     float sampleRadius) noexcept
{
    const float sampleMagnitude = std::abs(sampleCoc);
    const float radius = sampleRadius * std::abs(maximumCoc);
    const float t = std::clamp(sampleMagnitude - (radius - 0.5f),
                               0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return sampleMagnitude * smooth;
}

float rasterWorkbenchDofMipLod(float sampleCoc, float centerCoc) noexcept
{
    const float coc = std::abs(sampleCoc);
    const float maximumRadius = std::abs(centerCoc);
    const float mipMetric = std::clamp(
        (coc + std::min(coc, maximumRadius)) * 0.5f - 21.0f,
        0.0f, 16.0f) * 0.25f;
    if (mipMetric <= 0.0f) return 0.0f;
    // The source texture has levels 0, 1, and 2. OpenGL clamps the shader's
    // larger theoretical LODs to the available highest level.
    return std::clamp(std::log2(mipMetric), 0.0f, 2.0f);
}

int rasterWorkbenchDofTapCountForBudget(quint64 candidatePixels) noexcept
{
    constexpr quint64 maximumGatherTaps = 24'000'000;
    if (candidatePixels == 0) return 49;
    return static_cast<int>(std::clamp<quint64>(
        maximumGatherTaps / candidatePixels, 1, 49));
}

float rasterWorkbenchDofResolveBlend(float cocPixels) noexcept
{
    const float t = std::clamp((std::abs(cocPixels) - 1.0f) / 2.0f,
                               0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float rasterWorkbenchShadowFactor(const QVector3D &normal,
                                  const QVector3D &lightDirection,
                                  float intensity, float shift, float focus,
                                  bool shadowed) noexcept
{
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    shift = std::clamp(shift, 0.0f, 1.0f);
    focus = std::clamp(focus, 0.0001f, 0.99999f);
    const float focusEdge = 1.0f - focus * (1.0f - shift);
    const float lightFactor = -QVector3D::dotProduct(normal, lightDirection);
    const float t = std::clamp((lightFactor - shift) /
                                   std::max(focusEdge - shift, 1.0e-6f),
                               0.0f, 1.0f);
    const float shadowMix = t * t * (3.0f - 2.0f * t);
    return shadowMix * (shadowed ? 0.0f : intensity) + (1.0f - intensity);
}

float rasterWorkbenchShadowDepthBias(float minimumLightDepth,
                                     float maximumLightDepth) noexcept
{
    if (!std::isfinite(minimumLightDepth) ||
        !std::isfinite(maximumLightDepth)) {
        return 0.0f;
    }
    const float depthRange = std::abs(maximumLightDepth - minimumLightDepth);
    const float maximumMagnitude = std::max(
        std::abs(minimumLightDepth), std::abs(maximumLightDepth));
    const float nextDepth = std::nextafter(
        maximumMagnitude, std::numeric_limits<float>::infinity());
    const float floatStep = nextDepth - maximumMagnitude;
    return std::max(depthRange * 1.0e-6f, floatStep * 2.0f);
}

QSize rasterWorkbenchShadowMapSize(const QSize &viewportSize) noexcept
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0)
        return {};

    constexpr qint64 maximumShadowMapPixels = 16 * 1024 * 1024;
    const auto scaledSizeWithinBudget = [&](qint64 scale) {
        const qint64 width = viewportSize.width();
        const qint64 height = viewportSize.height();
        if (width <= 0 || height <= 0 ||
            width > std::numeric_limits<int>::max() / scale ||
            height > std::numeric_limits<int>::max() / scale) {
            return QSize{};
        }
        const qint64 scaledWidth = width * scale;
        const qint64 scaledHeight = height * scale;
        if (scaledHeight > 0 &&
            scaledWidth <= maximumShadowMapPixels / scaledHeight) {
            return QSize(static_cast<int>(scaledWidth),
                         static_cast<int>(scaledHeight));
        }
        return QSize{};
    };
    const QSize fourTimesSize = scaledSizeWithinBudget(4);
    if (!fourTimesSize.isEmpty()) return fourTimesSize;
    const QSize twiceSize = scaledSizeWithinBudget(2);
    if (!twiceSize.isEmpty()) return twiceSize;
    return viewportSize;
}

QRgb rasterWorkbenchDofMedian3x3(
    const std::array<QRgb, 9> &samples) noexcept
{
    std::array<int, 9> channels{};
    const auto median = [&](auto channelValue) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            channels[i] = channelValue(samples[i]);
        }
        std::nth_element(channels.begin(), channels.begin() + 4,
                         channels.end());
        return channels[4];
    };
    return qRgba(median([](QRgb pixel) { return qRed(pixel); }),
                 median([](QRgb pixel) { return qGreen(pixel); }),
                 median([](QRgb pixel) { return qBlue(pixel); }),
                 median([](QRgb pixel) { return qAlpha(pixel); }));
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
    const std::vector<QVector3D> &sceneLinearBackgroundPixels,
    bool allowFragmentGeneratedDepth,
    const QVector<ViewportRenderObject> &scalarVolumeObjects,
    std::vector<RasterWorkbenchLinearPixel> *sceneLinearCapture,
    std::vector<float> *cavityFactorCapture,
    RasterWorkbenchCavityTapCapture *cavityTapCapture)
{
    if (colorImage == nullptr || colorImage->isNull() ||
        (objects.isEmpty() && scalarVolumeObjects.isEmpty()) ||
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
        const ViewportDepthGeometry *preparedGeometry =
            object.preparedDepthGeometry.data();
        if (preparedGeometry == nullptr) {
            if (!allowFragmentGeneratedDepth || shadingSettings.cavity) {
                return false;
            }
            continue;
        }
        if (preparedGeometry->surfaceVertices.size() < 3 ||
            preparedGeometry->surfaceVertices.size() % 3 != 0) {
            return false;
        }
    }
    const int pixelCount = static_cast<int>(pixelCount64);
    const bool xrayEnabled = shadingSettings.xrayEnabled();
    struct WeightedOitAccumulation {
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float shadowedRed = 0.0f;
        float shadowedGreen = 0.0f;
        float shadowedBlue = 0.0f;
        float weight = 0.0f;
        float revealage = 1.0f;
    };
    const auto makeOitBuffer = [pixelCount] {
        return std::vector<WeightedOitAccumulation>(
            static_cast<std::size_t>(pixelCount));
    };
    const auto addOitFragment = [width, height](
        std::vector<WeightedOitAccumulation> &buffer,
        const RasterWorkbenchLinearFragment &fragment,
        float shadowFactor = 1.0f) {
        if (fragment.x < 0 || fragment.x >= width || fragment.y < 0 ||
            fragment.y >= height || !std::isfinite(fragment.alpha) ||
            fragment.alpha <= 0.0f) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(fragment.y) *
                                      static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(fragment.x);
        const float weightedAlpha = fragment.alpha *
            rasterWorkbenchTransparentWeight(
                fragment.oitLinearDepth >= 0.0f
                    ? fragment.oitLinearDepth
                    : fragment.cameraDepth);
        WeightedOitAccumulation &accumulation = buffer[index];
        accumulation.red += fragment.sceneLinearRgb[0] * weightedAlpha;
        accumulation.green += fragment.sceneLinearRgb[1] * weightedAlpha;
        accumulation.blue += fragment.sceneLinearRgb[2] * weightedAlpha;
        accumulation.shadowedRed +=
            fragment.sceneLinearRgb[0] * weightedAlpha * shadowFactor;
        accumulation.shadowedGreen +=
            fragment.sceneLinearRgb[1] * weightedAlpha * shadowFactor;
        accumulation.shadowedBlue +=
            fragment.sceneLinearRgb[2] * weightedAlpha * shadowFactor;
        accumulation.weight += weightedAlpha;
        accumulation.revealage *= 1.0f - fragment.alpha;
    };
    const auto resolveOit = [pixelCount](
        const std::vector<WeightedOitAccumulation> &buffer,
        const std::vector<RasterWorkbenchLinearPixel> &base,
        bool useShadowedColor = false) {
        std::vector<RasterWorkbenchLinearPixel> resolved = base;
        for (int index = 0; index < pixelCount; ++index) {
            const WeightedOitAccumulation &accumulation =
                buffer[static_cast<std::size_t>(index)];
            if (accumulation.weight <= 0.0f) continue;
            const float alpha = 1.0f - accumulation.revealage;
            const float transmittance = 1.0f - alpha;
            const float weight = std::clamp(
                accumulation.weight, 1.0e-4f, 5.0e4f);
            const float red = useShadowedColor
                ? accumulation.shadowedRed : accumulation.red;
            const float green = useShadowedColor
                ? accumulation.shadowedGreen : accumulation.green;
            const float blue = useShadowedColor
                ? accumulation.shadowedBlue : accumulation.blue;
            RasterWorkbenchLinearPixel &pixel =
                resolved[static_cast<std::size_t>(index)];
            pixel.red = (red / weight) * alpha +
                        pixel.red * transmittance;
            pixel.green = (green / weight) * alpha +
                          pixel.green * transmittance;
            pixel.blue = (blue / weight) * alpha +
                         pixel.blue * transmittance;
            pixel.alpha = alpha + pixel.alpha * transmittance;
        }
        return resolved;
    };
    struct VolumeSlicePixel {
        float depth = 0.0f;
        int objectOrder = 0;
        int sampleIndex = -1;
        RasterWorkbenchLinearPixel color;
        bool depthPreClipped = false;
    };
    struct RayMarchVolume {
        ViewportRenderObject object;
        int objectOrder = 0;
    };
    std::vector<std::vector<VolumeSlicePixel>> volumeSlicePixels;
    if (!scalarVolumeObjects.isEmpty())
        volumeSlicePixels.resize(static_cast<std::size_t>(pixelCount));
    static const std::vector<VolumeSlicePixel> noVolumePixels;
    const auto volumePixelsAt = [&](int index)
        -> const std::vector<VolumeSlicePixel> & {
        return volumeSlicePixels.empty()
            ? noVolumePixels
            : volumeSlicePixels[static_cast<std::size_t>(index)];
    };
    QVector<RayMarchVolume> rayMarchVolumes;
    const auto volumeBaseColor = [&shadingSettings](
        const ViewportRenderObject &object) {
        QVector3D baseColor = workbenchDefaultSolidMaterialDiffuseColor();
        switch (shadingSettings.colorMode) {
        case ViewportColorMode::Material:
        case ViewportColorMode::Attribute:
        case ViewportColorMode::Texture:
            if (object.hasWorkbenchMaterial)
                baseColor = object.workbenchMaterial.baseColorSceneLinear;
            break;
        case ViewportColorMode::Object:
            baseColor = QVector3D(object.displayColorSceneLinear);
            break;
        case ViewportColorMode::Random: {
            const QColor randomColor = workbenchRandomColor(
                object.objectId, object.displayName,
                workbenchRandomIdentityLibraryPath(object));
            baseColor = workbenchRec709ToSceneLinear(QVector3D(
                randomColor.redF(), randomColor.greenF(), randomColor.blueF()));
            break;
        }
        case ViewportColorMode::Custom:
            baseColor = workbenchSrgbToSceneLinear(QVector3D(
                shadingSettings.customColor.redF(),
                shadingSettings.customColor.greenF(),
                shadingSettings.customColor.blueF()));
            break;
        }
        return baseColor;
    };
    constexpr std::size_t kMaximumVolumeSamples = 1'000'000;
    std::size_t retainedVolumeSamples = 0;
    for (int objectOrder = 0; objectOrder < scalarVolumeObjects.size();
         ++objectOrder) {
        const ViewportRenderObject &object = scalarVolumeObjects.at(objectOrder);
        if (!object.shape.scalarVolume.useSlice) {
            rayMarchVolumes.append({object, objectOrder});
            continue;
        }
        QVector<RasterWorkbenchVolumeSliceSample> samples;
        if (!rasterWorkbenchVolumeSliceSamples(
                object, transform, viewportSize, devicePixelRatio, &samples)) {
            continue;
        }
        const std::size_t sampleCount =
            static_cast<std::size_t>(samples.size());
        if (sampleCount > kMaximumVolumeSamples - retainedVolumeSamples)
            return false;
        retainedVolumeSamples += sampleCount;
        const QVector3D baseColor = volumeBaseColor(object);
        const float densityScale = std::max(
            0.0f, object.shape.scalarVolume.densityScale);
        for (const RasterWorkbenchVolumeSliceSample &sample : samples) {
            if (sample.x < 0 || sample.x >= width || sample.y < 0 ||
                sample.y >= height || !std::isfinite(sample.cameraDepth) ||
                !std::isfinite(sample.density) ||
                !std::isfinite(sample.sliceStepWorld) ||
                sample.sampleIndex < 0 || sample.sampleIndex >= 4) continue;
            const float density = sample.density * densityScale;
            const float extinction = std::max(density, 1.0e-4f);
            const float alpha = 1.0f - std::exp(
                -extinction * sample.sliceStepWorld);
            const float scatterScale = density / extinction;
            VolumeSlicePixel pixel;
            pixel.depth = sample.cameraDepth;
            pixel.objectOrder = objectOrder;
            pixel.sampleIndex = sample.sampleIndex;
            pixel.color = {baseColor.x() * alpha * scatterScale * 0.25f,
                           baseColor.y() * alpha * scatterScale * 0.25f,
                           baseColor.z() * alpha * scatterScale * 0.25f,
                           alpha};
            volumeSlicePixels[static_cast<std::size_t>(
                sample.y * width + sample.x)].push_back(pixel);
        }
    }
    for (auto &pixelSamples : volumeSlicePixels) {
        std::stable_sort(pixelSamples.begin(), pixelSamples.end(),
                         [](const VolumeSlicePixel &left,
                            const VolumeSlicePixel &right) {
                             return left.objectOrder < right.objectOrder;
                         });
    }
    const auto compositeVolumeSlices = [](
        RasterWorkbenchLinearPixel base,
        const std::vector<VolumeSlicePixel> &slices, float opaqueDepth,
        float transparentDepth, bool inFrontCovered) {
        if (inFrontCovered) return base;
        float occlusionDepth = std::numeric_limits<float>::infinity();
        if (std::isfinite(opaqueDepth))
            occlusionDepth = std::min(occlusionDepth, opaqueDepth);
        if (std::isfinite(transparentDepth))
            occlusionDepth = std::min(occlusionDepth, transparentDepth);
        RasterWorkbenchLinearPixel average{};
        for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
            RasterWorkbenchLinearPixel subsample = base;
            for (const VolumeSlicePixel &slice : slices) {
                if (slice.sampleIndex >= 0 &&
                    slice.sampleIndex != sampleIndex) continue;
                if (!slice.depthPreClipped && slice.depth >= occlusionDepth)
                    continue;
                const float remain = 1.0f - slice.color.alpha;
                subsample = {slice.color.red + subsample.red * remain,
                             slice.color.green + subsample.green * remain,
                             slice.color.blue + subsample.blue * remain,
                             slice.color.alpha + subsample.alpha * remain};
            }
            average.red += subsample.red * 0.25f;
            average.green += subsample.green * 0.25f;
            average.blue += subsample.blue * 0.25f;
            average.alpha += subsample.alpha * 0.25f;
        }
        return average;
    };
    constexpr std::size_t kMaximumVolumeRayIntegrationSamples = 4'000'000;
    const auto appendRayMarchSamples = [&](const QVector<float> &sceneDepth,
                                           const QVector<bool> &frontCoverage) {
        std::size_t remainingIntegrationSamples =
            kMaximumVolumeRayIntegrationSamples;
        for (const RayMarchVolume &rayMarchVolume : rayMarchVolumes) {
            if (remainingIntegrationSamples == 0) break;
            const ViewportRenderObject &object = rayMarchVolume.object;
            QVector<RasterWorkbenchVolumeRaySample> samples;
            std::size_t integratedSampleCount = 0;
            const bool sampled = rasterWorkbenchVolumeRaySamples(
                object, transform, viewportSize, devicePixelRatio, sceneDepth,
                frontCoverage, remainingIntegrationSamples,
                &integratedSampleCount, &samples);
            integratedSampleCount = std::min(integratedSampleCount,
                                             remainingIntegrationSamples);
            remainingIntegrationSamples -= integratedSampleCount;
            if (!sampled) continue;
            const std::size_t sampleCount =
                static_cast<std::size_t>(samples.size());
            if (sampleCount > kMaximumVolumeSamples - retainedVolumeSamples)
                return false;
            retainedVolumeSamples += sampleCount;
            const QVector3D baseColor = volumeBaseColor(object);
            for (const RasterWorkbenchVolumeRaySample &sample : samples) {
                if (sample.x < 0 || sample.x >= width || sample.y < 0 ||
                    sample.y >= height) continue;
                VolumeSlicePixel pixel;
                pixel.depth = sample.cameraDepth;
                pixel.objectOrder = rayMarchVolume.objectOrder;
                pixel.depthPreClipped = true;
                pixel.color = {
                    sample.premultipliedColor.red * baseColor.x(),
                    sample.premultipliedColor.green * baseColor.y(),
                    sample.premultipliedColor.blue * baseColor.z(),
                    sample.premultipliedColor.alpha};
                volumeSlicePixels[static_cast<std::size_t>(
                    sample.y * width + sample.x)].push_back(pixel);
            }
        }
        for (auto &pixelSamples : volumeSlicePixels) {
            std::stable_sort(pixelSamples.begin(), pixelSamples.end(),
                             [](const VolumeSlicePixel &left,
                                const VolumeSlicePixel &right) {
                                 return left.objectOrder < right.objectOrder;
                             });
        }
        return true;
    };
    if (surfaceLayers != nullptr) {
        surfaceLayers->cavityAppliedBeforeTransparency = false;
        surfaceLayers->outlineAppliedBeforeDisplay = false;
        surfaceLayers->sceneLinearOutlinePending = false;
        surfaceLayers->sceneLinearDofPending = false;
        surfaceLayers->sceneLinearColor.clear();
        surfaceLayers->sceneLinearOpaqueColor.clear();
        surfaceLayers->sceneLinearTransparentColor.clear();
        surfaceLayers->sceneLinearShadowedTransparentColor.clear();
        surfaceLayers->sceneLinearShadowedTransparentColorAvailable = false;
        surfaceLayers->sceneLinearOutlineCoverage.clear();
        surfaceLayers->sceneLinearSize = QSize();
        surfaceLayers->opaqueSceneDepth.clear();
        surfaceLayers->opaqueSceneDepthSize = QSize();
        surfaceLayers->regularSceneDepth.clear();
        surfaceLayers->regularSceneDepthSize = QSize();
        surfaceLayers->inFrontSceneDepth.clear();
        surfaceLayers->inFrontSceneDepthSize = QSize();
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
        std::array<float, 3> sceneLinearRgb{};
        float alpha = 0.0f;
    };
    // Bound storage independently from viewport size. If the visible layer
    // count exceeds this budget, let the caller use its existing sorted path.
    constexpr qsizetype kMaximumTransparentFragments = 4'000'000;
    QVector<TransparentFragment> regularTransparentFragments;
    QVector<TransparentFragment> frontTransparentFragments;
    const qreal dpr = devicePixelRatio;
    const Point3D cameraPosition = transform.cameraPosition(viewportSize);
    const Point3D viewDirection = transform.viewDirection();
    bool fragmentGeneratedDepthUnavailable = false;

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
                    // The view direction points outward toward the camera,
                    // so negate its dot product to get positive camera
                    // distance for reciprocal interpolation. The resolved
                    // depth restores the negative sign below, where nearer
                    // values compare greater in this buffer.
                    viewDepth[corner] = -static_cast<float>(
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
                        z = -1.0 / reciprocal;
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
        if (allowFragmentGeneratedDepth &&
            !object.preparedDepthGeometry && fragmentSink != nullptr) {
            if (fragmentSink->empty()) {
                fragmentGeneratedDepthUnavailable = true;
            } else {
                objectDepth.fill(noDepth, pixelCount);
                for (const RasterWorkbenchLinearFragment &fragment :
                     *fragmentSink) {
                    if (fragment.x < 0 || fragment.x >= width ||
                        fragment.y < 0 || fragment.y >= height ||
                        !std::isfinite(fragment.cameraDepth)) continue;
                    float &stored = objectDepth[fragment.y * width + fragment.x];
                    stored = std::max(stored, fragment.cameraDepth);
                }
            }
        }
        consume(layer);
    };

    // Compute cavity while opaque depth and normals are available. Weighted
    // OIT consumes this in its scene-linear opaque buffer; the sorted fallback
    // applies it to its retained opaque layer before transparency composition.
    std::vector<float> regularCavityFactors;
    std::vector<float> inFrontCavityFactors;
    if (shadingSettings.cavity && !shadingSettings.xrayEnabled() &&
        (useWeightedOit || surfaceLayers != nullptr)) {
        ViewportShadingSettings cavityOnly = shadingSettings;
        cavityOnly.outline = false;
        cavityOnly.depthOfField = false;
        const auto computeCavityFactors = [&](
            std::vector<float> *factors,
            RasterWorkbenchCavityTapCapture *tapCapture) {
            if (objects.isEmpty()) return;
            QImage cavityProbe(colorImage->size(),
                               QImage::Format_ARGB32_Premultiplied);
            cavityProbe.setDevicePixelRatio(colorImage->devicePixelRatioF());
            cavityProbe.fill(Qt::white);
            (void)applyRasterWorkbenchEffects(
                &cavityProbe, objects, transform, viewportSize,
                devicePixelRatio, cavityOnly, nullptr, factors, tapCapture);
        };
        computeCavityFactors(&regularCavityFactors, cavityTapCapture);
        // Blender builds cavity from the combined opaque depth and normal
        // inputs, with visible in-front samples taking ownership at covered
        // pixels. Apply those same factors to whichever opaque layer is
        // composited at each pixel.
        inFrontCavityFactors = regularCavityFactors;
    }
    if (cavityFactorCapture != nullptr)
        *cavityFactorCapture = regularCavityFactors;

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
        renderObject(object, &objectFragments,
                     [&](const QImage &layer) {
            for (int y = 0; y < height; ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                for (int x = 0; x < width; ++x) {
                    const int index = y * width + x;
                    const QRgb pixel = row[x];
                    if (qAlpha(pixel) < 255 || xrayEnabled ||
                        objectDepth[index] == noDepth)
                        continue;
                    if (objectDepth[index] >= domainOpaqueDepth[index]) {
                        domainOpaqueDepth[index] = objectDepth[index];
                        domainOpaqueColor[index] = pixel;
                        domainOpaqueObject[index] = objectIndex;
                    }
                }
            }
            {
                QVector<int> nearestOpaqueFragment(pixelCount, -1);
                for (int fragmentIndex = 0;
                     fragmentIndex < static_cast<int>(objectFragments.size());
                     ++fragmentIndex) {
                    const RasterWorkbenchLinearFragment &fragment =
                        objectFragments[static_cast<std::size_t>(fragmentIndex)];
                    if (!isOpaqueWorkbenchAlpha(fragment.alpha, xrayEnabled) ||
                        fragment.x < 0 ||
                        fragment.x >= width || fragment.y < 0 ||
                        fragment.y >= height) continue;
                    const int index = fragment.y * width + fragment.x;
                    const int previous = nearestOpaqueFragment[index];
                    if (previous < 0 || fragment.cameraDepth <=
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
        if (fragmentGeneratedDepthUnavailable) return false;
        if (objectFragments.size() >
                static_cast<std::size_t>(kMaximumTransparentFragments)) {
            return false;
        }
    }

    if (useWeightedOit) {
        std::vector<WeightedOitAccumulation> regularOit = makeOitBuffer();
        std::vector<WeightedOitAccumulation> frontOit = makeOitBuffer();
        const bool accumulateShadowedTransparency =
            surfaceLayers != nullptr && shadingSettings.shadows &&
            !shadingSettings.xrayEnabled();
        bool completeShadowReceiverData = accumulateShadowedTransparency;
        QVector3D transparentLightDirection(
            -shadingSettings.shadowDirection.x(),
            shadingSettings.shadowDirection.z(),
            -shadingSettings.shadowDirection.y());
        if (transparentLightDirection.lengthSquared() < 1.0e-10f) {
            transparentLightDirection =
                QVector3D(-0.57735026f, 0.57735026f, -0.57735026f);
        }
        transparentLightDirection.normalize();
        QVector<float> nearestRegularTransparentDepth(pixelCount, noDepth);
        QVector<float> nearestFrontTransparentDepth(pixelCount, noDepth);
        QVector<float> regularTransparentOverlayDepth(pixelCount, noDepth);
        QVector<float> frontTransparentOverlayDepth(pixelCount, noDepth);
        QVector<float> regularTransparentOutlineDepth(pixelCount, noDepth);
        QVector<float> frontTransparentOutlineDepth(pixelCount, noDepth);
        std::vector<quint64> regularTransparentOutlineIds(
            static_cast<std::size_t>(pixelCount), 0);
        std::vector<quint64> frontTransparentOutlineIds(
            static_cast<std::size_t>(pixelCount), 0);
        const std::vector<RasterWorkbenchLinearPixel> emptyBase(
            static_cast<std::size_t>(pixelCount));
        for (int objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
            const ViewportRenderObject &object = objects[objectIndex];
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
                if (fragment.alpha <= 0.0f ||
                    isOpaqueWorkbenchAlpha(fragment.alpha, xrayEnabled) ||
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
                std::vector<WeightedOitAccumulation> &domainOit = inFront
                    ? frontOit : regularOit;
                float transparentShadowFactor = 1.0f;
                if (accumulateShadowedTransparency &&
                    completeShadowReceiverData) {
                    if (!fragment.hasShadowReceiver) {
                        completeShadowReceiverData = false;
                    } else {
                        const QVector3D normal = normalized(QVector3D(
                            fragment.worldNormal[0], fragment.worldNormal[1],
                            fragment.worldNormal[2]));
                        if (normal.lengthSquared() <= 1.0e-12f) {
                            completeShadowReceiverData = false;
                        } else {
                            transparentShadowFactor =
                                rasterWorkbenchShadowFactor(
                                normal, transparentLightDirection,
                                static_cast<float>(
                                    shadingSettings.shadowIntensity),
                                static_cast<float>(
                                    shadingSettings.shadowOffset),
                                static_cast<float>(
                                    shadingSettings.shadowFocus), false);
                        }
                    }
                }
                addOitFragment(domainOit, fragment,
                               transparentShadowFactor);
                QVector<float> &domainTransparentDepth = inFront
                    ? nearestFrontTransparentDepth
                    : nearestRegularTransparentDepth;
                if (domainTransparentDepth[index] == noDepth ||
                    fragment.cameraDepth < domainTransparentDepth[index]) {
                    domainTransparentDepth[index] = fragment.cameraDepth;
                }
                QVector<float> &outlineDepth = inFront
                    ? frontTransparentOutlineDepth
                    : regularTransparentOutlineDepth;
                std::vector<quint64> &outlineIds = inFront
                    ? frontTransparentOutlineIds
                    : regularTransparentOutlineIds;
                if (outlineDepth[index] == noDepth ||
                    fragment.cameraDepth < outlineDepth[index]) {
                    outlineDepth[index] = fragment.cameraDepth;
                    outlineIds[static_cast<std::size_t>(index)] =
                        object.objectId.value();
                }
                QVector<float> &domainOverlayDepth = inFront
                    ? frontTransparentOverlayDepth
                    : regularTransparentOverlayDepth;
                domainOverlayDepth[index] = std::max(
                    domainOverlayDepth[index], fragment.cameraDepth);
            }
        }

        const QVector3D background = sceneLinearBackground.value_or(
            QVector3D(0.0f, 0.0f, 0.0f));
        const bool hasPixelBackground =
            sceneLinearBackgroundPixels.size() ==
            static_cast<std::size_t>(pixelCount);
        bool sceneLinearDofBackgroundAvailable =
            hasPixelBackground || sceneLinearBackground.has_value();
        if (!sceneLinearDofBackgroundAvailable &&
            width == colorImage->width() && height == colorImage->height()) {
            sceneLinearDofBackgroundAvailable = true;
            for (int y = 0; y < height && sceneLinearDofBackgroundAvailable; ++y) {
                const auto *row = reinterpret_cast<const QRgb *>(
                    colorImage->constScanLine(y));
                for (int x = 0; x < width; ++x) {
                    if (qAlpha(row[x]) != 0) {
                        sceneLinearDofBackgroundAvailable = false;
                        break;
                    }
                }
            }
        }
        const bool retainWeightedSceneLinearDof =
            surfaceLayers != nullptr && shadingSettings.depthOfField &&
            !shadingSettings.xrayEnabled() &&
            transform.cameraPreferences().sceneCameraEnabled &&
            width == colorImage->width() && height == colorImage->height() &&
            sceneLinearDofBackgroundAvailable;
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
        QVector<bool> inFrontCovered(pixelCount, false);
        for (int index = 0; index < pixelCount; ++index) {
            inFrontCovered[index] = frontOpaqueSceneDepth[index] != noDepth ||
                nearestFrontTransparentDepth[index] != noDepth;
        }
        QVector<float> raySceneDepth(pixelCount, noDepth);
        for (int index = 0; index < pixelCount; ++index) {
            const float opaqueDepth = opaqueSceneDepth[index];
            const float transparentDepth = nearestRegularTransparentDepth[index];
            raySceneDepth[index] = opaqueDepth == noDepth
                ? transparentDepth
                : transparentDepth == noDepth
                    ? opaqueDepth : std::min(opaqueDepth, transparentDepth);
        }
        if (!appendRayMarchSamples(raySceneDepth, inFrontCovered)) return false;
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
        std::vector<RasterWorkbenchLinearPixel> resolved =
            resolveOit(regularOit, basePixels);
        for (int index = 0; index < pixelCount; ++index) {
            resolved[static_cast<std::size_t>(index)] =
                compositeVolumeSlices(
                    resolved[static_cast<std::size_t>(index)],
                    volumePixelsAt(index), opaqueSceneDepth[index],
                    nearestRegularTransparentDepth[index],
                    inFrontCovered[index]);
        }
        std::vector<RasterWorkbenchLinearPixel> transparentResolved =
            resolveOit(regularOit, emptyBase);
        std::vector<RasterWorkbenchLinearPixel> shadowedTransparentResolved;
        if (accumulateShadowedTransparency && completeShadowReceiverData) {
            shadowedTransparentResolved =
                resolveOit(regularOit, emptyBase, true);
        }
        std::vector<RasterWorkbenchLinearPixel> frontBasePixels(
            static_cast<std::size_t>(pixelCount));
        const std::vector<RasterWorkbenchLinearPixel>
            regularOpaqueScenePixels = opaqueScenePixels;
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
        std::vector<RasterWorkbenchLinearPixel> frontResolved =
            resolveOit(frontOit, frontBasePixels);
        const std::vector<RasterWorkbenchLinearPixel> frontTransparentResolved =
            resolveOit(frontOit, emptyBase);
        std::vector<RasterWorkbenchLinearPixel> frontShadowedTransparentResolved;
        if (accumulateShadowedTransparency && completeShadowReceiverData) {
            frontShadowedTransparentResolved =
                resolveOit(frontOit, emptyBase, true);
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
            if (!shadowedTransparentResolved.empty() &&
                !frontShadowedTransparentResolved.empty()) {
                RasterWorkbenchLinearPixel &shadowed =
                    shadowedTransparentResolved[
                        static_cast<std::size_t>(index)];
                const RasterWorkbenchLinearPixel &frontShadowed =
                    frontShadowedTransparentResolved[
                        static_cast<std::size_t>(index)];
                const float remainFrontShadowed =
                    1.0f - frontShadowed.alpha;
                shadowed = {
                    frontShadowed.red + shadowed.red * remainOpaque *
                                           remainFrontShadowed,
                    frontShadowed.green + shadowed.green * remainOpaque *
                                             remainFrontShadowed,
                    frontShadowed.blue + shadowed.blue * remainOpaque *
                                            remainFrontShadowed,
                    transparent.alpha};
            }
        }
        const bool canResolveOutlineInSceneLinear =
            surfaceLayers != nullptr && shadingSettings.outline;
        if (surfaceLayers != nullptr && shadingSettings.shadows &&
            !shadingSettings.xrayEnabled()) {
            surfaceLayers->sceneLinearColor = resolved;
            surfaceLayers->sceneLinearSize = QSize(width, height);
            surfaceLayers->sceneLinearOpaqueColor = opaqueScenePixels;
            surfaceLayers->sceneLinearRegularOpaqueColor =
                regularOpaqueScenePixels;
            surfaceLayers->sceneLinearInFrontOpaqueColor =
                frontOpaqueScenePixels;
            surfaceLayers->sceneLinearTransparentColor = transparentResolved;
            if (!shadowedTransparentResolved.empty()) {
                surfaceLayers->sceneLinearShadowedTransparentColor =
                    std::move(shadowedTransparentResolved);
                surfaceLayers->sceneLinearShadowedTransparentColorAvailable =
                    true;
            }
        }
        if (retainWeightedSceneLinearDof) {
            surfaceLayers->sceneLinearColor = resolved;
            surfaceLayers->sceneLinearSize = QSize(width, height);
            surfaceLayers->sceneLinearDofPending = true;
        }
        if (canResolveOutlineInSceneLinear) {
            std::vector<quint64> regularObjectIds(
                static_cast<std::size_t>(pixelCount), 0);
            std::vector<quint64> frontObjectIds(
                static_cast<std::size_t>(pixelCount), 0);
            for (int index = 0; index < pixelCount; ++index) {
                const int regularIndex = opaqueObject[index];
                if (regularIndex >= 0 && regularIndex < objects.size()) {
                    regularObjectIds[static_cast<std::size_t>(index)] =
                        objects[regularIndex].objectId.value();
                }
                const int frontIndex = frontOpaqueObject[index];
                if (frontIndex >= 0 && frontIndex < objects.size()) {
                    frontObjectIds[static_cast<std::size_t>(index)] =
                        objects[frontIndex].objectId.value();
                }
                if (regularTransparentOutlineIds[
                        static_cast<std::size_t>(index)] != 0) {
                    regularObjectIds[static_cast<std::size_t>(index)] =
                        regularTransparentOutlineIds[
                            static_cast<std::size_t>(index)];
                }
                if (frontTransparentOutlineIds[
                        static_cast<std::size_t>(index)] != 0) {
                    frontObjectIds[static_cast<std::size_t>(index)] =
                        frontTransparentOutlineIds[
                            static_cast<std::size_t>(index)];
                }
            }
            std::vector<quint64> visibleObjectIds(
                static_cast<std::size_t>(pixelCount), 0);
            for (int index = 0; index < pixelCount; ++index) {
                const std::size_t pixelIndex =
                    static_cast<std::size_t>(index);
                visibleObjectIds[pixelIndex] = frontObjectIds[pixelIndex] != 0
                    ? frontObjectIds[pixelIndex] : regularObjectIds[pixelIndex];
            }
            const QColor outline = shadingSettings.outlineColor;
            const QVector3D outlineColor = workbenchSrgbToSceneLinear(
                QVector3D(outline.redF(), outline.greenF(), outline.blueF()));
            const float outlineAlpha = static_cast<float>(outline.alphaF());
            const int sampleOffset = workbenchEffectUiScale(dpr);
            surfaceLayers->sceneLinearColor = resolved;
            surfaceLayers->sceneLinearSize = QSize(width, height);
            const bool deferOutlineUntilEffects =
                !shadingSettings.xrayEnabled() &&
                (shadingSettings.shadows || shadingSettings.depthOfField);
            if (deferOutlineUntilEffects) {
                surfaceLayers->sceneLinearOutlineCoverage.assign(
                    static_cast<std::size_t>(pixelCount), 0.0f);
                surfaceLayers->sceneLinearOutlinePending = true;
                surfaceLayers->sceneLinearDofPending =
                    shadingSettings.depthOfField &&
                    !shadingSettings.xrayEnabled() &&
                    transform.cameraPreferences().sceneCameraEnabled;
            }
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int index = y * width + x;
                    const quint64 objectId = visibleObjectIds[
                        static_cast<std::size_t>(index)];
                    int differentNeighbors = 0;
                    const std::array<QPoint, 4> offsets = {
                        QPoint(sampleOffset, 0), QPoint(-sampleOffset, 0),
                        QPoint(0, sampleOffset), QPoint(0, -sampleOffset)};
                    for (const QPoint &offset : offsets) {
                        const int sx = std::clamp(x + offset.x(), 0, width - 1);
                        const int sy = std::clamp(y + offset.y(), 0, height - 1);
                        differentNeighbors +=
                            visibleObjectIds[static_cast<std::size_t>(
                                sy * width + sx)] != objectId;
                    }
                    const float alpha = outlineAlpha *
                        (static_cast<float>(differentNeighbors) / 4.0f);
                    if (alpha <= 0.0f) continue;
                    if (deferOutlineUntilEffects) {
                        surfaceLayers->sceneLinearOutlineCoverage[
                            static_cast<std::size_t>(index)] = alpha;
                        continue;
                    }
                    RasterWorkbenchLinearPixel &pixel = resolved[
                        static_cast<std::size_t>(index)];
                    const float remain = 1.0f - alpha;
                    pixel.red = outlineColor.x() * alpha + pixel.red * remain;
                    pixel.green = outlineColor.y() * alpha + pixel.green * remain;
                    pixel.blue = outlineColor.z() * alpha + pixel.blue * remain;
                    pixel.alpha = alpha + pixel.alpha * remain;
                }
            }
            surfaceLayers->outlineAppliedBeforeDisplay =
                !deferOutlineUntilEffects;
        }
        if (surfaceLayers != nullptr) {
            // Keep the bounded scene-linear resolve available for camera
            // overlays that Blender composites after effects but before the
            // final display transform.
            surfaceLayers->sceneLinearColor = resolved;
            surfaceLayers->sceneLinearSize = QSize(width, height);
        }
        if (sceneLinearCapture != nullptr) {
            *sceneLinearCapture = resolved;
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
        if (surfaceLayers != nullptr) {
            surfaceLayers->opaqueSceneDepth = opaqueSceneDepth;
            surfaceLayers->opaqueSceneDepthSize = QSize(width, height);
            surfaceLayers->regularSceneDepth = opaqueSceneDepth;
            surfaceLayers->inFrontSceneDepth = frontOpaqueSceneDepth;
            for (int index = 0; index < pixelCount; ++index) {
                surfaceLayers->regularSceneDepth[index] = std::max(
                    surfaceLayers->regularSceneDepth[index],
                    regularTransparentOverlayDepth[index]);
                surfaceLayers->inFrontSceneDepth[index] = std::max(
                    surfaceLayers->inFrontSceneDepth[index],
                    frontTransparentOverlayDepth[index]);
            }
            surfaceLayers->regularSceneDepthSize = QSize(width, height);
            surfaceLayers->inFrontSceneDepthSize = QSize(width, height);
        }
        return true;
    }

    const bool haveRegularFactors =
        regularCavityFactors.size() == static_cast<std::size_t>(pixelCount);
    const bool haveFrontFactors =
        inFrontCavityFactors.size() == static_cast<std::size_t>(pixelCount);
    if (haveRegularFactors || haveFrontFactors) {
        for (int index = 0; index < pixelCount; ++index) {
            RasterWorkbenchLinearPixel &regular =
                opaqueScenePixels[static_cast<std::size_t>(index)];
            RasterWorkbenchLinearPixel &front =
                frontOpaqueScenePixels[static_cast<std::size_t>(index)];
            if (haveRegularFactors) {
                const float factor = regularCavityFactors[
                    static_cast<std::size_t>(index)];
                regular.red *= factor;
                regular.green *= factor;
                regular.blue *= factor;
            }
            if (haveFrontFactors) {
                const float factor = inFrontCavityFactors[
                    static_cast<std::size_t>(index)];
                front.red *= factor;
                front.green *= factor;
                front.blue *= factor;
            }
        }
        if (surfaceLayers != nullptr)
            surfaceLayers->cavityAppliedBeforeTransparency = true;
    }

    // The sorted fallback retains its per-pixel far-to-near order, but now
    // composites the captured Workbench fragments in scene-linear color.
    bool transparentFragmentBudgetExceeded = false;
    QVector<float> regularTransparentOutlineDepth(pixelCount, noDepth);
    QVector<float> frontTransparentOutlineDepth(pixelCount, noDepth);
    std::vector<quint64> regularTransparentOutlineIds(
        static_cast<std::size_t>(pixelCount), 0);
    std::vector<quint64> frontTransparentOutlineIds(
        static_cast<std::size_t>(pixelCount), 0);
    for (int objectOrder = 0; objectOrder < objects.size(); ++objectOrder) {
        const ViewportRenderObject &object = objects[objectOrder];
        RasterWorkbenchFragmentSink objectFragments;
        renderObject(object, &objectFragments, [&](const QImage &) {
            if (objectFragments.size() >
                static_cast<std::size_t>(kMaximumTransparentFragments)) {
                transparentFragmentBudgetExceeded = true;
                return;
            }
        });
        if (transparentFragmentBudgetExceeded) return false;
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
        const QVector<float> &domainOpaqueDepth = object.showInFront
            ? frontOpaqueSceneDepth : opaqueSceneDepth;
        for (const RasterWorkbenchLinearFragment &fragment : objectFragments) {
            if (fragment.alpha <= 0.0f ||
                isOpaqueWorkbenchAlpha(fragment.alpha, xrayEnabled) ||
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
            // Each Workbench transparent domain depth-tests against its own
            // opaque pass before resolving. In-front fragments bypass regular
            // scene depth, but still remain hidden by nearer in-front opaque
            // geometry.
            if (fragment.cameraDepth <= domainOpaqueDepth[index]) continue;
            QVector<float> &outlineDepth = object.showInFront
                ? frontTransparentOutlineDepth
                : regularTransparentOutlineDepth;
            std::vector<quint64> &outlineIds = object.showInFront
                ? frontTransparentOutlineIds
                : regularTransparentOutlineIds;
            if (outlineDepth[index] == noDepth ||
                fragment.cameraDepth < outlineDepth[index]) {
                outlineDepth[index] = fragment.cameraDepth;
                outlineIds[static_cast<std::size_t>(index)] =
                    object.objectId.value();
            }
            if (regularTransparentFragments.size() +
                    frontTransparentFragments.size() >=
                kMaximumTransparentFragments) {
                transparentFragmentBudgetExceeded = true;
                break;
            }
            (object.showInFront ? frontTransparentFragments
                                : regularTransparentFragments).append(
                {index, fragment.cameraDepth, objectOrder,
                 fragment.sceneLinearRgb,
                 fragment.alpha});
        }
        if (transparentFragmentBudgetExceeded) return false;
    }

    const auto sortPixelFragments = [](QVector<TransparentFragment> *fragments) {
        std::sort(fragments->begin(), fragments->end(),
                  [](const TransparentFragment &left,
                     const TransparentFragment &right) {
                      if (left.pixelIndex != right.pixelIndex)
                          return left.pixelIndex < right.pixelIndex;
                      if (left.depth != right.depth)
                          return left.depth < right.depth;
                      return left.objectOrder < right.objectOrder;
                  });
    };
    sortPixelFragments(&regularTransparentFragments);
    sortPixelFragments(&frontTransparentFragments);
    std::vector<RasterWorkbenchLinearPixel> regularTransparentPixels(
        static_cast<std::size_t>(pixelCount));
    std::vector<RasterWorkbenchLinearPixel> frontTransparentPixels(
        static_cast<std::size_t>(pixelCount));
    const auto accumulateLinearFragments = [](
        const QVector<TransparentFragment> &fragments,
        std::vector<RasterWorkbenchLinearPixel> *pixels) {
        for (const TransparentFragment &fragment : fragments) {
            RasterWorkbenchLinearPixel &pixel =
                (*pixels)[static_cast<std::size_t>(fragment.pixelIndex)];
            const float remain = 1.0f - fragment.alpha;
            pixel.red = fragment.sceneLinearRgb[0] * fragment.alpha +
                        pixel.red * remain;
            pixel.green = fragment.sceneLinearRgb[1] * fragment.alpha +
                          pixel.green * remain;
            pixel.blue = fragment.sceneLinearRgb[2] * fragment.alpha +
                         pixel.blue * remain;
            pixel.alpha = fragment.alpha + pixel.alpha * remain;
        }
    };
    accumulateLinearFragments(regularTransparentFragments,
                             &regularTransparentPixels);
    accumulateLinearFragments(frontTransparentFragments,
                             &frontTransparentPixels);
    const auto overLinear = [](const RasterWorkbenchLinearPixel &front,
                               const RasterWorkbenchLinearPixel &back) {
        const float remain = 1.0f - front.alpha;
        return RasterWorkbenchLinearPixel{
            front.red + back.red * remain,
            front.green + back.green * remain,
            front.blue + back.blue * remain,
            front.alpha + back.alpha * remain};
    };
    const bool hasPixelBackground = sceneLinearBackgroundPixels.size() ==
        static_cast<std::size_t>(pixelCount);
    const auto displayPremultiplied = [&](const RasterWorkbenchLinearPixel &pixel) {
        if (pixel.alpha <= 0.0f) return qRgba(0, 0, 0, 0);
        const float inverseAlpha = 1.0f / pixel.alpha;
        const QVector3D display = workbenchSceneLinearToDisplaySrgb(
            QVector3D(pixel.red * inverseAlpha, pixel.green * inverseAlpha,
                      pixel.blue * inverseAlpha),
            shadingSettings.colorTransform, shadingSettings.colorExposure,
            shadingSettings.colorGamma, shadingSettings.colorLutData,
            shadingSettings.colorLutMetadata);
        return qPremultiply(qRgba(
            std::clamp(qRound(display.x() * 255.0f), 0, 255),
            std::clamp(qRound(display.y() * 255.0f), 0, 255),
            std::clamp(qRound(display.z() * 255.0f), 0, 255),
            std::clamp(qRound(pixel.alpha * 255.0f), 0, 255)));
    };
    const auto compositeDisplay = [](QRgb foreground, QRgb background) {
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

    // The sorted fallback can retain an exact scene-linear resolve when its
    // destination is empty or its background was supplied in scene-linear
    // form. In that case, resolve the outline here before display conversion,
    // matching the weighted compositor path.
    bool sceneLinearDestinationValid =
        hasPixelBackground || sceneLinearBackground.has_value();
    if (!sceneLinearDestinationValid) {
        sceneLinearDestinationValid = true;
        for (int y = 0; y < height && sceneLinearDestinationValid; ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(
                colorImage->constScanLine(y));
            for (int x = 0; x < width; ++x) {
                if (qAlpha(row[x]) != 0) {
                    sceneLinearDestinationValid = false;
                    break;
                }
            }
        }
    }
    const bool sortedSceneLinearDofPending =
        surfaceLayers != nullptr && shadingSettings.depthOfField &&
        !shadingSettings.xrayEnabled() &&
        transform.cameraPreferences().sceneCameraEnabled;
    const bool sortedXraySuppressesDof = shadingSettings.xrayEnabled();
    const bool retainSortedSceneLinear = surfaceLayers != nullptr &&
        (!shadingSettings.depthOfField || sortedSceneLinearDofPending ||
         sortedXraySuppressesDof) &&
        sceneLinearDestinationValid;
    const bool canResolveSortedOutlineInSceneLinear =
        retainSortedSceneLinear && shadingSettings.outline;
    const bool deferSortedOutlineUntilEffects =
        canResolveSortedOutlineInSceneLinear && shadingSettings.shadows &&
        !shadingSettings.xrayEnabled();
    std::vector<quint64> sortedVisibleObjectIds;
    QVector<float> volumeRegularTransparentDepth(pixelCount, noDepth);
    QVector<float> volumeFrontTransparentDepth(pixelCount, noDepth);
    for (const TransparentFragment &fragment : regularTransparentFragments) {
        if (fragment.pixelIndex >= 0 && fragment.pixelIndex < pixelCount &&
            fragment.alpha > 0.0f) {
            float &depth = volumeRegularTransparentDepth[fragment.pixelIndex];
            if (depth == noDepth || fragment.depth < depth)
                depth = fragment.depth;
        }
    }
    for (const TransparentFragment &fragment : frontTransparentFragments) {
        if (fragment.pixelIndex >= 0 && fragment.pixelIndex < pixelCount &&
            fragment.alpha > 0.0f) {
            float &depth = volumeFrontTransparentDepth[fragment.pixelIndex];
            if (depth == noDepth || fragment.depth < depth)
                depth = fragment.depth;
        }
    }
    if (!useWeightedOit) {
        QVector<float> raySceneDepth(pixelCount, noDepth);
        QVector<bool> rayInFrontCoverage(pixelCount, false);
        for (int index = 0; index < pixelCount; ++index) {
            const float opaqueDepth = opaqueSceneDepth[index];
            const float transparentDepth = volumeRegularTransparentDepth[index];
            raySceneDepth[index] = opaqueDepth == noDepth
                ? transparentDepth
                : transparentDepth == noDepth
                    ? opaqueDepth : std::min(opaqueDepth, transparentDepth);
            rayInFrontCoverage[index] =
                frontOpaqueSceneDepth[index] != noDepth ||
                volumeFrontTransparentDepth[index] != noDepth;
        }
        if (!appendRayMarchSamples(raySceneDepth, rayInFrontCoverage))
            return false;
    }
    if (canResolveSortedOutlineInSceneLinear) {
        std::vector<quint64> regularObjectIds(
            static_cast<std::size_t>(pixelCount), 0);
        std::vector<quint64> frontObjectIds(
            static_cast<std::size_t>(pixelCount), 0);
        for (int index = 0; index < pixelCount; ++index) {
            const int regularIndex = opaqueObject[index];
            if (regularIndex >= 0 && regularIndex < objects.size()) {
                regularObjectIds[static_cast<std::size_t>(index)] =
                    objects[regularIndex].objectId.value();
            }
            const int frontIndex = frontOpaqueObject[index];
            if (frontIndex >= 0 && frontIndex < objects.size()) {
                frontObjectIds[static_cast<std::size_t>(index)] =
                    objects[frontIndex].objectId.value();
            }
            if (regularTransparentOutlineIds[
                    static_cast<std::size_t>(index)] != 0) {
                regularObjectIds[static_cast<std::size_t>(index)] =
                    regularTransparentOutlineIds[
                        static_cast<std::size_t>(index)];
            }
            if (frontTransparentOutlineIds[
                    static_cast<std::size_t>(index)] != 0) {
                frontObjectIds[static_cast<std::size_t>(index)] =
                    frontTransparentOutlineIds[
                        static_cast<std::size_t>(index)];
            }
        }
        sortedVisibleObjectIds.resize(static_cast<std::size_t>(pixelCount));
        for (int index = 0; index < pixelCount; ++index) {
            const std::size_t pixelIndex = static_cast<std::size_t>(index);
            sortedVisibleObjectIds[pixelIndex] =
                frontObjectIds[pixelIndex] != 0
                    ? frontObjectIds[pixelIndex] : regularObjectIds[pixelIndex];
        }
    }
    if (retainSortedSceneLinear) {
        surfaceLayers->sceneLinearSize = QSize(width, height);
        surfaceLayers->sceneLinearColor.assign(
            static_cast<std::size_t>(pixelCount), {});
        if (shadingSettings.shadows &&
            !shadingSettings.xrayEnabled()) {
            surfaceLayers->sceneLinearOpaqueColor.assign(
                static_cast<std::size_t>(pixelCount), {});
            surfaceLayers->sceneLinearTransparentColor.assign(
                static_cast<std::size_t>(pixelCount), {});
        }
        if (deferSortedOutlineUntilEffects) {
            surfaceLayers->sceneLinearOutlineCoverage.assign(
                static_cast<std::size_t>(pixelCount), 0.0f);
            surfaceLayers->sceneLinearOutlinePending = true;
        }
        surfaceLayers->sceneLinearDofPending = sortedSceneLinearDofPending;
    }
    const QVector3D sortedOutlineColor = canResolveSortedOutlineInSceneLinear
        ? workbenchSrgbToSceneLinear(QVector3D(
              shadingSettings.outlineColor.redF(),
              shadingSettings.outlineColor.greenF(),
              shadingSettings.outlineColor.blueF()))
        : QVector3D();
    const float sortedOutlineAlpha = canResolveSortedOutlineInSceneLinear
        ? static_cast<float>(shadingSettings.outlineColor.alphaF()) : 0.0f;
    const int sortedOutlineOffset = workbenchEffectUiScale(devicePixelRatio);

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
            const auto &regularOpaque = opaqueScenePixels[
                static_cast<std::size_t>(index)];
            const auto &frontOpaque = frontOpaqueScenePixels[
                static_cast<std::size_t>(index)];
            const auto &regularTransparent = regularTransparentPixels[
                static_cast<std::size_t>(index)];
            const auto &frontTransparent = frontTransparentPixels[
                static_cast<std::size_t>(index)];
            RasterWorkbenchLinearPixel background;
            if (hasPixelBackground) {
                const QVector3D &color = sceneLinearBackgroundPixels[
                    static_cast<std::size_t>(index)];
                background = RasterWorkbenchLinearPixel::fromStraight(
                    color.x(), color.y(), color.z(), 1.0f);
            } else if (sceneLinearBackground.has_value()) {
                const QVector3D &color = *sceneLinearBackground;
                background = RasterWorkbenchLinearPixel::fromStraight(
                    color.x(), color.y(), color.z(), 1.0f);
            }
            RasterWorkbenchLinearPixel regular = overLinear(regularOpaque,
                                                             background);
            regular = overLinear(regularTransparent, regular);
            regular = compositeVolumeSlices(
                regular, volumePixelsAt(index),
                opaqueSceneDepth[index],
                volumeRegularTransparentDepth[index],
                frontOpaqueSceneDepth[index] != noDepth ||
                    volumeFrontTransparentDepth[index] != noDepth);
            RasterWorkbenchLinearPixel final = overLinear(frontOpaque, regular);
            final = overLinear(frontTransparent, final);
            float sortedOutlineCoverage = 0.0f;
            if (canResolveSortedOutlineInSceneLinear) {
                const quint64 objectId = sortedVisibleObjectIds[
                    static_cast<std::size_t>(index)];
                int differentNeighbors = 0;
                const std::array<QPoint, 4> offsets = {
                    QPoint(sortedOutlineOffset, 0),
                    QPoint(-sortedOutlineOffset, 0),
                    QPoint(0, sortedOutlineOffset),
                    QPoint(0, -sortedOutlineOffset)};
                for (const QPoint &offset : offsets) {
                    const int sx = std::clamp(x + offset.x(), 0, width - 1);
                    const int sy = std::clamp(y + offset.y(), 0, height - 1);
                    differentNeighbors += sortedVisibleObjectIds[
                        static_cast<std::size_t>(sy * width + sx)] != objectId;
                }
                sortedOutlineCoverage = sortedOutlineAlpha *
                    (static_cast<float>(differentNeighbors) / 4.0f);
                if (!deferSortedOutlineUntilEffects) {
                    const float remain = 1.0f - sortedOutlineCoverage;
                    final.red = sortedOutlineColor.x() *
                                    sortedOutlineCoverage +
                                final.red * remain;
                    final.green = sortedOutlineColor.y() *
                                      sortedOutlineCoverage +
                                  final.green * remain;
                    final.blue = sortedOutlineColor.z() *
                                     sortedOutlineCoverage +
                                 final.blue * remain;
                    final.alpha = sortedOutlineCoverage +
                                  final.alpha * remain;
                } else {
                    surfaceLayers->sceneLinearOutlineCoverage[
                        static_cast<std::size_t>(index)] =
                        sortedOutlineCoverage;
                }
            }
            if (retainSortedSceneLinear) {
                surfaceLayers->sceneLinearColor[
                    static_cast<std::size_t>(index)] = final;
            }
            RasterWorkbenchLinearPixel opaqueLayer = overLinear(
                frontOpaque, regularOpaque);
            const QRgb opaqueDisplay = displayPremultiplied(opaqueLayer);
            const float frontRemain = 1.0f - frontOpaque.alpha;
            const RasterWorkbenchLinearPixel retainedRegularTransparent{
                regularTransparent.red * frontRemain,
                regularTransparent.green * frontRemain,
                regularTransparent.blue * frontRemain,
                regularTransparent.alpha * frontRemain};
            const RasterWorkbenchLinearPixel transparentLayer = overLinear(
                frontTransparent, retainedRegularTransparent);
            const QRgb transparentDisplay = displayPremultiplied(
                transparentLayer);
            if (retainSortedSceneLinear &&
                surfaceLayers->sceneLinearOpaqueColor.size() ==
                    static_cast<std::size_t>(pixelCount)) {
                const std::size_t linearIndex =
                    static_cast<std::size_t>(index);
                surfaceLayers->sceneLinearOpaqueColor[linearIndex] =
                    opaqueLayer;
                surfaceLayers->sceneLinearTransparentColor[linearIndex] =
                    transparentLayer;
            }
            if (opaqueRow != nullptr) opaqueRow[x] = opaqueDisplay;
            if (transparentRow != nullptr)
                transparentRow[x] = transparentDisplay;

            const QRgb combined = displayPremultiplied(final);
            destination[x] = compositeDisplay(combined, destination[x]);
        }
    }
    if (canResolveSortedOutlineInSceneLinear)
        surfaceLayers->outlineAppliedBeforeDisplay =
            !deferSortedOutlineUntilEffects;
    if (surfaceLayers != nullptr) {
        surfaceLayers->opaqueSceneDepth = opaqueSceneDepth;
        surfaceLayers->opaqueSceneDepthSize = QSize(width, height);
        surfaceLayers->regularSceneDepth = opaqueSceneDepth;
        surfaceLayers->inFrontSceneDepth = frontOpaqueSceneDepth;
        for (const TransparentFragment &fragment : regularTransparentFragments) {
            if (fragment.alpha <= 0.0f || fragment.pixelIndex < 0 ||
                fragment.pixelIndex >= pixelCount) continue;
            surfaceLayers->regularSceneDepth[fragment.pixelIndex] = std::max(
                surfaceLayers->regularSceneDepth[fragment.pixelIndex],
                fragment.depth);
        }
        for (const TransparentFragment &fragment : frontTransparentFragments) {
            if (fragment.alpha <= 0.0f || fragment.pixelIndex < 0 ||
                fragment.pixelIndex >= pixelCount) continue;
            surfaceLayers->inFrontSceneDepth[fragment.pixelIndex] = std::max(
                surfaceLayers->inFrontSceneDepth[fragment.pixelIndex],
                fragment.depth);
        }
        surfaceLayers->regularSceneDepthSize = QSize(width, height);
        surfaceLayers->inFrontSceneDepthSize = QSize(width, height);
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
                                 RasterWorkbenchSurfaceLayers *surfaceLayers,
                                 std::vector<float> *cavityFactors,
                                 RasterWorkbenchCavityTapCapture *cavityTapCapture,
                                 const QVector<float> *committedCurveDepth,
                                 const QVector<float> *committedInFrontCurveDepth,
                                 const QSize &committedCurveDepthSize,
                                 qreal cameraFrameSensorScale,
                                 bool smoothWiresOverlay,
                                 bool sceneCameraView)
{
    const bool xrayEnabled = settings.xrayEnabled();
    const bool selectedOutlineEnabled =
        settings.selectedObjectOutlineVisible && settings.wireOverlaysVisible;
    if (colorImage == nullptr || colorImage->isNull() ||
        (!settings.cavity && !settings.outline && !selectedOutlineEnabled &&
         !settings.depthOfField && !settings.shadows) ||
        settings.mode != ViewportShadingMode::Solid ||
        (xrayEnabled && !settings.outline && !selectedOutlineEnabled) ||
        viewportSize.isEmpty()) {
        return;
    }

    // Outlines use all visible geometry. Cavity uses a separate opaque-only
    // depth/normal buffer so transparent surfaces neither occlude its samples
    // nor receive its shading.
    const bool cavityEnabled = settings.cavity && !xrayEnabled &&
        !(surfaceLayers != nullptr &&
          surfaceLayers->cavityAppliedBeforeTransparency);
    const bool shadowEnabled = settings.shadows && !xrayEnabled;
    const ViewportCameraPreferences cameraOptics = transform.cameraPreferences();
    const bool perspective = transform.isPerspectiveEnabled();
    const bool depthOfFieldEnabled = settings.depthOfField && !xrayEnabled &&
                                     cameraOptics.sceneCameraEnabled &&
                                     sceneCameraView;

    const int outputWidth = colorImage->width();
    const int outputHeight = colorImage->height();
    const qreal colorDpr = std::max<qreal>(colorImage->devicePixelRatio(), 1.0e-6);
    const bool hasDeferredSceneLinearOutline = surfaceLayers != nullptr &&
        surfaceLayers->sceneLinearOutlinePending &&
        surfaceLayers->sceneLinearSize == QSize(outputWidth, outputHeight) &&
        surfaceLayers->sceneLinearColor.size() ==
            static_cast<std::size_t>(outputWidth) * outputHeight &&
        surfaceLayers->sceneLinearOutlineCoverage.size() ==
            static_cast<std::size_t>(outputWidth) * outputHeight;
    qreal dpr = std::max<qreal>(devicePixelRatio, 1.0);
    int width = qRound(viewportSize.width() * dpr);
    int height = qRound(viewportSize.height() * dpr);
    if (width <= 0 || height <= 0 || outputWidth <= 0 || outputHeight <= 0)
        return;
    constexpr qint64 maximumRasterEffectPixels = 1'000'000;
    qint64 pixelCountWide = qint64(width) * qint64(height);
    if (pixelCountWide > maximumRasterEffectPixels) {
        // Keep software effects enabled on large displays while bounding the
        // depth, normal, and object-ID buffers. The final pass maps this
        // lower-resolution result over the full-resolution scene image.
        dpr *= std::sqrt(static_cast<qreal>(maximumRasterEffectPixels) /
                         static_cast<qreal>(pixelCountWide));
        width = std::max(1, qFloor(viewportSize.width() * dpr));
        height = std::max(1, qFloor(viewportSize.height() * dpr));
        pixelCountWide = qint64(width) * qint64(height);
    }
    if (pixelCountWide > std::numeric_limits<int>::max()) return;
    const int pixelCount = static_cast<int>(pixelCountWide);
    const auto displayRetainedSceneLinear = [&]() {
        if (surfaceLayers == nullptr ||
            surfaceLayers->sceneLinearSize != QSize(outputWidth, outputHeight) ||
            surfaceLayers->sceneLinearColor.size() !=
                static_cast<std::size_t>(outputWidth) * outputHeight) {
            return false;
        }
        QImage resolvedImage(outputWidth, outputHeight,
                             QImage::Format_ARGB32_Premultiplied);
        resolvedImage.setDevicePixelRatio(colorImage->devicePixelRatioF());
        for (int y = 0; y < outputHeight; ++y) {
            auto *row = reinterpret_cast<QRgb *>(resolvedImage.scanLine(y));
            for (int x = 0; x < outputWidth; ++x) {
                const RasterWorkbenchLinearPixel &pixel =
                    surfaceLayers->sceneLinearColor[
                        static_cast<std::size_t>(y) * outputWidth + x];
                if (pixel.alpha <= 0.0f) {
                    row[x] = qRgba(0, 0, 0, 0);
                    continue;
                }
                const QVector3D display = workbenchSceneLinearToDisplaySrgb(
                    QVector3D(pixel.red / pixel.alpha,
                              pixel.green / pixel.alpha,
                              pixel.blue / pixel.alpha),
                    settings.colorTransform, settings.colorExposure,
                    settings.colorGamma, settings.colorLutData,
                    settings.colorLutMetadata);
                row[x] = qPremultiply(qRgba(
                    std::clamp(qRound(display.x() * 255.0f), 0, 255),
                    std::clamp(qRound(display.y() * 255.0f), 0, 255),
                    std::clamp(qRound(display.z() * 255.0f), 0, 255),
                    std::clamp(qRound(pixel.alpha * 255.0f), 0, 255)));
            }
        }
        *colorImage = resolvedImage.convertToFormat(colorImage->format());
        return true;
    };
    // This CPU path keeps multiple per-pixel depth/normal/object buffers, so
    // pixelCountWide is bounded above before allocating them.
    QVector<RasterSample> samples(width * height);
    QVector<RasterSample> inFrontSamples(width * height);
    const bool rasterWorkbenchOutlineEnabled = settings.outline &&
        !hasDeferredSceneLinearOutline &&
        !(surfaceLayers != nullptr &&
          surfaceLayers->outlineAppliedBeforeDisplay);
    QVector<RasterOutlineSample> outlineSamples;
    QVector<RasterOutlineSample> inFrontOutlineSamples;
    QVector<RasterOutlineSample> selectedOutlineSamples;
    QVector<RasterOutlineSample> inFrontSelectedOutlineSamples;
    QSet<quint64> xraySelectedMeshOutlineIds;
    QVector<float> outlineOpaqueDepth;
    QVector<float> inFrontOutlineOpaqueDepth;
    if (rasterWorkbenchOutlineEnabled) {
        outlineSamples.resize(width * height);
        inFrontOutlineSamples.resize(width * height);
        outlineOpaqueDepth.fill(std::numeric_limits<float>::infinity(),
                                width * height);
        inFrontOutlineOpaqueDepth.fill(std::numeric_limits<float>::infinity(),
                                       width * height);
    }
    if (selectedOutlineEnabled) {
        selectedOutlineSamples.resize(width * height);
        inFrontSelectedOutlineSamples.resize(width * height);
    }
    QVector<RasterSample> opaqueSamples;
    QVector<RasterSample> inFrontOpaqueSamples;
    if (cavityEnabled || shadowEnabled) {
        opaqueSamples.resize(width * height);
        inFrontOpaqueSamples.resize(width * height);
    }
    QVector<ShadowTriangle> shadowTriangles;
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
        const auto flatMeshViewedEdgeOn = [&]() {
            if (perspective ||
                object.shape.geometryType != GeometryType::PolygonMesh ||
                !validatePolygonMesh(object.shape.polygonMesh)) {
                return false;
            }
            const PolygonMesh3D &mesh = object.shape.polygonMesh;
            if (mesh.vertices.isEmpty()) return false;
            qreal minimum[3] = {
                std::numeric_limits<qreal>::infinity(),
                std::numeric_limits<qreal>::infinity(),
                std::numeric_limits<qreal>::infinity()};
            qreal maximum[3] = {
                -std::numeric_limits<qreal>::infinity(),
                -std::numeric_limits<qreal>::infinity(),
                -std::numeric_limits<qreal>::infinity()};
            for (const Point3D &vertex : mesh.vertices) {
                const qreal coordinates[3] = {vertex.x, vertex.y, vertex.z};
                for (int axis = 0; axis < 3; ++axis) {
                    if (!std::isfinite(coordinates[axis])) return false;
                    minimum[axis] = std::min(minimum[axis], coordinates[axis]);
                    maximum[axis] = std::max(maximum[axis], coordinates[axis]);
                }
            }
            qreal largestDimension = 0.0;
            qreal dimensions[3];
            for (int axis = 0; axis < 3; ++axis) {
                dimensions[axis] = maximum[axis] - minimum[axis];
                largestDimension = std::max(largestDimension, dimensions[axis]);
            }
            if (largestDimension <= 0.0) return false;
            const qreal flatTolerance = largestDimension * 1.0e-6;
            int flatAxis = -1;
            for (int axis = 0; axis < 3; ++axis) {
                if (dimensions[axis] <= flatTolerance) {
                    if (flatAxis != -1) return false;
                    flatAxis = axis;
                }
            }
            if (flatAxis == -1) return false;

            const auto &m = object.meshObjectTransform;
            const QVector3D axisX{float(m[0]), float(m[4]), float(m[8])};
            const QVector3D axisY{float(m[1]), float(m[5]), float(m[9])};
            const QVector3D axisZ{float(m[2]), float(m[6]), float(m[10])};
            QVector3D flatNormal;
            if (flatAxis == 0) {
                flatNormal = QVector3D::crossProduct(axisY, axisZ);
            } else if (flatAxis == 1) {
                flatNormal = QVector3D::crossProduct(axisZ, axisX);
            } else {
                flatNormal = QVector3D::crossProduct(axisX, axisY);
            }
            if (flatNormal.lengthSquared() <= 1.0e-16f) return false;
            flatNormal.normalize();
            return std::abs(QVector3D::dotProduct(
                       flatNormal, viewFacingVector)) < 1.0e-3f;
        };
        const bool rasterizeSelectedMeshEdges =
            selectedOutlineEnabled && object.selected &&
            object.shape.geometryType == GeometryType::PolygonMesh &&
            (xrayEnabled || flatMeshViewedEdgeOn());
        const bool selectedOutlineSupported =
            object.blenderSourceObjectType != QLatin1String("SURFACE");
        const bool needsSelectedOutlineGeometry =
            selectedOutlineEnabled && object.selected &&
            selectedOutlineSupported;
        const bool perFaceMaterialAlpha =
            (object.shape.geometryType == GeometryType::PolygonMesh ||
             object.shape.geometryType == GeometryType::NurbsSolid) &&
            (settings.colorMode == ViewportColorMode::Material ||
             settings.colorMode == ViewportColorMode::Attribute ||
             settings.colorMode == ViewportColorMode::Texture);
        if (!perFaceMaterialAlpha &&
            objectSourceAlpha(object, settings.colorMode) <= 0.0f &&
            !shadowEnabled && !rasterizeSelectedMeshEdges &&
            !needsSelectedOutlineGeometry) {
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
                float triangleSourceAlpha =
                    settings.colorMode == ViewportColorMode::Attribute
                        ? 1.0f
                        : objectAlpha;
                if (settings.colorMode == ViewportColorMode::Material ||
                    settings.colorMode == ViewportColorMode::Texture) {
                    float faceAlpha = object.workbenchMaterial.alpha;
                    if (faceSlot >= 0 && faceSlot < object.meshMaterials.size()) {
                        faceAlpha = object.meshMaterials[faceSlot].alpha;
                    } else if (faceSlot >= 0 && faceSlot < mesh.materialSlots.size()) {
                        faceAlpha = mesh.materialSlots[faceSlot].alpha;
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
                     polygonMeshFaceTriangles(mesh, faceIndex, points)) {
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
            if (rasterizeSelectedMeshEdges) {
                QHash<quint64, QVector<QVector3D>> edgeFaceNormals;
                const quint64 objectId = object.objectId.value() |
                                         0x80000000ull;
                const QVector3D offset(
                    static_cast<float>(object.preparedGeometryOffset.x),
                    static_cast<float>(object.preparedGeometryOffset.y),
                    static_cast<float>(object.preparedGeometryOffset.z));
                const auto rasterizeEdge = [&](int first, int second) {
                    if (first < 0 || second < 0 ||
                        first >= mesh.vertices.size() ||
                        second >= mesh.vertices.size() || first == second) {
                        return;
                    }
                    QVector3D worldFirst = worldPoint(mesh.vertices[first]) + offset;
                    QVector3D worldSecond = worldPoint(mesh.vertices[second]) + offset;
                    QPointF screenFirst;
                    QPointF screenSecond;
                    if (!transform.worldPointToScreenUnclipped(
                            Point3D(worldFirst.x(), worldFirst.y(), worldFirst.z()),
                            viewportSize, &screenFirst) ||
                        !transform.worldPointToScreenUnclipped(
                            Point3D(worldSecond.x(), worldSecond.y(), worldSecond.z()),
                            viewportSize, &screenSecond)) {
                        return;
                    }
                    QPointF a = screenFirst * dpr;
                    QPointF b = screenSecond * dpr;
                    const QPointF originalA = a;
                    const QPointF originalB = b;
                    const qreal dx = b.x() - a.x();
                    const qreal dy = b.y() - a.y();
                    qreal t0 = 0.0;
                    qreal t1 = 1.0;
                    const auto clip = [&](qreal p, qreal q) {
                        if (std::abs(p) <= 1.0e-12) return q >= 0.0;
                        const qreal t = q / p;
                        if (p < 0.0) {
                            if (t > t1) return false;
                            t0 = std::max(t0, t);
                        } else {
                            if (t < t0) return false;
                            t1 = std::min(t1, t);
                        }
                        return true;
                    };
                    if (!clip(-dx, a.x()) ||
                        !clip(dx, width - 1.0 - a.x()) ||
                        !clip(-dy, a.y()) ||
                        !clip(dy, height - 1.0 - a.y()) || t1 < t0) {
                        return;
                    }
                    a = originalA + (originalB - originalA) * t0;
                    b = originalA + (originalB - originalA) * t1;
                    const float depthA = rasterWorkbenchCameraDepth(
                        worldFirst, camera, towardCamera);
                    const float depthB = rasterWorkbenchCameraDepth(
                        worldSecond, camera, towardCamera);
                    const qreal length = std::max(std::abs(b.x() - a.x()),
                                                  std::abs(b.y() - a.y()));
                    const int steps = std::clamp(
                        qCeil(length * 2.0), 1,
                        std::max(width, height) * 4);
                    auto &domainSamples = object.showInFront
                        ? inFrontSelectedOutlineSamples
                        : selectedOutlineSamples;
                    for (int step = 0; step <= steps; ++step) {
                        const qreal t = static_cast<qreal>(step) / steps;
                        const int x = qFloor(a.x() + (b.x() - a.x()) * t);
                        const int y = qFloor(a.y() + (b.y() - a.y()) * t);
                        if (x < 0 || x >= width || y < 0 || y >= height)
                            continue;
                        const float sourceT = t0 + (t1 - t0) * t;
                        const float depth = depthA + (depthB - depthA) * sourceT;
                        RasterOutlineSample &sample =
                            domainSamples[y * width + x];
                        if (sample.objectId == 0 || depth <= sample.depth) {
                            sample.depth = depth;
                            sample.objectId = objectId;
                        }
                    }
                };
                for (const QVector<int> &face : mesh.faces) {
                    if (face.size() < 2) continue;
                    QVector<QVector3D> worldFace;
                    worldFace.reserve(face.size());
                    for (int index : face) {
                        if (index < 0 || index >= mesh.vertices.size()) {
                            worldFace.clear();
                            break;
                        }
                        worldFace.append(worldPoint(mesh.vertices[index]));
                    }
                    if (worldFace.size() < 3) continue;
                    QVector3D faceNormal;
                    for (int corner = 0; corner < worldFace.size(); ++corner) {
                        const QVector3D &a = worldFace[corner];
                        const QVector3D &b = worldFace[(corner + 1) % worldFace.size()];
                        faceNormal += QVector3D(
                            (a.y() - b.y()) * (a.z() + b.z()),
                            (a.z() - b.z()) * (a.x() + b.x()),
                            (a.x() - b.x()) * (a.y() + b.y()));
                    }
                    if (faceNormal.lengthSquared() <= 1.0e-16f) continue;
                    faceNormal.normalize();
                    for (int corner = 0; corner < face.size(); ++corner) {
                        const int first = face[corner];
                        const int second = face[(corner + 1) % face.size()];
                        if (first < 0 || second < 0 || first == second ||
                            first >= mesh.vertices.size() ||
                            second >= mesh.vertices.size()) {
                            continue;
                        }
                        const quint32 low = static_cast<quint32>(
                            std::min(first, second));
                        const quint32 high = static_cast<quint32>(
                            std::max(first, second));
                        edgeFaceNormals[(quint64(low) << 32) | high]
                            .append(faceNormal);
                    }
                }
                for (auto edge = edgeFaceNormals.cbegin();
                     edge != edgeFaceNormals.cend(); ++edge) {
                    const QVector<QVector3D> &normals = edge.value();
                    bool draw = normals.size() == 1;
                    if (!draw && normals.size() > 1) {
                        const int first = static_cast<int>(edge.key() >> 32);
                        const int second = static_cast<int>(edge.key() & 0xffffffffu);
                        const QVector3D midpoint =
                            (worldPoint(mesh.vertices[first]) +
                             worldPoint(mesh.vertices[second])) * 0.5f + offset;
                        QVector3D cameraDirection = perspective
                            ? QVector3D(float(camera.x), float(camera.y),
                                        float(camera.z)) - midpoint
                            : QVector3D(float(towardCamera.x),
                                        float(towardCamera.y),
                                        float(towardCamera.z));
                        if (cameraDirection.lengthSquared() > 1.0e-16f) {
                            cameraDirection.normalize();
                            bool frontFacing = false;
                            bool backFacing = false;
                            bool edgeOn = false;
                            for (const QVector3D &normal : normals) {
                                const float facing = QVector3D::dotProduct(
                                    normal, cameraDirection);
                                frontFacing |= facing > 1.0e-5f;
                                backFacing |= facing < -1.0e-5f;
                                edgeOn |= std::abs(facing) <= 1.0e-5f;
                            }
                            draw = (frontFacing && backFacing) ||
                                   (edgeOn && (frontFacing || backFacing));
                        }
                    }
                    if (draw) {
                        rasterizeEdge(static_cast<int>(edge.key() >> 32),
                                      static_cast<int>(edge.key() & 0xffffffffu));
                    }
                }
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
        const bool hasFaceTexture = std::any_of(
            object.nurbsSolidFaceMaterials.cbegin(),
            object.nurbsSolidFaceMaterials.cend(),
            [](const WorkbenchMaterialData &material) {
                return !material.textureImageTiles.isEmpty() ||
                       !material.textureImagePath.isEmpty() ||
                       !material.textureImagePackedDataBase64.isEmpty() ||
                       !material.textureImageSceneLinearFloatDataBase64.isEmpty();
            });
        if (hasFaceTexture) {
            textureAlphaTriangles.resize(vertices.size() / 3);
        }
        if (settings.colorMode == ViewportColorMode::Texture &&
            (object.shape.geometryType == GeometryType::NurbsSurface ||
             object.shape.geometryType == GeometryType::NurbsSolid) &&
            geometry->surfaceTextureCoordinates.size() == vertices.size() &&
            ((!object.workbenchMaterial.textureImageTiles.isEmpty() ||
             !object.workbenchMaterial.textureImagePath.isEmpty() ||
             !object.workbenchMaterial.textureImagePackedDataBase64.isEmpty() ||
             !object.workbenchMaterial.textureImageSceneLinearFloatDataBase64.isEmpty() ||
             hasFaceTexture) &&
            (!ignoresTextureAlpha(
                 object.workbenchMaterial.textureImageAlphaMode) ||
             hasFaceTexture))) {
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
        if (object.shape.geometryType == GeometryType::NurbsSolid &&
            geometry->surfaceFaceRanges.size() > 0 &&
            (settings.colorMode == ViewportColorMode::Material ||
             settings.colorMode == ViewportColorMode::Attribute ||
             settings.colorMode == ViewportColorMode::Texture)) {
            QVector<QString> faceIds;
            if (object.shape.nurbsSolid.boundaryFaces.isEmpty()) {
                faceIds = {object.shape.nurbsSolid.baseCapFaceId,
                           object.shape.nurbsSolid.displacedCapFaceId};
                for (const QString &id : object.shape.nurbsSolid.wallFaceIds)
                    faceIds.append(id);
            }
            for (int faceIndex = 0;
                 faceIndex < geometry->surfaceFaceRanges.size(); ++faceIndex) {
                const auto faceIt = faceIndex < faceIds.size()
                    ? object.nurbsSolidFaceMaterials.constFind(faceIds[faceIndex])
                    : object.nurbsSolidFaceMaterials.cend();
                if (faceIt == object.nurbsSolidFaceMaterials.cend()) continue;
                const auto &faceRange = geometry->surfaceFaceRanges[faceIndex];
                const float alpha = saturate(float(faceIt->alpha));
                for (int triangle = faceRange.firstVertex / 3;
                     triangle < (faceRange.firstVertex + faceRange.vertexCount) / 3 &&
                     triangle < triangleAlphas.size(); ++triangle) {
                    triangleAlphas[triangle] = alpha;
                }
                for (int triangle = faceRange.firstVertex / 3;
                     triangle < (faceRange.firstVertex + faceRange.vertexCount) / 3 &&
                     triangle < textureAlphaTriangles.size(); ++triangle) {
                    textureAlphaTriangles[triangle] = TextureAlphaTriangle{};
                }
                if (settings.colorMode != ViewportColorMode::Texture ||
                    ignoresTextureAlpha(faceIt->textureImageAlphaMode)) {
                    continue;
                }
                const WorkbenchMaterialData &material = faceIt.value();
                const bool hasUdim = !material.textureImageTiles.isEmpty();
                const QHash<int, QImage> udimImages = hasUdim
                    ? loadTextureAlphaTiles(material) : QHash<int, QImage>{};
                const QImage alphaImage = hasUdim ? QImage{}
                    : loadTextureAlphaImage(material);
                const auto floatAlphaImage = hasUdim
                    ? TextureAlphaTriangle::FloatImage{}
                    : loadFloatAlphaPayload(
                        material.textureImageSceneLinearFloatDataBase64,
                        material.textureImageSceneLinearFloatWidth,
                        material.textureImageSceneLinearFloatHeight);
                const auto floatAlphaUdimImages = hasUdim
                    ? loadFloatTextureAlphaTiles(material)
                    : QHash<int, TextureAlphaTriangle::FloatImage>{};
                if ((hasUdim && udimImages.isEmpty() &&
                     floatAlphaUdimImages.isEmpty()) && alphaImage.isNull() &&
                    floatAlphaImage.width <= 0) continue;
                for (int triangle = faceRange.firstVertex / 3;
                     triangle < (faceRange.firstVertex + faceRange.vertexCount) / 3 &&
                     triangle < textureAlphaTriangles.size(); ++triangle) {
                    TextureAlphaTriangle &alphaTriangle =
                        textureAlphaTriangles[triangle];
                    alphaTriangle.udim = hasUdim;
                    alphaTriangle.udimImages = udimImages;
                    alphaTriangle.floatUdimImages = floatAlphaUdimImages;
                    alphaTriangle.image = alphaImage;
                    alphaTriangle.floatImage = floatAlphaImage;
                    alphaTriangle.interpolation = material.textureImageInterpolation;
                    alphaTriangle.extension = material.textureImageExtension;
                    alphaTriangle.cutout = true;
                    for (int corner = 0; corner < 3; ++corner) {
                        const QPointF uv = geometry->surfaceTextureCoordinates[
                            triangle * 3 + corner];
                        alphaTriangle.uvs[corner] = hasUdim
                            ? uv : QPointF(uv.x(), 1.0 - uv.y());
                    }
                }
            }
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
            bool finiteWorldTriangle = true;
            for (int corner = 0; corner < 3; ++corner) {
                world[corner] = vertices[triangle + corner] + QVector3D(
                    object.preparedGeometryOffset.x,
                    object.preparedGeometryOffset.y,
                    object.preparedGeometryOffset.z);
                finiteWorldTriangle = finiteWorldTriangle &&
                    std::isfinite(world[corner].x()) &&
                    std::isfinite(world[corner].y()) &&
                    std::isfinite(world[corner].z());
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
            if (!finiteWorldTriangle) continue;
            const float triangleSourceAlpha = triangleIndex < triangleAlphas.size()
                ? triangleAlphas[triangleIndex] : objectAlpha;
            if (shadowEnabled) {
                ShadowTriangle caster;
                for (int corner = 0; corner < 3; ++corner) {
                    caster.vertices[corner] = world[corner];
                }
                shadowTriangles.append(caster);
            }
            // Shadow casters are submitted from world geometry independently
            // of whether their vertices can be projected by the camera.
            if (!valid) continue;
            // Zero-alpha material geometry is still a Blender shadow caster,
            // but it must not become a visible receiver or depth sample here.
            if (triangleSourceAlpha <= 0.0f &&
                !rasterWorkbenchOutlineEnabled &&
                !needsSelectedOutlineGeometry) {
                continue;
            }
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
            if (rasterizeSelectedMeshEdges) {
                xraySelectedMeshOutlineIds.insert(id);
            }
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
                        // Camera depth grows farther into the scene, so keep
                        // the smallest depth when surfaces overlap. The GPU
                        // opaque prepass uses LEQUAL, so later coplanar
                        // fragments replace earlier samples.
                        if (target.objectId != 0 && z > target.depth) return;
                        target.depth = z;
                        target.position = interpolatedPosition;
                        target.normal = interpolatedNormal;
                        target.objectId = id;
                        target.sourceAlpha = triangleSourceAlpha;
                    };
                    const float sourceAlpha = triangleSourceAlpha;
                    if (needsSelectedOutlineGeometry &&
                        !rasterizeSelectedMeshEdges) {
                        QVector<RasterOutlineSample> &domainSelectedOutline =
                            object.showInFront ? inFrontSelectedOutlineSamples
                                               : selectedOutlineSamples;
                        RasterOutlineSample &selectedSample =
                            domainSelectedOutline[y * width + x];
                        if (selectedSample.objectId == 0 ||
                            z <= selectedSample.depth) {
                            selectedSample.depth = z;
                            selectedSample.objectId = id;
                        }
                    }
                    if (rasterWorkbenchOutlineEnabled &&
                        isOpaqueWorkbenchAlpha(sourceAlpha)) {
                        QVector<float> &domainOpaqueDepth = object.showInFront
                            ? inFrontOutlineOpaqueDepth : outlineOpaqueDepth;
                        float &opaqueDepth = domainOpaqueDepth[y * width + x];
                        opaqueDepth = std::min(opaqueDepth, z);
                    }
                    if (rasterWorkbenchOutlineEnabled) {
                        const QVector<float> &domainOpaqueDepth =
                            object.showInFront ? inFrontOutlineOpaqueDepth
                                               : outlineOpaqueDepth;
                        const float opaqueDepth =
                            domainOpaqueDepth[y * width + x];
                        if (std::isfinite(opaqueDepth) &&
                            z > opaqueDepth + 1.0e-5f) {
                            continue;
                        }
                        QVector<RasterOutlineSample> &domainOutlineSamples =
                            object.showInFront ? inFrontOutlineSamples
                                               : outlineSamples;
                        RasterOutlineSample &outlineSample =
                            domainOutlineSamples[y * width + x];
                        if (outlineSample.objectId == 0 ||
                            z <= outlineSample.depth) {
                            outlineSample.depth = z;
                            outlineSample.objectId = id;
                        }
                    }
                    // Blender's transparent-depth pass retains zero-alpha
                    // material surfaces for effects and overlay occlusion,
                    // while the color pass leaves them invisible.
                    writeNearest(sample);
                    if (sourceAlpha <= 0.0f) continue;
                    if ((cavityEnabled || shadowEnabled) &&
                        isOpaqueWorkbenchAlpha(sourceAlpha)) {
                        QVector<RasterSample> &domainOpaqueSamples =
                            object.showInFront ? inFrontOpaqueSamples
                                               : opaqueSamples;
                        writeNearest(domainOpaqueSamples[y * width + x]);
                    }
                }
            }
        }
    }

    // Preserve regular scene depth for the DOF visibility test before
    // depth_in_front replaces regular samples in the combined scene buffer.
    QVector<float> regularDofDepth;
    if (settings.depthOfField) {
        regularDofDepth.resize(samples.size());
        for (int index = 0; index < samples.size(); ++index) {
            regularDofDepth[index] = samples[index].objectId != 0
                ? samples[index].depth
                : -std::numeric_limits<float>::infinity();
        }
    }

    const QVector<RasterSample> regularOpaqueSamples =
        shadowEnabled ? opaqueSamples : QVector<RasterSample>{};

    // Workbench keeps depth_in_front separate through its effects inputs.
    // Select the nearest sample inside each domain first, then let covered
    // in-front samples replace regular scene samples regardless of geometric
    // depth. Cavity and shadows use opaque receivers, so a transparent
    // in-front sample does not remove the regular opaque receiver underneath.
    for (int index = 0; index < samples.size(); ++index) {
        if (inFrontSamples[index].objectId != 0) {
            samples[index] = inFrontSamples[index];
        }
        if (rasterWorkbenchOutlineEnabled &&
            inFrontOutlineSamples[index].objectId != 0) {
            outlineSamples[index] = inFrontOutlineSamples[index];
        }
        if (selectedOutlineEnabled &&
            inFrontSelectedOutlineSamples[index].objectId != 0) {
            selectedOutlineSamples[index] =
                inFrontSelectedOutlineSamples[index];
        }
        if ((cavityEnabled || shadowEnabled) &&
            inFrontOpaqueSamples[index].objectId != 0) {
            opaqueSamples[index] = inFrontOpaqueSamples[index];
        }
    }

    // The software fallback uses a single orthographic light-depth buffer.
    // Transparent geometry can cast a binary shadow, while the receiver pass
    // retains separate opaque and nearest-visible transparent samples.
    QVector<float> lightDepth;
    const QSize lightMapSize = rasterWorkbenchShadowMapSize(
        QSize(width, height));
    const int lightMapWidth = lightMapSize.width();
    const int lightMapHeight = lightMapSize.height();
    QVector<QVector2D> regularReceiverLightCoordinates;
    QVector<float> regularReceiverLightDepth;
    QVector<QVector2D> inFrontReceiverLightCoordinates;
    QVector<float> inFrontReceiverLightDepth;
    QVector<QVector2D> transparentReceiverLightCoordinates;
    QVector<float> transparentReceiverLightDepth;
    float softwareDepthBias = 0.0f;
    QVector3D shadowLightDirection;
    QVector3D shadowLightRight;
    QVector3D shadowLightUp;
    float shadowLightMinX = 0.0f;
    float shadowLightMaxX = 1.0f;
    float shadowLightMinY = 0.0f;
    float shadowLightMaxY = 1.0f;
    if (shadowEnabled && !shadowTriangles.isEmpty()) {
        shadowLightDirection = QVector3D(-settings.shadowDirection.x(),
                                         settings.shadowDirection.z(),
                                         -settings.shadowDirection.y());
        if (shadowLightDirection.lengthSquared() < 1.0e-10f)
            shadowLightDirection = QVector3D(-0.57735026f, 0.57735026f,
                                             -0.57735026f);
        shadowLightDirection.normalize();
        QVector3D helper(0.0f, 1.0f, 0.0f);
        if (std::abs(QVector3D::dotProduct(helper, shadowLightDirection)) > 0.95f)
            helper = QVector3D(1.0f, 0.0f, 0.0f);
        shadowLightRight = normalized(
            QVector3D::crossProduct(shadowLightDirection, helper));
        shadowLightUp = normalized(
            QVector3D::crossProduct(shadowLightRight, shadowLightDirection));
        float casterMinX = std::numeric_limits<float>::max();
        float casterMaxX = std::numeric_limits<float>::lowest();
        float casterMinY = casterMinX;
        float casterMaxY = casterMaxX;
        float minLightDepth = std::numeric_limits<float>::max();
        float maxLightDepth = std::numeric_limits<float>::lowest();
        for (const ShadowTriangle &triangle : shadowTriangles) {
            for (const QVector3D &point : triangle.vertices) {
                const float x = QVector3D::dotProduct(point, shadowLightRight);
                const float y = QVector3D::dotProduct(point, shadowLightUp);
                const float z = QVector3D::dotProduct(point, shadowLightDirection);
                casterMinX = std::min(casterMinX, x);
                casterMaxX = std::max(casterMaxX, x);
                casterMinY = std::min(casterMinY, y);
                casterMaxY = std::max(casterMaxY, y);
                minLightDepth = std::min(minLightDepth, z);
                maxLightDepth = std::max(maxLightDepth, z);
            }
        }
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        bool hasReceiverBounds = false;
        const auto includeReceiverPoint = [&](const QVector3D &point) {
            const float x = QVector3D::dotProduct(point, shadowLightRight);
            const float y = QVector3D::dotProduct(point, shadowLightUp);
            if (!std::isfinite(x) || !std::isfinite(y)) return;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            hasReceiverBounds = true;
        };
        const qreal receiverPixelQuarter = 0.25 / std::max<qreal>(dpr, 1.0e-6);
        const std::array<QPointF, 4> receiverScreenOffsets{{
            QPointF(-receiverPixelQuarter, -receiverPixelQuarter),
            QPointF(receiverPixelQuarter, -receiverPixelQuarter),
            QPointF(-receiverPixelQuarter, receiverPixelQuarter),
            QPointF(receiverPixelQuarter, receiverPixelQuarter)}};
        const auto includeReceiverBounds = [&](
            const QVector<RasterSample> &receiverSamples) {
            for (int index = 0; index < receiverSamples.size(); ++index) {
                const RasterSample &receiver = receiverSamples[index];
                if (receiver.objectId == 0) continue;
                includeReceiverPoint(receiver.position);
                const Point3D receiverOrigin{receiver.position.x(),
                                             receiver.position.y(),
                                             receiver.position.z()};
                const Point3D receiverNormal{receiver.normal.x(),
                                             receiver.normal.y(),
                                             receiver.normal.z()};
                const WorkPlaneFrame receiverPlane =
                    makeWorkPlaneFrameFromNormal(receiverOrigin,
                                                 receiverNormal);
                const QPointF screenPixelCenter(
                    (index % width + 0.5) / dpr,
                    (index / width + 0.5) / dpr);
                for (const QPointF &screenOffset : receiverScreenOffsets) {
                    QPointF receiverPlanePoint;
                    if (!transform.screenToWorkPlaneUnclipped(
                            screenPixelCenter + screenOffset, viewportSize,
                            receiverPlane, &receiverPlanePoint)) {
                        continue;
                    }
                    const Point3D worldPoint = workPlaneFramePointToWorld(
                        receiverPlanePoint, receiverPlane);
                    includeReceiverPoint(QVector3D(
                        float(worldPoint.x), float(worldPoint.y),
                        float(worldPoint.z)));
                }
            }
        };
        includeReceiverBounds(regularOpaqueSamples);
        includeReceiverBounds(inFrontOpaqueSamples);
        if (!hasReceiverBounds) {
            minX = casterMinX;
            maxX = casterMaxX;
            minY = casterMinY;
            maxY = casterMaxY;
        }
        softwareDepthBias = rasterWorkbenchShadowDepthBias(
            minLightDepth, maxLightDepth);
        const float extentX = std::max(maxX - minX, 1.0e-4f);
        const float extentY = std::max(maxY - minY, 1.0e-4f);
        const float padX = extentX * 0.02f + 1.0e-4f;
        const float padY = extentY * 0.02f + 1.0e-4f;
        shadowLightMinX = minX - padX;
        shadowLightMaxX = maxX + padX;
        shadowLightMinY = minY - padY;
        shadowLightMaxY = maxY + padY;
        const qsizetype lightMapPixelCount =
            qsizetype(lightMapWidth) * lightMapHeight;
        lightDepth.fill(std::numeric_limits<float>::infinity(),
                        lightMapPixelCount);
        const auto lightScreen = [&](const QVector3D &point) {
            return QPointF((QVector3D::dotProduct(point, shadowLightRight) -
                            shadowLightMinX) /
                               (shadowLightMaxX - shadowLightMinX) *
                               lightMapWidth,
                           (shadowLightMaxY -
                            QVector3D::dotProduct(point, shadowLightUp)) /
                               (shadowLightMaxY - shadowLightMinY) *
                               lightMapHeight);
        };
        for (const ShadowTriangle &triangle : shadowTriangles) {
            const QPointF p[3] = {lightScreen(triangle.vertices[0]),
                                  lightScreen(triangle.vertices[1]),
                                  lightScreen(triangle.vertices[2])};
            const float z[3] = {QVector3D::dotProduct(triangle.vertices[0], shadowLightDirection),
                                QVector3D::dotProduct(triangle.vertices[1], shadowLightDirection),
                                QVector3D::dotProduct(triangle.vertices[2], shadowLightDirection)};
            const qreal area = (p[1].x() - p[0].x()) * (p[2].y() - p[0].y()) -
                               (p[1].y() - p[0].y()) * (p[2].x() - p[0].x());
            if (std::abs(area) < 1.0e-10) continue;
            const qreal triangleMinX = std::min(
                {p[0].x(), p[1].x(), p[2].x()});
            const qreal triangleMaxX = std::max(
                {p[0].x(), p[1].x(), p[2].x()});
            const qreal triangleMinY = std::min(
                {p[0].y(), p[1].y(), p[2].y()});
            const qreal triangleMaxY = std::max(
                {p[0].y(), p[1].y(), p[2].y()});
            if (triangleMaxX < 0.0 || triangleMinX >= lightMapWidth ||
                triangleMaxY < 0.0 || triangleMinY >= lightMapHeight) {
                continue;
            }
            const int x0 = static_cast<int>(std::floor(
                std::max<qreal>(0.0, triangleMinX)));
            const int x1 = static_cast<int>(std::ceil(
                std::min<qreal>(lightMapWidth - 1.0, triangleMaxX)));
            const int y0 = static_cast<int>(std::floor(
                std::max<qreal>(0.0, triangleMinY)));
            const int y1 = static_cast<int>(std::ceil(
                std::min<qreal>(lightMapHeight - 1.0, triangleMaxY)));
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                const qreal px = x + 0.5, py = y + 0.5;
                const qreal a = ((p[1].x()-px)*(p[2].y()-py)-(p[1].y()-py)*(p[2].x()-px))/area;
                const qreal b = ((p[2].x()-px)*(p[0].y()-py)-(p[2].y()-py)*(p[0].x()-px))/area;
                const qreal c = 1.0 - a - b;
                if (a < -1e-6 || b < -1e-6 || c < -1e-6) continue;
                const qsizetype mapIndex =
                    qsizetype(y) * lightMapWidth + x;
                float &depth = lightDepth[mapIndex];
                depth = std::min(depth, float(a*z[0] + b*z[1] + c*z[2]));
            }
        }
        regularReceiverLightDepth.resize(width * height);
        regularReceiverLightCoordinates.resize(width * height);
        inFrontReceiverLightDepth.resize(width * height);
        inFrontReceiverLightCoordinates.resize(width * height);
        transparentReceiverLightDepth.resize(width * height);
        transparentReceiverLightCoordinates.resize(width * height);
        const auto prepareReceiverLightSamples = [&](
            const QVector<RasterSample> &receiverSamples,
            QVector<float> *receiverDepths,
            QVector<QVector2D> *receiverCoordinates) {
            for (int i = 0; i < receiverSamples.size(); ++i) {
                if (receiverSamples[i].objectId == 0) continue;
                (*receiverDepths)[i] = QVector3D::dotProduct(
                    receiverSamples[i].position, shadowLightDirection);
                const QPointF p = lightScreen(receiverSamples[i].position);
                (*receiverCoordinates)[i] = QVector2D(p.x(), p.y());
            }
        };
        prepareReceiverLightSamples(regularOpaqueSamples,
                                    &regularReceiverLightDepth,
                                    &regularReceiverLightCoordinates);
        prepareReceiverLightSamples(inFrontOpaqueSamples,
                                    &inFrontReceiverLightDepth,
                                    &inFrontReceiverLightCoordinates);
        for (int i = 0; i < opaqueSamples.size(); ++i) {
            const RasterSample &transparent = samples[i];
            if (transparent.objectId != 0 && transparent.sourceAlpha > 0.0f &&
                !isOpaqueWorkbenchAlpha(transparent.sourceAlpha)) {
                transparentReceiverLightDepth[i] =
                    QVector3D::dotProduct(transparent.position,
                                          shadowLightDirection);
                const QPointF p = lightScreen(transparent.position);
                transparentReceiverLightCoordinates[i] =
                    QVector2D(p.x(), p.y());
            }
        }
    }

    const auto applyOutline = [&](bool drawWorkbenchOutline,
                                  bool drawSelectionOutline) {
        if (!drawWorkbenchOutline && !drawSelectionOutline) return;
        QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage result = source.copy();
        const QColor outline = settings.outlineColor;
        const QColor selectionOutline = settings.themeActiveObjectColor;
        // Blender offsets its four object-ID taps by UI scale in normalized
        // viewport coordinates. The QPainter ID buffer is allocated at the
        // device-pixel resolution, so this is the equivalent texel distance.
        // Integer ID sampling resolves the fractional scale to the nearest
        // texel, as the GPU texture lookup does.
        const int outlineSampleOffset = workbenchEffectUiScale(dpr);
        const float outlineExposureScale = std::exp2(
            static_cast<float>(settings.colorExposure));
        const bool standardLinearOutlineBlend = drawWorkbenchOutline &&
            settings.colorTransform == 1 &&
            settings.colorGamma > workbenchGammaEpsilon &&
            std::isfinite(settings.colorGamma) &&
            std::isfinite(outlineExposureScale) && outlineExposureScale > 0.0f;
        const QVector3D outlineSceneLinear = workbenchSrgbToSceneLinear(
            QVector3D(outline.redF(), outline.greenF(), outline.blueF()));
        const auto blendStandardOutline = [&](QRgb pixel, float coverage) {
            const float sourceAlpha = static_cast<float>(outline.alphaF()) *
                                      coverage;
            const float destinationAlpha = qAlpha(pixel) / 255.0f;
            const float outputAlpha = sourceAlpha + destinationAlpha *
                                                       (1.0f - sourceAlpha);
            if (sourceAlpha <= 0.0f || outputAlpha <= 0.0f) return pixel;
            const QVector3D encodedDestination(
                std::pow(qRed(pixel) / 255.0f,
                         static_cast<float>(settings.colorGamma)),
                std::pow(qGreen(pixel) / 255.0f,
                         static_cast<float>(settings.colorGamma)),
                std::pow(qBlue(pixel) / 255.0f,
                         static_cast<float>(settings.colorGamma)));
            const QVector3D destinationLinear =
                workbenchSrgbToSceneLinear(encodedDestination) /
                outlineExposureScale;
            const QVector3D outputLinear =
                (outlineSceneLinear * sourceAlpha +
                 destinationLinear *
                     (destinationAlpha * (1.0f - sourceAlpha))) /
                outputAlpha;
            const QVector3D display = workbenchSceneLinearToDisplaySrgb(
                outputLinear, settings.colorTransform,
                settings.colorExposure, settings.colorGamma,
                settings.colorLutData, settings.colorLutMetadata);
            return qRgba(
                std::clamp(qRound(display.x() * 255.0f), 0, 255),
                std::clamp(qRound(display.y() * 255.0f), 0, 255),
                std::clamp(qRound(display.z() * 255.0f), 0, 255),
                std::clamp(qRound(outputAlpha * 255.0f), 0, 255));
        };
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
                const quint64 objectId = drawWorkbenchOutline
                    ? outlineSamples[sampleIndex].objectId
                    : samples[sampleIndex].objectId;
                const quint64 selectionObjectId = drawSelectionOutline
                    ? selectedOutlineSamples[sampleIndex].objectId
                    : 0;
                int differentNeighbors = 0;
                float selectedOutlineCoverage = 0.0f;
                float selectedOutlineHardOpacity = 0.0f;
                float selectedOutlineWiderOpacity = 0.0f;
                const std::array<QPoint, 4> cardinalNeighbors = {
                    QPoint(outlineSampleOffset, 0),
                    QPoint(-outlineSampleOffset, 0),
                    QPoint(0, outlineSampleOffset),
                    QPoint(0, -outlineSampleOffset),
                };
                const auto addSelectedOutlineCoverage =
                    [&](const QPoint &offset, bool widerRing) {
                        const int sx = std::clamp(
                            depthX + offset.x(), 0, width - 1);
                        const int sy = std::clamp(
                            depthY + offset.y(), 0, height - 1);
                        const int neighborIndex = sy * width + sx;
                        const quint64 neighborId =
                            selectedOutlineSamples[neighborIndex].objectId;
                        const bool xrayMeshEdge = xrayEnabled &&
                            (xraySelectedMeshOutlineIds.contains(
                                 selectionObjectId) ||
                             xraySelectedMeshOutlineIds.contains(neighborId));
                        // Blender's X-Ray wire resolve omits expansion toward
                        // negative screen X/Y. QImage's Y axis points down, so
                        // those directions are left and down here.
                        if (xrayMeshEdge &&
                            (offset.x() < 0 || offset.y() > 0)) {
                            return;
                        }
                        const bool selectionBoundary =
                            selectionObjectId != neighborId &&
                            (selectionObjectId != 0 || neighborId != 0);
                        if (!selectionBoundary) return;
                        const RasterOutlineSample &selectedSide =
                            selectionObjectId != 0
                                ? selectedOutlineSamples[sampleIndex]
                                : selectedOutlineSamples[neighborIndex];
                        const int selectedSideIndex = selectionObjectId != 0
                            ? sampleIndex : neighborIndex;
                        const RasterSample &sceneSample =
                            samples[selectedSideIndex];
                        const bool occluded = !xrayEnabled &&
                            sceneSample.objectId != 0 &&
                            selectedSide.depth >
                                sceneSample.depth + 1.0e-5f;
                        const float boundaryOpacity = occluded ? 0.35f : 1.0f;
                        if (widerRing) {
                            selectedOutlineWiderOpacity = std::max(
                                selectedOutlineWiderOpacity, boundaryOpacity);
                        } else {
                            selectedOutlineCoverage += boundaryOpacity;
                            selectedOutlineHardOpacity = std::max(
                                selectedOutlineHardOpacity, boundaryOpacity);
                        }
                    };
                if (drawSelectionOutline) {
                    for (const QPoint &offset : cardinalNeighbors) {
                        addSelectedOutlineCoverage(offset, false);
                    }
                    if (selectedOutlineCoverage <= 0.0f) {
                        const std::array<QPoint, 4> diagonalNeighbors = {
                            QPoint(outlineSampleOffset, outlineSampleOffset),
                            QPoint(outlineSampleOffset, -outlineSampleOffset),
                            QPoint(-outlineSampleOffset, outlineSampleOffset),
                            QPoint(-outlineSampleOffset, -outlineSampleOffset),
                        };
                        for (const QPoint &offset : diagonalNeighbors) {
                            addSelectedOutlineCoverage(offset, false);
                        }
                    }
                    if (selectedOutlineCoverage <= 0.0f &&
                        outlineSampleOffset > 1) {
                        const std::array<QPoint, 4> widerCardinalNeighbors = {
                            QPoint(outlineSampleOffset * 2, 0),
                            QPoint(-outlineSampleOffset * 2, 0),
                            QPoint(0, outlineSampleOffset * 2),
                            QPoint(0, -outlineSampleOffset * 2),
                        };
                        for (const QPoint &offset : widerCardinalNeighbors) {
                            addSelectedOutlineCoverage(offset, true);
                        }
                        if (selectedOutlineWiderOpacity <= 0.0f) {
                            const std::array<QPoint, 4> widerDiagonalNeighbors = {
                                QPoint(outlineSampleOffset * 2,
                                       outlineSampleOffset * 2),
                                QPoint(outlineSampleOffset * 2,
                                       -outlineSampleOffset * 2),
                                QPoint(-outlineSampleOffset * 2,
                                       outlineSampleOffset * 2),
                                QPoint(-outlineSampleOffset * 2,
                                       -outlineSampleOffset * 2),
                            };
                            for (const QPoint &offset : widerDiagonalNeighbors) {
                                addSelectedOutlineCoverage(offset, true);
                            }
                        }
                    }
                }
                for (const QPoint &offset : cardinalNeighbors) {
                    const int sx = std::clamp(depthX + offset.x(), 0, width - 1);
                    const int sy = std::clamp(depthY + offset.y(), 0, height - 1);
                    const int neighborIndex = sy * width + sx;
                    const quint64 neighborId = drawWorkbenchOutline
                        ? outlineSamples[neighborIndex].objectId
                        : samples[neighborIndex].objectId;
                    differentNeighbors += neighborId != objectId;
                }
                const auto blend = [&](const QColor &color, float coverage) {
                    const float sourceAlpha =
                        static_cast<float>(color.alphaF()) * coverage;
                    if (sourceAlpha <= 0.0f) return;
                    const float outlineRed = static_cast<float>(color.red());
                    const float outlineGreen = static_cast<float>(color.green());
                    const float outlineBlue = static_cast<float>(color.blue());
                    const QRgb pixel = out[x];
                    const float dstAlpha = qAlpha(pixel) / 255.0f;
                    const float outAlpha = sourceAlpha +
                                           dstAlpha * (1.0f - sourceAlpha);
                    const auto blendChannel = [&](int dst, float src) {
                    if (outAlpha <= 0.0f) return 0;
                    return std::clamp(qRound(
                        (src * sourceAlpha + dst * dstAlpha *
                         (1.0f - sourceAlpha)) / outAlpha), 0, 255);
                    };
                    out[x] = qRgba(blendChannel(qRed(pixel), outlineRed),
                                   blendChannel(qGreen(pixel), outlineGreen),
                                   blendChannel(qBlue(pixel), outlineBlue),
                                   std::clamp(qRound(outAlpha * 255.0f), 0, 255));
                };
                if (drawWorkbenchOutline) {
                    const float coverage =
                        static_cast<float>(differentNeighbors) / 4.0f;
                    if (standardLinearOutlineBlend) {
                        out[x] = blendStandardOutline(out[x], coverage);
                    } else {
                        blend(outline, coverage);
                    }
                }
                if (drawSelectionOutline) {
                    const float coverage = selectedOutlineCoverage > 0.0f
                        ? (smoothWiresOverlay
                               ? selectedOutlineCoverage / 4.0f
                               : selectedOutlineHardOpacity)
                        : (smoothWiresOverlay
                               ? 0.25f * selectedOutlineWiderOpacity
                               : selectedOutlineWiderOpacity);
                    blend(selectionOutline, coverage);
                }
            }
        }
        *colorImage = result.convertToFormat(colorImage->format());
    };

    if (cavityEnabled) {

    const bool ssao = settings.cavityType == ViewportCavityType::World ||
                      settings.cavityType == ViewportCavityType::Both;
    const bool curvature = settings.cavityType == ViewportCavityType::Screen ||
                           settings.cavityType == ViewportCavityType::Both;
    const int requested = std::clamp(settings.cavitySamples, 1, 500);
    // Keep the bounded sample count stable as geometry enters or leaves the
    // view. Using covered pixels here made cavity quality depend on scene
    // coverage even when the viewport size and requested sample count stayed
    // unchanged.
    const int effectiveSamples = rasterWorkbenchCavitySampleCount(
        requested, static_cast<qsizetype>(opaqueSamples.size()));
    const float radius = rasterWorkbenchCavityRadius(
        static_cast<float>(settings.cavityDistance),
        static_cast<float>(
            transform.viewScalePixelsPerWorldUnit(viewportSize)),
        static_cast<float>(dpr));
    const float attenuation = std::max(0.0f,
        static_cast<float>(settings.cavityAttenuation));
    const Point3D cameraPosition = perspective
        ? transform.cameraPosition(viewportSize) : transform.viewTarget();
    const Point3D viewTarget = transform.viewTarget();
    const QMatrix4x4 cavityViewProjection =
        viewportViewProjection(transform, viewportSize);
    const auto isNearPlaneSample = [&cavityViewProjection](
                                      const QVector3D &position) {
        const QVector4D clip = cavityViewProjection *
            QVector4D(position, 1.0f);
        if (!std::isfinite(clip.w()) || clip.w() <= 1.0e-12f ||
            !std::isfinite(clip.z())) {
            return false;
        }
        const float normalizedDepth = clip.z() / clip.w() * 0.5f + 0.5f;
        return normalizedDepth <= 1.0e-6f;
    };
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
            const bool skipSsaoForNearPlane =
                ssao && isNearPlaneSample(center.position);
            const float step = rasterWorkbenchCavityStep(
                radius, center.position, cameraPosition, viewTarget,
                towardCamera, perspective);
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
                const float curvatureStep =
                    workbenchEffectFractionalUiScale(dpr);
                const auto nearestSampleAt = [&](float sx, float sy)
                    -> const RasterSample & {
                    const int ix = std::clamp(
                        static_cast<int>(std::floor(sx + 0.5f)), 0,
                        width - 1);
                    const int iy = std::clamp(
                        static_cast<int>(std::floor(sy + 0.5f)), 0,
                        height - 1);
                    return opaqueSamples[iy * width + ix];
                };
                // Blender's integer and floating-point texture samplers both
                // use nearest filtering here. Preserve the fractional UV
                // offset before resolving each cardinal tap to its texel.
                const RasterSample &up = nearestSampleAt(
                    float(x), float(y) - curvatureStep);
                const RasterSample &down = nearestSampleAt(
                    float(x), float(y) + curvatureStep);
                const RasterSample &right = nearestSampleAt(
                    float(x) + curvatureStep, float(y));
                const RasterSample &left = nearestSampleAt(
                    float(x) - curvatureStep, float(y));
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
            // The QPainter depth rows run top-down; Workbench's GL fragment
            // coordinates and uploaded noise texture rows run bottom-up.
            const int noiseY = height - 1 - y;
            const float noiseAngle = rasterCavityNoise(x, noiseY, 0) *
                                     6.28318530718f;
            const float rotationX = std::cos(noiseAngle);
            const float rotationY = std::sin(noiseAngle);
            const float noiseRadius = std::clamp(
                rasterCavityNoise(x, noiseY, 1) - 0.5f, -0.499f, 0.499f) /
                static_cast<float>(requested);
            const bool captureTaps = cavityTapCapture != nullptr &&
                cavityTapCapture->receiverPixels.contains(QPoint(x, y));
            for (int i = 0; i < effectiveSamples; ++i) {
                // If the bounded CPU budget reduces the sample count, spread
                // those taps over the requested Workbench sequence instead of
                // retaining only its small-radius prefix.
                const int sampleIndex = std::min(
                    requested - 1, static_cast<int>(
                        (static_cast<double>(i) + 0.5) * requested /
                        effectiveSamples));
                // Match Workbench's deterministic Hammersley-like disk sample
                // sequence and per-pixel blue-noise rotation/jitter.
                const float sampleRadius = std::fmod(
                    (static_cast<float>(sampleIndex) + 0.5f) /
                        static_cast<float>(requested), 1.0f);
                const float angle = radicalInverseVdc(
                    static_cast<unsigned int>(sampleIndex)) * 6.28318530718f;
                const float directionX = std::cos(angle);
                const float directionY = std::sin(angle);
                // GLSL's dot products with rotX=(cos,sin) and
                // rotY=(-sin,cos) rotate the disk direction by the negative
                // blue-noise angle. Keep the QPainter fallback on that same
                // orientation rather than applying the inverse rotation.
                const float jitteredX = directionX * rotationX +
                                        directionY * rotationY;
                const float jitteredY = -directionX * rotationY +
                                        directionY * rotationX;
                const float sampleDistance = std::max(
                    0.0f, sampleRadius + noiseRadius);
                const float sampleOffsetX =
                    jitteredX * sampleDistance * step;
                const float sampleOffsetY =
                    -jitteredY * sampleDistance * step;
                const float samplePixelX = x + 0.5f + sampleOffsetX;
                const float samplePixelY = y + 0.5f + sampleOffsetY;
                const QPointF sampleScreen(samplePixelX / dpr,
                                           samplePixelY / dpr);
                // Workbench's texture Y grows upward in GL coordinates;
                // retained QPainter samples use top-down rows.
                const QPoint nearestSample = rasterWorkbenchNearestCavityPixel(
                    QPointF(samplePixelX, samplePixelY), QSize(width, height));
                const int sx = nearestSample.x();
                const int sy = nearestSample.y();
                RasterWorkbenchCavityTap tapCapture;
                if (captureTaps) {
                    tapCapture.receiverX = x;
                    tapCapture.receiverY = y;
                    tapCapture.requestedSamples = requested;
                    tapCapture.effectiveSamples = effectiveSamples;
                    tapCapture.tap = i;
                    tapCapture.sampleIndex = sampleIndex;
                    tapCapture.sampleX = sx;
                    tapCapture.sampleY = sy;
                    tapCapture.samplePixelX = samplePixelX;
                    tapCapture.samplePixelY = samplePixelY;
                    tapCapture.inBounds = sx >= 0 && sy >= 0;
                }
                if (sx < 0 || sy < 0) {
                    if (captureTaps)
                        cavityTapCapture->taps.push_back(tapCapture);
                    continue;
                }
                const RasterSample &neighbor = opaqueSamples[sy * width + sx];
                if (captureTaps)
                    tapCapture.neighborObjectId = neighbor.objectId;
                if (neighbor.objectId == 0) {
                    if (ssao && !skipSsaoForNearPlane) {
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
                            if (captureTaps) {
                                tapCapture.distance = distance;
                                tapCapture.cavityDot = cavityDot;
                                tapCapture.edgeDot = edgeDot;
                                tapCapture.bias = bias;
                                tapCapture.attenuation = distanceAttenuation;
                            }
                            if (cavityDot > -bias) {
                                cavityAmount += cavityDot *
                                                distanceAttenuation;
                                if (captureTaps)
                                    tapCapture.cavityContribution =
                                        cavityDot * distanceAttenuation;
                            }
                            if (edgeDot > bias) {
                                edgeAmount += edgeDot *
                                              distanceAttenuation;
                                if (captureTaps)
                                    tapCapture.edgeContribution =
                                        edgeDot * distanceAttenuation;
                            }
                            hadBackgroundCavitySample = true;
                        }
                    }
                    if (captureTaps)
                        cavityTapCapture->taps.push_back(tapCapture);
                    continue;
                }
                ++validNeighbors;
                if (ssao && !skipSsaoForNearPlane) {
                    // Blender reconstructs the sampled view position from
                    // the fractional UV and sampled depth. Retained CPU depth
                    // samples store only the pixel-center position, so move
                    // that point across its constant camera-depth plane to
                    // the actual sample coordinate before measuring SSAO.
                    QVector3D sampledPosition = neighbor.position;
                    const WorkPlaneFrame neighborViewPlane =
                        makeWorkPlaneFrameFromNormal(
                            {neighbor.position.x(), neighbor.position.y(),
                             neighbor.position.z()},
                            towardCamera, viewRight);
                    QPointF fractionalSample;
                    if (isValidWorkPlaneFrame(neighborViewPlane) &&
                        transform.screenToWorkPlane(
                            sampleScreen, viewportSize, neighborViewPlane,
                            &fractionalSample)) {
                        const Point3D reconstructed =
                            workPlaneFramePointToWorld(fractionalSample,
                                                       neighborViewPlane);
                        sampledPosition = QVector3D(
                            static_cast<float>(reconstructed.x),
                            static_cast<float>(reconstructed.y),
                            static_cast<float>(reconstructed.z));
                    }
                    const QVector3D direction = sampledPosition -
                                                center.position;
                    const float distance = direction.length();
                    const float cavityDot = QVector3D::dotProduct(
                        direction, center.normal);
                    const float edgeDot = -cavityDot;
                    const float bias = 0.05f * distance + 0.0001f;
                    const float distanceAttenuation = 1.0f /
                        std::max(distance * (1.0f + distance * distance *
                                             attenuation), 1.0e-8f);
                    if (captureTaps) {
                        tapCapture.distance = distance;
                        tapCapture.cavityDot = cavityDot;
                        tapCapture.edgeDot = edgeDot;
                        tapCapture.bias = bias;
                        tapCapture.attenuation = distanceAttenuation;
                    }
                    if (cavityDot > -bias) {
                        cavityAmount += cavityDot * distanceAttenuation;
                        if (captureTaps)
                            tapCapture.cavityContribution =
                                cavityDot * distanceAttenuation;
                    }
                    if (edgeDot > bias) {
                        edgeAmount += edgeDot * distanceAttenuation;
                        if (captureTaps)
                            tapCapture.edgeContribution =
                                edgeDot * distanceAttenuation;
                    }
                }
                if (captureTaps)
                    cavityTapCapture->taps.push_back(tapCapture);
            }
            // Curvature is evaluated independently of the SSAO sample disk.
            // Keep its response even when every World-cavity tap is outside
            // the viewport or otherwise has no usable depth sample.
            if (validNeighbors == 0 && !hadBackgroundCavitySample &&
                !curvature) {
                continue;
            }
            cavityAmount = ssao
                ? saturate(cavityAmount /
                           static_cast<float>(effectiveSamples) * valley)
                : 0.0f;
            edgeAmount = ssao
                ? edgeAmount / static_cast<float>(effectiveSamples) * ridge
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

    if (shadowEnabled && !lightDepth.isEmpty() &&
        settings.shadowIntensity > 0.0) {
        QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage result = source.copy();
        QVector3D lightDirection(-settings.shadowDirection.x(),
                                  settings.shadowDirection.z(),
                                  -settings.shadowDirection.y());
        if (lightDirection.lengthSquared() < 1.0e-10f)
            lightDirection = QVector3D(-0.57735026f, 0.57735026f,
                                       -0.57735026f);
        lightDirection.normalize();
        const auto shadowFactorAt = [&](const QVector2D &lightPixel,
                                        float receiverDepth,
                                        const QVector3D &receiverPosition,
                                        const QVector3D &normal,
                                        const QPointF &screenPixelCenter,
                                        bool applyCasterOcclusion = true) {
            if (!applyCasterOcclusion) {
                // Blender's transparent pass applies the normal-facing
                // light factor without consulting opaque shadow visibility.
                return rasterWorkbenchShadowFactor(
                    normal, lightDirection,
                    static_cast<float>(settings.shadowIntensity),
                    static_cast<float>(settings.shadowOffset),
                    static_cast<float>(settings.shadowFocus), false);
            }
            // Project the four quarter-pixel coverage points onto the
            // receiver plane before looking them up in the light map. A
            // viewport pixel becomes a skewed footprint in light-map space
            // for an angled receiver, so independent light-X/Y offsets do
            // not represent its actual coverage.
            const Point3D receiverOrigin{receiverPosition.x(),
                                         receiverPosition.y(),
                                         receiverPosition.z()};
            const Point3D receiverNormal{normal.x(), normal.y(), normal.z()};
            const WorkPlaneFrame receiverPlane = makeWorkPlaneFrameFromNormal(
                receiverOrigin, receiverNormal);
            const qreal pixelQuarter = 0.25 / std::max<qreal>(dpr, 1.0e-6);
            const std::array<QPointF, 4> screenOffsets{{
                QPointF(-pixelQuarter, -pixelQuarter),
                QPointF(pixelQuarter, -pixelQuarter),
                QPointF(-pixelQuarter, pixelQuarter),
                QPointF(pixelQuarter, pixelQuarter)}};
            float factorSum = 0.0f;
            for (const QPointF &screenOffset : screenOffsets) {
                QPointF receiverPlanePoint;
                QVector2D samplePosition = lightPixel;
                if (transform.screenToWorkPlaneUnclipped(
                        screenPixelCenter + screenOffset, viewportSize,
                        receiverPlane, &receiverPlanePoint)) {
                    const Point3D worldPoint = workPlaneFramePointToWorld(
                        receiverPlanePoint, receiverPlane);
                    const QVector3D worldPosition(
                        float(worldPoint.x), float(worldPoint.y),
                        float(worldPoint.z));
                    const QPointF projectedLight(
                        (QVector3D::dotProduct(worldPosition,
                                               shadowLightRight) -
                         shadowLightMinX) /
                            (shadowLightMaxX - shadowLightMinX) *
                            lightMapWidth,
                        (shadowLightMaxY -
                         QVector3D::dotProduct(worldPosition,
                                               shadowLightUp)) /
                            (shadowLightMaxY - shadowLightMinY) *
                            lightMapHeight);
                    samplePosition = QVector2D(projectedLight.x(),
                                               projectedLight.y());
                }
                if (samplePosition.x() < 0.0f || samplePosition.y() < 0.0f ||
                    samplePosition.x() >= lightMapWidth ||
                    samplePosition.y() >= lightMapHeight) {
                    // The light-map bounds enclose visible opaque receiver
                    // centers and their four coverage samples, so outside
                    // samples fall beyond the area that needs a shadow map.
                    factorSum += rasterWorkbenchShadowFactor(
                        normal, lightDirection,
                        static_cast<float>(settings.shadowIntensity),
                        static_cast<float>(settings.shadowOffset),
                        static_cast<float>(settings.shadowFocus), false);
                    continue;
                }
                const int lightX = std::clamp(qFloor(samplePosition.x()), 0,
                                              lightMapWidth - 1);
                const int lightY = std::clamp(qFloor(samplePosition.y()), 0,
                                              lightMapHeight - 1);
                const qsizetype mapIndex =
                    qsizetype(lightY) * lightMapWidth + lightX;
                const float casterDepth = lightDepth[mapIndex];
                float localDepthBias = softwareDepthBias;
                if (std::isfinite(casterDepth) &&
                    std::isfinite(receiverDepth)) {
                    const float comparedDepthMagnitude = std::max(
                        std::abs(casterDepth), std::abs(receiverDepth));
                    const float nextComparedDepth = std::nextafter(
                        comparedDepthMagnitude,
                        std::numeric_limits<float>::infinity());
                    localDepthBias = std::min(
                        softwareDepthBias,
                        (nextComparedDepth - comparedDepthMagnitude) * 2.0f);
                }
                const bool shadowed = std::isfinite(casterDepth) &&
                    casterDepth < receiverDepth - localDepthBias;
                factorSum += rasterWorkbenchShadowFactor(
                    normal, lightDirection,
                    static_cast<float>(settings.shadowIntensity),
                    static_cast<float>(settings.shadowOffset),
                    static_cast<float>(settings.shadowFocus), shadowed);
            }
            return factorSum / static_cast<float>(screenOffsets.size());
        };
        const std::vector<RasterWorkbenchLinearPixel> *
            perFragmentShadowedTransparent = nullptr;
        bool hasPerFragmentShadowResolve = false;
        if (surfaceLayers != nullptr &&
            surfaceLayers->sceneLinearShadowedTransparentColorAvailable &&
            surfaceLayers->sceneLinearSize == QSize(width, height) &&
            outputWidth == width && outputHeight == height &&
            surfaceLayers->sceneLinearTransparentColor.size() ==
                static_cast<std::size_t>(pixelCount) &&
            surfaceLayers->sceneLinearShadowedTransparentColor.size() ==
                static_cast<std::size_t>(pixelCount)) {
            perFragmentShadowedTransparent =
                &surfaceLayers->sceneLinearShadowedTransparentColor;
            hasPerFragmentShadowResolve = true;
        }
        const bool hasSeparateOpaqueShadowLayers =
            surfaceLayers != nullptr &&
            surfaceLayers->sceneLinearSize == QSize(width, height) &&
            surfaceLayers->sceneLinearColor.size() ==
                static_cast<std::size_t>(pixelCount) &&
            surfaceLayers->sceneLinearOpaqueColor.size() ==
                static_cast<std::size_t>(pixelCount) &&
            surfaceLayers->sceneLinearRegularOpaqueColor.size() ==
                static_cast<std::size_t>(pixelCount) &&
            surfaceLayers->sceneLinearInFrontOpaqueColor.size() ==
                static_cast<std::size_t>(pixelCount) &&
            surfaceLayers->sceneLinearTransparentColor.size() ==
                static_cast<std::size_t>(pixelCount);
        for (int y = 0; y < outputHeight; ++y) {
            QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
            const int depthY = std::clamp(
                static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
                0, height - 1);
            for (int x = 0; x < outputWidth; ++x) {
                const int depthX = std::clamp(
                    static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                    0, width - 1);
                const int index = depthY * width + depthX;
                const RasterSample &receiver = opaqueSamples[index];
                const RasterSample &regularReceiver =
                    regularOpaqueSamples[index];
                const RasterSample &inFrontReceiver =
                    inFrontOpaqueSamples[index];
                const RasterSample &visible = samples[index];
                const bool hasOpaqueReceiver = receiver.objectId != 0;
                const bool hasTransparentReceiver = visible.objectId != 0 &&
                    visible.sourceAlpha > 0.0f &&
                    !isOpaqueWorkbenchAlpha(visible.sourceAlpha);
                if ((!hasOpaqueReceiver && !hasTransparentReceiver) ||
                    visible.objectId == 0) continue;
                const bool hasTransparentTop =
                    hasOpaqueReceiver &&
                    (receiver.objectId != visible.objectId ||
                     std::abs(receiver.depth - visible.depth) >
                         std::max(1.0e-5f,
                                  std::abs(receiver.depth) * 1.0e-6f));
                const bool hasRetainedLayers = surfaceLayers != nullptr &&
                    !surfaceLayers->opaqueColor.isNull() &&
                    !surfaceLayers->transparentColor.isNull() &&
                    surfaceLayers->opaqueColor.size() == colorImage->size() &&
                    surfaceLayers->transparentColor.size() == colorImage->size();
                if ((hasTransparentTop || hasTransparentReceiver) &&
                    !hasRetainedLayers) continue;
                const QPointF receiverScreenPosition(
                    (depthX + 0.5) / dpr, (depthY + 0.5) / dpr);
                const float regularOpaqueFactor =
                    regularReceiver.objectId != 0
                        ? shadowFactorAt(
                              regularReceiverLightCoordinates[index],
                              regularReceiverLightDepth[index],
                              regularReceiver.position,
                              regularReceiver.normal,
                              receiverScreenPosition)
                        : 1.0f;
                const float inFrontOpaqueFactor =
                    inFrontReceiver.objectId != 0
                        ? shadowFactorAt(
                              inFrontReceiverLightCoordinates[index],
                              inFrontReceiverLightDepth[index],
                              inFrontReceiver.position,
                              inFrontReceiver.normal,
                              receiverScreenPosition)
                        : 1.0f;
                const float opaqueFactor = inFrontReceiver.objectId != 0
                    ? inFrontOpaqueFactor : regularOpaqueFactor;
                const float transparentFactor = hasTransparentReceiver
                    ? shadowFactorAt(
                          transparentReceiverLightCoordinates[index],
                          transparentReceiverLightDepth[index],
                          visible.position, visible.normal,
                          QPointF((depthX + 0.5) / dpr,
                                  (depthY + 0.5) / dpr), false)
                    : 1.0f;
                const bool perFragmentTransparentChanged =
                    hasPerFragmentShadowResolve &&
                    ((*perFragmentShadowedTransparent)[
                         static_cast<std::size_t>(index)].red !=
                         surfaceLayers->sceneLinearTransparentColor[
                             static_cast<std::size_t>(index)].red ||
                     (*perFragmentShadowedTransparent)[
                         static_cast<std::size_t>(index)].green !=
                         surfaceLayers->sceneLinearTransparentColor[
                             static_cast<std::size_t>(index)].green ||
                     (*perFragmentShadowedTransparent)[
                         static_cast<std::size_t>(index)].blue !=
                         surfaceLayers->sceneLinearTransparentColor[
                         static_cast<std::size_t>(index)].blue);
                const bool opaqueLayerShadowChanged =
                    regularOpaqueFactor < 1.0f || inFrontOpaqueFactor < 1.0f;
                if (!opaqueLayerShadowChanged &&
                    transparentFactor >= 1.0f &&
                    !perFragmentTransparentChanged) continue;
                if (surfaceLayers != nullptr &&
                    (hasSeparateOpaqueShadowLayers ||
                     surfaceLayers->sceneLinearOutlinePending ||
                     surfaceLayers->sceneLinearDofPending ||
                     hasPerFragmentShadowResolve) &&
                    surfaceLayers->sceneLinearSize == QSize(width, height) &&
                    surfaceLayers->sceneLinearColor.size() ==
                        static_cast<std::size_t>(width) * height &&
                    surfaceLayers->sceneLinearOpaqueColor.size() ==
                        static_cast<std::size_t>(width) * height &&
                    surfaceLayers->sceneLinearRegularOpaqueColor.size() ==
                        static_cast<std::size_t>(width) * height &&
                    surfaceLayers->sceneLinearInFrontOpaqueColor.size() ==
                        static_cast<std::size_t>(width) * height &&
                    surfaceLayers->sceneLinearTransparentColor.size() ==
                        static_cast<std::size_t>(width) * height &&
                    (opaqueLayerShadowChanged || transparentFactor < 1.0f ||
                     perFragmentTransparentChanged)) {
                    const std::size_t linearIndex =
                        static_cast<std::size_t>(index);
                    RasterWorkbenchLinearPixel &linear =
                        surfaceLayers->sceneLinearColor[linearIndex];
                    const RasterWorkbenchLinearPixel &regularOpaque =
                        surfaceLayers->sceneLinearRegularOpaqueColor[
                            linearIndex];
                    const RasterWorkbenchLinearPixel &inFrontOpaque =
                        surfaceLayers->sceneLinearInFrontOpaqueColor[
                            linearIndex];
                    const RasterWorkbenchLinearPixel &transparent =
                        surfaceLayers->sceneLinearTransparentColor[linearIndex];
                    const float remain = 1.0f - transparent.alpha;
                    const float regularBehindFront =
                        1.0f - inFrontOpaque.alpha;
                    linear.red +=
                        (regularOpaque.red * (regularOpaqueFactor - 1.0f) *
                             regularBehindFront +
                         inFrontOpaque.red * (inFrontOpaqueFactor - 1.0f)) *
                        remain;
                    linear.green +=
                        (regularOpaque.green * (regularOpaqueFactor - 1.0f) *
                             regularBehindFront +
                         inFrontOpaque.green * (inFrontOpaqueFactor - 1.0f)) *
                        remain;
                    linear.blue +=
                        (regularOpaque.blue * (regularOpaqueFactor - 1.0f) *
                             regularBehindFront +
                         inFrontOpaque.blue * (inFrontOpaqueFactor - 1.0f)) *
                        remain;
                    if (hasPerFragmentShadowResolve) {
                        if (perFragmentTransparentChanged) {
                            const RasterWorkbenchLinearPixel &shadowedTransparent =
                                (*perFragmentShadowedTransparent)[linearIndex];
                            linear.red += shadowedTransparent.red - transparent.red;
                            linear.green += shadowedTransparent.green - transparent.green;
                            linear.blue += shadowedTransparent.blue - transparent.blue;
                        }
                    } else if (hasTransparentReceiver) {
                        linear.red += transparent.red *
                                      (transparentFactor - 1.0f);
                        linear.green += transparent.green *
                                        (transparentFactor - 1.0f);
                        linear.blue += transparent.blue *
                                       (transparentFactor - 1.0f);
                    }
                }
                const QRgb pixel = source.pixel(x, y);
                QRgb transparent = 0;
                if (hasTransparentTop) {
                    const QRgb opaque = surfaceLayers->opaqueColor.pixel(x, y);
                    transparent =
                        surfaceLayers->transparentColor.pixel(x, y);
                    const float remain =
                        1.0f - qAlpha(transparent) / 255.0f;
                    const auto shadeOpaqueContribution = [&](int channel,
                                                             int opaqueChannel) {
                        return std::clamp(
                            channel + qRound(opaqueChannel *
                                             (opaqueFactor - 1.0f) *
                                             remain),
                            0, 255);
                    };
                    out[x] = qRgba(shadeOpaqueContribution(qRed(pixel), qRed(opaque)),
                                   shadeOpaqueContribution(qGreen(pixel), qGreen(opaque)),
                                   shadeOpaqueContribution(qBlue(pixel), qBlue(opaque)),
                                   qAlpha(pixel));
                } else if (hasOpaqueReceiver && !hasTransparentReceiver) {
                    out[x] = qRgba(qRound(qRed(pixel) * opaqueFactor),
                                   qRound(qGreen(pixel) * opaqueFactor),
                                   qRound(qBlue(pixel) * opaqueFactor),
                                   qAlpha(pixel));
                } else {
                    out[x] = pixel;
                }
                if (hasTransparentReceiver && hasRetainedLayers &&
                    !hasPerFragmentShadowResolve) {
                    if (!hasTransparentTop) {
                        transparent = surfaceLayers->transparentColor.pixel(x, y);
                    }
                    const auto shadeTransparentContribution =
                        [&](int channel, int transparentChannel) {
                            return std::clamp(
                                channel + qRound(transparentChannel *
                                    (transparentFactor - 1.0f)),
                                0, 255);
                        };
                    out[x] = qRgba(
                        shadeTransparentContribution(qRed(out[x]),
                                                      qRed(transparent)),
                        shadeTransparentContribution(qGreen(out[x]),
                                                      qGreen(transparent)),
                        shadeTransparentContribution(qBlue(out[x]),
                                                      qBlue(transparent)),
                        qAlpha(out[x]));
                }
            }
        }
        *colorImage = result.convertToFormat(colorImage->format());
        if ((hasPerFragmentShadowResolve || hasSeparateOpaqueShadowLayers) &&
            surfaceLayers != nullptr &&
            !surfaceLayers->sceneLinearOutlinePending &&
            !surfaceLayers->sceneLinearDofPending) {
            (void)displayRetainedSceneLinear();
        }
    }

    if (surfaceLayers != nullptr &&
        surfaceLayers->sceneLinearOutlinePending &&
        surfaceLayers->sceneLinearSize == QSize(outputWidth, outputHeight) &&
        surfaceLayers->sceneLinearColor.size() ==
            static_cast<std::size_t>(outputWidth) * outputHeight &&
        surfaceLayers->sceneLinearOutlineCoverage.size() ==
            static_cast<std::size_t>(outputWidth) * outputHeight) {
        const QColor outline = settings.outlineColor;
        const QVector3D outlineColor = workbenchSrgbToSceneLinear(
            QVector3D(outline.redF(), outline.greenF(), outline.blueF()));
        for (std::size_t index = 0;
             index < surfaceLayers->sceneLinearColor.size(); ++index) {
            RasterWorkbenchLinearPixel &pixel =
                surfaceLayers->sceneLinearColor[index];
            const float outlineAlpha = std::clamp(
                surfaceLayers->sceneLinearOutlineCoverage[index], 0.0f, 1.0f);
            const float remain = 1.0f - outlineAlpha;
            pixel.red = outlineColor.x() * outlineAlpha + pixel.red * remain;
            pixel.green = outlineColor.y() * outlineAlpha + pixel.green * remain;
            pixel.blue = outlineColor.z() * outlineAlpha + pixel.blue * remain;
            pixel.alpha = outlineAlpha + pixel.alpha * remain;
        }
        surfaceLayers->outlineAppliedBeforeDisplay = true;
        surfaceLayers->sceneLinearOutlinePending = false;
        if (!surfaceLayers->sceneLinearDofPending)
            (void)displayRetainedSceneLinear();
    }

    applyOutline(rasterWorkbenchOutlineEnabled, false);

    // QPainter fallback approximation: use the software-resolved visible
    // surface depth to estimate a physical circle of confusion, apply a
    // CoC-aware near/far prefilter, and gather aperture samples. This remains
    // camera-gated and uses camera-relative depth for either projection.
    if (depthOfFieldEnabled) {
        struct LinearGatherPixel {
            // Premultiplied RGB and alpha from the scene-linear source/gather.
            float red = 0.0f;
            float green = 0.0f;
            float blue = 0.0f;
            float alpha = 0.0f;
        };
        const bool sceneLinearDofActive = surfaceLayers != nullptr &&
            surfaceLayers->sceneLinearDofPending &&
            surfaceLayers->sceneLinearSize == QSize(outputWidth, outputHeight) &&
            surfaceLayers->sceneLinearColor.size() ==
                static_cast<std::size_t>(outputWidth) * outputHeight &&
            (!settings.outline ||
             surfaceLayers->outlineAppliedBeforeDisplay);
        const QImage source = colorImage->convertToFormat(QImage::Format_ARGB32);
        QImage gatheredResult = source.copy();
        const auto decodeDisplayChannel = [](int channel) {
            return workbenchSrgbToSceneLinear(
                       static_cast<float>(channel) / 255.0f) * 255.0f;
        };
        if (!sceneLinearDofActive) {
            // The low-memory fallback no longer has the pre-display scene
            // values. Decode the sRGB display channels before its blur so
            // filtering does not darken edges by averaging encoded bytes.
            for (int y = 0; y < outputHeight; ++y) {
                QRgb *out = reinterpret_cast<QRgb *>(
                    gatheredResult.scanLine(y));
                const auto *in = reinterpret_cast<const QRgb *>(
                    source.constScanLine(y));
                for (int x = 0; x < outputWidth; ++x) {
                    const float alpha = qAlpha(in[x]) / 255.0f;
                    out[x] = qRgba(
                        qRound(decodeDisplayChannel(qRed(in[x])) * alpha),
                        qRound(decodeDisplayChannel(qGreen(in[x])) * alpha),
                        qRound(decodeDisplayChannel(qBlue(in[x])) * alpha),
                        qAlpha(in[x]));
                }
            }
        }
        std::vector<LinearGatherPixel> gatheredLinear;
        if (sceneLinearDofActive) {
            gatheredLinear.resize(
                static_cast<std::size_t>(outputWidth) * outputHeight);
            for (std::size_t index = 0; index < gatheredLinear.size(); ++index) {
                const RasterWorkbenchLinearPixel &pixel =
                    surfaceLayers->sceneLinearColor[index];
                gatheredLinear[index] = {
                    pixel.red, pixel.green, pixel.blue, pixel.alpha};
            }
        }
        const qsizetype outputPixelCount =
            qsizetype(outputWidth) * outputHeight;
        QVector<float> dofMixes(outputPixelCount, 0.0f);
        const auto dofKernel = rasterWorkbenchDofKernel(
            cameraOptics.apertureBlades,
            cameraOptics.apertureRotationRadians,
            cameraOptics.apertureRatio);
        // Blender Workbench converts lens and sensor dimensions from mm to
        // the scene's meter-based coordinates; depth and focus stay in scene
        // coordinates (also the convention of the imported Blender geometry).
        const qreal focal = cameraOptics.focalLengthMillimeters * 0.001;
        const qreal focus = cameraOptics.focusDistance;
        const qreal fStop = cameraOptics.fStop;
        const qreal sensorScale =
            std::isfinite(cameraFrameSensorScale) &&
                    cameraFrameSensorScale > 0.0
                ? cameraFrameSensorScale : 1.0;
        const qreal sensorWidth = cameraOptics.opticalSensorSizeMillimeters *
                                  sensorScale * 0.001;
        if (std::isfinite(focal) && focal > 0.0 && std::isfinite(focus) &&
            focus > 0.0 && std::isfinite(fStop) && fStop > 0.0 &&
            std::isfinite(sensorWidth) && sensorWidth > 0.0 &&
            std::abs(focus - focal) > 1.0e-6) {
            // The full-screen resolve needs to consider clear-depth pixels
            // near geometry, while avoiding a 49-tap gather across empty
            // regions of a large software target. Dilate covered output
            // pixels by the maximum bounded CoC radius in linear time.
            QVector<quint8> dofCoverage(outputPixelCount, 0);
            QVector<quint8> horizontalCoverage(outputPixelCount, 0);
            const bool hasCommittedCurveDepth =
                committedCurveDepth != nullptr &&
                committedCurveDepthSize == QSize(outputWidth, outputHeight) &&
                committedCurveDepth->size() == outputPixelCount;
            const bool hasCommittedInFrontCurveDepth =
                committedInFrontCurveDepth != nullptr &&
                committedCurveDepthSize == QSize(outputWidth, outputHeight) &&
                committedInFrontCurveDepth->size() == outputPixelCount;
            const auto visibleCurveDepthAt = [&](int x, int y,
                                                  bool inFrontDomain) {
                const bool hasCurveDepth = inFrontDomain
                    ? hasCommittedInFrontCurveDepth : hasCommittedCurveDepth;
                if (!hasCurveDepth) {
                    return std::numeric_limits<float>::infinity();
                }
                const QVector<float> *curveDepths = inFrontDomain
                    ? committedInFrontCurveDepth : committedCurveDepth;
                const float depth = (*curveDepths)[
                    qsizetype(y) * outputWidth + x];
                const int depthX = std::clamp(
                    static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                    0, width - 1);
                const int depthY = std::clamp(
                    static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
                    0, height - 1);
                const int depthIndex = depthY * width + depthX;
                const bool hasSurface = inFrontDomain
                    ? inFrontSamples[depthIndex].objectId != 0
                    : std::isfinite(regularDofDepth[depthIndex]);
                const float surfaceDepth = inFrontDomain
                    ? inFrontSamples[depthIndex].depth
                    : regularDofDepth[depthIndex];
                if (!std::isfinite(depth) || depth <= 0.0f ||
                    (hasSurface && depth > surfaceDepth + 1.0e-4f)) {
                    return std::numeric_limits<float>::infinity();
                }
                return depth;
            };
            const auto visibleSceneDepthAt = [&](int x, int y) {
                const int depthX = std::clamp(
                    static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                    0, width - 1);
                const int depthY = std::clamp(
                    static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
                    0, height - 1);
                const int depthIndex = depthY * width + depthX;
                const float inFrontCurveDepth =
                    visibleCurveDepthAt(x, y, true);
                if (std::isfinite(inFrontCurveDepth)) {
                    return inFrontCurveDepth;
                }
                if (inFrontSamples[depthIndex].objectId != 0) {
                    return inFrontSamples[depthIndex].depth;
                }
                const float regularCurveDepth =
                    visibleCurveDepthAt(x, y, false);
                if (std::isfinite(regularCurveDepth)) {
                    return regularCurveDepth;
                }
                return regularDofDepth[depthIndex];
            };
            for (int y = 0; y < outputHeight; ++y) {
                const int depthY = std::clamp(
                    static_cast<int>(std::floor((y + 0.5) / colorDpr * dpr)),
                    0, height - 1);
                for (int x = 0; x < outputWidth; ++x) {
                    const int depthX = std::clamp(
                        static_cast<int>(std::floor((x + 0.5) / colorDpr * dpr)),
                        0, width - 1);
                    const int depthIndex = depthY * width + depthX;
                    const bool hasRegularSurfaceDepth = std::isfinite(
                        regularDofDepth[depthIndex]);
                    if (hasRegularSurfaceDepth ||
                        std::isfinite(visibleCurveDepthAt(x, y, false)) ||
                        inFrontSamples[depthIndex].objectId != 0 ||
                        std::isfinite(visibleCurveDepthAt(x, y, true))) {
                        dofCoverage[qsizetype(y) * outputWidth + x] = 1;
                    }
                }
            }
            const int dilationX = std::min(
                qCeil(100.0), outputWidth);
            const int dilationY = std::min(
                qCeil(100.0), outputHeight);
            for (int y = 0; y < outputHeight; ++y) {
                int covered = 0;
                for (int x = 0; x <= dilationX && x < outputWidth; ++x) {
                    if (dofCoverage[qsizetype(y) * outputWidth + x] != 0)
                        ++covered;
                }
                for (int x = 0; x < outputWidth; ++x) {
                    horizontalCoverage[qsizetype(y) * outputWidth + x] =
                        covered > 0 ? 1 : 0;
                    const int removeX = x - dilationX;
                    const int addX = x + dilationX + 1;
                    if (removeX >= 0 && dofCoverage[
                            qsizetype(y) * outputWidth + removeX] != 0)
                        --covered;
                    if (addX < outputWidth && dofCoverage[
                            qsizetype(y) * outputWidth + addX] != 0)
                        ++covered;
                }
            }
            for (int x = 0; x < outputWidth; ++x) {
                int covered = 0;
                for (int y = 0; y <= dilationY && y < outputHeight; ++y) {
                    if (horizontalCoverage[qsizetype(y) * outputWidth + x] != 0)
                        ++covered;
                }
                for (int y = 0; y < outputHeight; ++y) {
                    dofCoverage[qsizetype(y) * outputWidth + x] =
                        covered > 0 ? 1 : 0;
                    const int removeY = y - dilationY;
                    const int addY = y + dilationY + 1;
                    if (removeY >= 0 && horizontalCoverage[
                            qsizetype(removeY) * outputWidth + x] != 0)
                        --covered;
                    if (addY < outputHeight && horizontalCoverage[
                            qsizetype(addY) * outputWidth + x] != 0)
                        ++covered;
                }
            }
            quint64 dofCandidatePixels = 0;
            for (quint8 candidate : dofCoverage)
                dofCandidatePixels += candidate != 0;
            const int dofTapCount = rasterWorkbenchDofTapCountForBudget(
                dofCandidatePixels);

            struct FilteredDofPixel {
                LinearGatherPixel color;
                float nearCoc = 0.0f;
                float farCoc = 0.0f;
            };
            struct DofMipLevel {
                int width = 1;
                int height = 1;
                std::vector<FilteredDofPixel> pixels;
            };
            std::array<DofMipLevel, 3> dofMips;
            dofMips[0].width = std::max(outputWidth / 2, 1);
            dofMips[0].height = std::max(outputHeight / 2, 1);
            dofMips[0].pixels.resize(
                static_cast<std::size_t>(dofMips[0].width) *
                dofMips[0].height);
            const auto sourceColorAt = [&](int x, int y) {
                if (sceneLinearDofActive) {
                    const RasterWorkbenchLinearPixel &pixel =
                        surfaceLayers->sceneLinearColor[
                            static_cast<std::size_t>(y) * outputWidth + x];
                    return LinearGatherPixel{
                        pixel.red, pixel.green, pixel.blue, pixel.alpha};
                }
                const QRgb pixel = source.pixel(x, y);
                const float alpha = qAlpha(pixel) / 255.0f;
                return LinearGatherPixel{
                    decodeDisplayChannel(qRed(pixel)) * alpha,
                    decodeDisplayChannel(qGreen(pixel)) * alpha,
                    decodeDisplayChannel(qBlue(pixel)) * alpha, alpha};
            };
            const auto sourceCocAtPixel = [&](int x, int y) {
                const float visibleDepth = visibleSceneDepthAt(x, y);
                const qreal z = std::isfinite(visibleDepth)
                    ? visibleDepth : cameraOptics.clipEnd;
                if (!std::isfinite(z) || z <= 0.0) return 0.0f;
                const qreal coc = 0.5 *
                    (focal * focal /
                     (fStop * std::abs(focus - focal))) *
                    ((focus - z) / std::max<qreal>(z, 1.0e-6)) *
                    outputWidth / sensorWidth;
                return static_cast<float>(std::clamp(coc, -100.0, 100.0));
            };
            std::vector<float> sourceCocs(
                static_cast<std::size_t>(outputWidth) * outputHeight);
            for (int y = 0; y < outputHeight; ++y) {
                for (int x = 0; x < outputWidth; ++x) {
                    sourceCocs[static_cast<std::size_t>(y) * outputWidth + x] =
                        sourceCocAtPixel(x, y);
                }
            }
            const auto sourceCocAt = [&](qreal x, qreal y) {
                // Blender linearly filters the signed CoC texture at each
                // fractional gather coordinate. Interpolate the CPU CoC
                // samples at output-pixel centers to preserve that behavior.
                const qreal pixelX = std::clamp<qreal>(
                    x, 0.0, outputWidth - 1.0);
                const qreal pixelY = std::clamp<qreal>(
                    y, 0.0, outputHeight - 1.0);
                const int x0 = static_cast<int>(std::floor(pixelX));
                const int y0 = static_cast<int>(std::floor(pixelY));
                const int x1 = std::min(x0 + 1, outputWidth - 1);
                const int y1 = std::min(y0 + 1, outputHeight - 1);
                const qreal tx = pixelX - x0;
                const qreal ty = pixelY - y0;
                const auto at = [&](int px, int py) {
                    return sourceCocs[
                        static_cast<std::size_t>(py) * outputWidth + px];
                };
                const qreal top = at(x0, y0) * (1.0 - tx) +
                    at(x1, y0) * tx;
                const qreal bottom = at(x0, y1) * (1.0 - tx) +
                    at(x1, y1) * tx;
                return static_cast<float>(top * (1.0 - ty) + bottom * ty);
            };
            for (int y = 0; y < dofMips[0].height; ++y) {
                for (int x = 0; x < dofMips[0].width; ++x) {
                    const int sourceX = std::min(x * 2, outputWidth - 1);
                    const int sourceY = std::min(y * 2, outputHeight - 1);
                    const std::array<QPoint, 4> offsets{{
                        QPoint(0, 0), QPoint(1, 0),
                        QPoint(0, 1), QPoint(1, 1)}};
                    std::array<float, 4> cocs{};
                    std::array<LinearGatherPixel, 4> colors{};
                    for (std::size_t i = 0; i < offsets.size(); ++i) {
                        const int px = std::min(
                            sourceX + offsets[i].x(), outputWidth - 1);
                        const int py = std::min(
                            sourceY + offsets[i].y(), outputHeight - 1);
                        cocs[i] = sourceCocAt(px, py);
                        colors[i] = sourceColorAt(px, py);
                    }
                    const RasterWorkbenchDofPrefilter filtered =
                        rasterWorkbenchDofPrefilter2x2(cocs);
                    qreal weightSum = 0.0;
                    LinearGatherPixel result;
                    for (std::size_t i = 0; i < colors.size(); ++i) {
                        const qreal weight = filtered.colorWeights[i];
                        result.red += colors[i].red * weight;
                        result.green += colors[i].green * weight;
                        result.blue += colors[i].blue * weight;
                        result.alpha += colors[i].alpha * weight;
                        weightSum += weight;
                    }
                    if (weightSum <= 0.0) {
                        for (const LinearGatherPixel &color : colors) {
                            result.red += color.red * 0.25f;
                            result.green += color.green * 0.25f;
                            result.blue += color.blue * 0.25f;
                            result.alpha += color.alpha * 0.25f;
                        }
                    } else {
                        result.red /= weightSum;
                        result.green /= weightSum;
                        result.blue /= weightSum;
                        result.alpha /= weightSum;
                    }
                    // Blender clamps its half-resolution DOF color target
                    // before constructing the remaining mip levels.
                    result.red = std::clamp(result.red, 0.0f, 3.0f);
                    result.green = std::clamp(result.green, 0.0f, 3.0f);
                    result.blue = std::clamp(result.blue, 0.0f, 3.0f);
                    dofMips[0].pixels[
                        static_cast<std::size_t>(y) * dofMips[0].width + x] = {
                            result, filtered.nearCoc, filtered.farCoc};
                }
            }

            // Blender builds quarter- and eighth-resolution CoC-aware mip
            // levels from the preceding level. Their dimensions and total
            // storage remain bounded by the half-resolution source pyramid.
            for (std::size_t level = 1; level < dofMips.size(); ++level) {
                DofMipLevel &destination = dofMips[level];
                const DofMipLevel &previous = dofMips[level - 1];
                destination.width = std::max(previous.width / 2, 1);
                destination.height = std::max(previous.height / 2, 1);
                destination.pixels.resize(
                    static_cast<std::size_t>(destination.width) *
                    destination.height);
                for (int y = 0; y < destination.height; ++y) {
                    for (int x = 0; x < destination.width; ++x) {
                        const int sourceX = x * 2;
                        const int sourceY = y * 2;
                        const std::array<const FilteredDofPixel *, 4> inputs{{
                            &previous.pixels[static_cast<std::size_t>(sourceY) *
                                                 previous.width + sourceX],
                            &previous.pixels[static_cast<std::size_t>(sourceY) *
                                                 previous.width + std::min(sourceX + 1, previous.width - 1)],
                            &previous.pixels[static_cast<std::size_t>(std::min(sourceY + 1, previous.height - 1)) *
                                                 previous.width + sourceX],
                            &previous.pixels[static_cast<std::size_t>(std::min(sourceY + 1, previous.height - 1)) *
                                                 previous.width + std::min(sourceX + 1, previous.width - 1)]}};
                        std::array<float, 4> nearCocs{};
                        std::array<float, 4> farCocs{};
                        for (std::size_t i = 0; i < inputs.size(); ++i) {
                            nearCocs[i] = inputs[i]->nearCoc;
                            farCocs[i] = inputs[i]->farCoc;
                        }
                        const RasterWorkbenchDofPrefilter filtered =
                            rasterWorkbenchDofPrefilter2x2(nearCocs, farCocs);
                        LinearGatherPixel color;
                        qreal weightSum = 0.0;
                        for (std::size_t i = 0; i < inputs.size(); ++i) {
                            const qreal weight = filtered.colorWeights[i];
                            color.red += inputs[i]->color.red * weight;
                            color.green += inputs[i]->color.green * weight;
                            color.blue += inputs[i]->color.blue * weight;
                            color.alpha += inputs[i]->color.alpha * weight;
                            weightSum += weight;
                        }
                        if (weightSum > 0.0) {
                            color.red /= weightSum;
                            color.green /= weightSum;
                            color.blue /= weightSum;
                            color.alpha /= weightSum;
                        } else {
                            for (const FilteredDofPixel *input : inputs) {
                                color.red += input->color.red * 0.25f;
                                color.green += input->color.green * 0.25f;
                                color.blue += input->color.blue * 0.25f;
                                color.alpha += input->color.alpha * 0.25f;
                            }
                        }
                        destination.pixels[
                            static_cast<std::size_t>(y) * destination.width + x] = {
                                color, filtered.nearCoc, filtered.farCoc};
                    }
                }
            }

            const auto sampleDofMip = [&](qreal sampleX, qreal sampleY,
                                          float lod) {
                const int lowerLevel = std::clamp(
                    static_cast<int>(std::floor(lod)), 0, 2);
                const int upperLevel = std::min(lowerLevel + 1, 2);
                const qreal blend = lod - lowerLevel;
                const auto bilinear = [&](int level) {
                    const DofMipLevel &mip = dofMips[static_cast<std::size_t>(level)];
                    const qreal mipX = std::clamp(
                        (sampleX + 0.5) * mip.width / outputWidth - 0.5,
                        0.0, qreal(mip.width - 1));
                    const qreal mipY = std::clamp(
                        (sampleY + 0.5) * mip.height / outputHeight - 0.5,
                        0.0, qreal(mip.height - 1));
                    const int x0 = std::clamp(static_cast<int>(std::floor(mipX)), 0, mip.width - 1);
                    const int y0 = std::clamp(static_cast<int>(std::floor(mipY)), 0, mip.height - 1);
                    const int x1 = std::min(x0 + 1, mip.width - 1);
                    const int y1 = std::min(y0 + 1, mip.height - 1);
                    const qreal tx = std::clamp(mipX - std::floor(mipX), 0.0, 1.0);
                    const qreal ty = std::clamp(mipY - std::floor(mipY), 0.0, 1.0);
                    const auto at = [&](int px, int py) -> const FilteredDofPixel & {
                        return mip.pixels[static_cast<std::size_t>(py) * mip.width + px];
                    };
                    const FilteredDofPixel &p00 = at(x0, y0);
                    const FilteredDofPixel &p10 = at(x1, y0);
                    const FilteredDofPixel &p01 = at(x0, y1);
                    const FilteredDofPixel &p11 = at(x1, y1);
                    const auto blendValue = [&](float a, float b, float c, float d) {
                        return static_cast<float>((a * (1.0 - tx) + b * tx) * (1.0 - ty) +
                                                   (c * (1.0 - tx) + d * tx) * ty);
                    };
                    FilteredDofPixel value;
                    value.color = {
                        blendValue(p00.color.red, p10.color.red, p01.color.red, p11.color.red),
                        blendValue(p00.color.green, p10.color.green, p01.color.green, p11.color.green),
                        blendValue(p00.color.blue, p10.color.blue, p01.color.blue, p11.color.blue),
                        blendValue(p00.color.alpha, p10.color.alpha, p01.color.alpha, p11.color.alpha)};
                    value.nearCoc = blendValue(p00.nearCoc, p10.nearCoc, p01.nearCoc, p11.nearCoc);
                    value.farCoc = blendValue(p00.farCoc, p10.farCoc, p01.farCoc, p11.farCoc);
                    return value;
                };
                const FilteredDofPixel a = bilinear(lowerLevel);
                if (upperLevel == lowerLevel) return a;
                const FilteredDofPixel b = bilinear(upperLevel);
                const auto mix = [blend](float x, float y) {
                    return static_cast<float>(x * (1.0 - blend) + y * blend);
                };
                return FilteredDofPixel{
                    {mix(a.color.red, b.color.red), mix(a.color.green, b.color.green),
                     mix(a.color.blue, b.color.blue), mix(a.color.alpha, b.color.alpha)},
                    mix(a.nearCoc, b.nearCoc), mix(a.farCoc, b.farCoc)};
            };

            for (int y = 0; y < outputHeight; ++y) {
                QRgb *out = reinterpret_cast<QRgb *>(gatheredResult.scanLine(y));
                for (int x = 0; x < outputWidth; ++x) {
                    const float visibleDepth = visibleSceneDepthAt(x, y);
                    const bool backgroundCenter =
                        !std::isfinite(visibleDepth);
                    const qsizetype pixelIndex =
                        qsizetype(y) * outputWidth + x;
                    if (backgroundCenter && dofCoverage[pixelIndex] == 0)
                        continue;
                    // The GPU resolve runs across the full target. A clear
                    // depth sample represents the far plane, allowing a
                    // nearby out-of-focus foreground surface to contribute
                    // blur beyond its silhouette.
                    const qreal z = backgroundCenter
                        ? cameraOptics.clipEnd : visibleDepth;
                    if (!std::isfinite(z) || z <= 0.0) continue;
                    // Blender's aperture radius is 0.5 * focal / f-stop;
                    // multiplying by focal / focus-distance produces this
                    // factor for both perspective and orthographic view depth.
                    const qreal cocMm = 0.5 * std::abs(
                        (focal * focal /
                         (fStop * std::abs(focus - focal))) *
                        ((z - focus) / std::max<qreal>(z, 1.0e-6)));
                    const qreal radius = std::clamp(
                        cocMm * outputWidth / sensorWidth, 0.0, 100.0);
                    const qreal mix = rasterWorkbenchDofResolveBlend(
                        static_cast<float>(radius));
                    if (mix <= 0.0) continue;
                    dofMixes[pixelIndex] = static_cast<float>(mix);
                    const QPointF gatherOriginOffset =
                        rasterWorkbenchDofGatherOriginOffset(
                            x, y, outputWidth, outputHeight,
                            static_cast<float>(radius));
                    qreal red = 0.0, green = 0.0, blue = 0.0, alpha = 0.0;
                    qreal totalWeight = 0.0;
                    for (int tapIndex = 0; tapIndex < dofTapCount; ++tapIndex) {
                        // Evenly subsample Blender's ordered kernel when a
                        // dense software resolve would exceed its fixed work
                        // budget. Sparse regions retain all 49 samples.
                        const std::size_t kernelIndex = dofTapCount == 1
                            ? 0
                            : static_cast<std::size_t>(
                                  tapIndex * (dofKernel.size() - 1) /
                                  (dofTapCount - 1));
                        const RasterWorkbenchDofTap &tap = dofKernel[kernelIndex];
                        const qreal sx = x + gatherOriginOffset.x() +
                                         tap.position.x() * radius;
                        const qreal sy = y + gatherOriginOffset.y() +
                                         tap.position.y() * radius;
                        // Blender chooses the source-color mip from its
                        // prepared half-resolution CoC texture. Keep the
                        // QPainter gather on that same CoC signal instead of
                        // re-reading full-resolution depth at the tap.
                        const FilteredDofPixel tapCoC =
                            sampleDofMip(sx, sy, 0.0f);
                        const float signedTapCoc =
                            tapCoC.nearCoc > tapCoC.farCoc
                                ? tapCoC.nearCoc : -tapCoC.farCoc;
                        const FilteredDofPixel sample = sampleDofMip(
                            sx, sy, rasterWorkbenchDofMipLod(
                                signedTapCoc,
                                static_cast<float>(radius)));
                        const float signedCoc = sample.nearCoc > sample.farCoc
                            ? sample.nearCoc : -sample.farCoc;
                        const float weight = rasterWorkbenchDofGatherWeight(
                            signedCoc,
                            static_cast<float>(radius), tap.radius);
                        red += sample.color.red * weight;
                        green += sample.color.green * weight;
                        blue += sample.color.blue * weight;
                        alpha += sample.color.alpha * weight;
                        totalWeight += weight;
                    }
                    if (totalWeight <= 0.0) {
                        const FilteredDofPixel center =
                            sampleDofMip(x, y, 0.0f);
                        if (sceneLinearDofActive) {
                            const std::size_t pixelIndex =
                                static_cast<std::size_t>(y) * outputWidth + x;
                            gatheredLinear[pixelIndex] = {
                                center.color.red, center.color.green,
                                center.color.blue, center.color.alpha};
                        } else {
                            out[x] = qRgba(
                                qRound(center.color.red),
                                qRound(center.color.green),
                                qRound(center.color.blue),
                                qRound(center.color.alpha * 255.0f));
                        }
                        continue;
                    }
                    if (sceneLinearDofActive) {
                        const std::size_t pixelIndex =
                            static_cast<std::size_t>(y) * outputWidth + x;
                        gatheredLinear[pixelIndex] = {
                            static_cast<float>(red / totalWeight),
                            static_cast<float>(green / totalWeight),
                            static_cast<float>(blue / totalWeight),
                            static_cast<float>(alpha / totalWeight)};
                    } else {
                        out[x] = qRgba(
                            qRound(red / totalWeight),
                            qRound(green / totalWeight),
                            qRound(blue / totalWeight),
                            qRound(alpha / totalWeight * 255.0));
                    }
                }
            }
            // Blender's 3x3 median operates on the half-resolution blur
            // target. Keep that stage at half resolution, then bilinearly
            // sample it during the full-resolution resolve.
            const int medianWidth = std::max(outputWidth / 2, 1);
            const int medianHeight = std::max(outputHeight / 2, 1);
            const auto medianCocRadiusAt = [&](int x, int y) {
                const FilteredDofPixel &coc = dofMips[0].pixels[
                    static_cast<std::size_t>(y) * dofMips[0].width + x];
                return std::clamp(
                    std::max(coc.nearCoc, coc.farCoc) - 9.0f, 0.0f, 1.0f);
            };
            QImage filteredGather(medianWidth, medianHeight,
                                  QImage::Format_ARGB32);
            filteredGather.fill(Qt::transparent);
            const auto sampleGatherLinear = [&](qreal sampleX,
                                                qreal sampleY) {
                sampleX = std::clamp(sampleX, 0.0,
                                     qreal(outputWidth - 1));
                sampleY = std::clamp(sampleY, 0.0,
                                     qreal(outputHeight - 1));
                const int x0 = static_cast<int>(std::floor(sampleX));
                const int y0 = static_cast<int>(std::floor(sampleY));
                const int x1 = std::min(x0 + 1, outputWidth - 1);
                const int y1 = std::min(y0 + 1, outputHeight - 1);
                const qreal tx = sampleX - x0;
                const qreal ty = sampleY - y0;
                const auto interpolate = [&](auto channel) {
                    const qreal top =
                        channel(gatheredResult.pixel(x0, y0)) * (1.0 - tx) +
                        channel(gatheredResult.pixel(x1, y0)) * tx;
                    const qreal bottom =
                        channel(gatheredResult.pixel(x0, y1)) * (1.0 - tx) +
                        channel(gatheredResult.pixel(x1, y1)) * tx;
                    return qRound(top * (1.0 - ty) + bottom * ty);
                };
                return qRgba(interpolate([](QRgb pixel) { return qRed(pixel); }),
                             interpolate([](QRgb pixel) { return qGreen(pixel); }),
                             interpolate([](QRgb pixel) { return qBlue(pixel); }),
                             interpolate([](QRgb pixel) { return qAlpha(pixel); }));
            };
            const qreal medianScaleX =
                static_cast<qreal>(outputWidth) / medianWidth;
            const qreal medianScaleY =
                static_cast<qreal>(outputHeight) / medianHeight;
            for (int y = 0; y < medianHeight; ++y) {
                QRgb *out = reinterpret_cast<QRgb *>(filteredGather.scanLine(y));
                const qreal centerY = (y + 0.5) * medianScaleY - 0.5;
                for (int x = 0; x < medianWidth; ++x) {
                    const qreal centerX = (x + 0.5) * medianScaleX - 0.5;
                    const float cocRadius = medianCocRadiusAt(x, y);
                    if (cocRadius <= 0.0f) {
                        out[x] = sampleGatherLinear(centerX, centerY);
                        continue;
                    }
                    const qreal stepX = medianScaleX * cocRadius;
                    const qreal stepY = medianScaleY * cocRadius;
                    std::array<QRgb, 9> neighborhood{};
                    int sampleIndex = 0;
                    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                            neighborhood[sampleIndex++] = sampleGatherLinear(
                                centerX + offsetX * stepX,
                                centerY + offsetY * stepY);
                        }
                    }
                    out[x] = rasterWorkbenchDofMedian3x3(neighborhood);
                }
            }
            std::vector<LinearGatherPixel> filteredLinearGather(
                static_cast<std::size_t>(medianWidth) * medianHeight);
            if (sceneLinearDofActive) {
                const auto sampleLinearGather = [&](qreal sampleX,
                                                    qreal sampleY) {
                    sampleX = std::clamp(sampleX, 0.0,
                                         qreal(outputWidth - 1));
                    sampleY = std::clamp(sampleY, 0.0,
                                         qreal(outputHeight - 1));
                    const int x0 = static_cast<int>(std::floor(sampleX));
                    const int y0 = static_cast<int>(std::floor(sampleY));
                    const int x1 = std::min(x0 + 1, outputWidth - 1);
                    const int y1 = std::min(y0 + 1, outputHeight - 1);
                    const qreal tx = sampleX - x0;
                    const qreal ty = sampleY - y0;
                    const LinearGatherPixel &p00 = gatheredLinear[
                        static_cast<std::size_t>(y0) * outputWidth + x0];
                    const LinearGatherPixel &p10 = gatheredLinear[
                        static_cast<std::size_t>(y0) * outputWidth + x1];
                    const LinearGatherPixel &p01 = gatheredLinear[
                        static_cast<std::size_t>(y1) * outputWidth + x0];
                    const LinearGatherPixel &p11 = gatheredLinear[
                        static_cast<std::size_t>(y1) * outputWidth + x1];
                    const auto interpolate = [&](float LinearGatherPixel::*channel) {
                        const qreal top = p00.*channel * (1.0 - tx) +
                                          p10.*channel * tx;
                        const qreal bottom = p01.*channel * (1.0 - tx) +
                                             p11.*channel * tx;
                        return static_cast<float>(top * (1.0 - ty) +
                                                  bottom * ty);
                    };
                    return LinearGatherPixel{
                        interpolate(&LinearGatherPixel::red),
                        interpolate(&LinearGatherPixel::green),
                        interpolate(&LinearGatherPixel::blue),
                        interpolate(&LinearGatherPixel::alpha)};
                };
                const auto medianChannel = [](auto channel,
                                              const std::array<LinearGatherPixel, 9> &pixels) {
                    std::array<float, 9> values{};
                    for (std::size_t i = 0; i < values.size(); ++i)
                        values[i] = channel(pixels[i]);
                    std::nth_element(values.begin(), values.begin() + 4,
                                     values.end());
                    return values[4];
                };
                for (int y = 0; y < medianHeight; ++y) {
                    const qreal centerY = (y + 0.5) * medianScaleY - 0.5;
                    for (int x = 0; x < medianWidth; ++x) {
                        const qreal centerX =
                            (x + 0.5) * medianScaleX - 0.5;
                        const float cocRadius = medianCocRadiusAt(x, y);
                        const std::size_t targetIndex =
                            static_cast<std::size_t>(y) * medianWidth + x;
                        if (cocRadius <= 0.0f) {
                            filteredLinearGather[targetIndex] =
                                sampleLinearGather(centerX, centerY);
                            continue;
                        }
                        const qreal stepX = medianScaleX * cocRadius;
                        const qreal stepY = medianScaleY * cocRadius;
                        std::array<LinearGatherPixel, 9> neighborhood{};
                        int sampleIndex = 0;
                        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                                neighborhood[sampleIndex++] = sampleLinearGather(
                                    centerX + offsetX * stepX,
                                    centerY + offsetY * stepY);
                            }
                        }
                        filteredLinearGather[targetIndex] = {
                            medianChannel([](const auto &p) { return p.red; },
                                          neighborhood),
                            medianChannel([](const auto &p) { return p.green; },
                                          neighborhood),
                            medianChannel([](const auto &p) { return p.blue; },
                                          neighborhood),
                            medianChannel([](const auto &p) { return p.alpha; },
                                          neighborhood)};
                    }
                }
            }
            const auto filteredHalfCoordinate = [&](qreal sampleX,
                                                   qreal sampleY) {
                return QPointF(
                    std::clamp((sampleX + 0.5) * medianWidth / outputWidth - 0.5,
                               0.0, qreal(medianWidth - 1)),
                    std::clamp((sampleY + 0.5) * medianHeight / outputHeight - 0.5,
                               0.0, qreal(medianHeight - 1)));
            };
            const auto sampleFilteredGather = [&](qreal sampleX,
                                                  qreal sampleY) {
                const QPointF half = filteredHalfCoordinate(sampleX, sampleY);
                const int x0 = static_cast<int>(std::floor(half.x()));
                const int y0 = static_cast<int>(std::floor(half.y()));
                const int x1 = std::min(x0 + 1, medianWidth - 1);
                const int y1 = std::min(y0 + 1, medianHeight - 1);
                const qreal tx = half.x() - x0;
                const qreal ty = half.y() - y0;
                const auto interpolate = [&](auto channel) {
                    const qreal top =
                        channel(filteredGather.pixel(x0, y0)) * (1.0 - tx) +
                        channel(filteredGather.pixel(x1, y0)) * tx;
                    const qreal bottom =
                        channel(filteredGather.pixel(x0, y1)) * (1.0 - tx) +
                        channel(filteredGather.pixel(x1, y1)) * tx;
                    return qRound(top * (1.0 - ty) + bottom * ty);
                };
                return qRgba(interpolate([](QRgb pixel) { return qRed(pixel); }),
                             interpolate([](QRgb pixel) { return qGreen(pixel); }),
                             interpolate([](QRgb pixel) { return qBlue(pixel); }),
                             interpolate([](QRgb pixel) { return qAlpha(pixel); }));
            };
            const auto sampleFilteredLinearGather = [&](qreal sampleX,
                                                        qreal sampleY) {
                const QPointF half = filteredHalfCoordinate(sampleX, sampleY);
                const int x0 = static_cast<int>(std::floor(half.x()));
                const int y0 = static_cast<int>(std::floor(half.y()));
                const int x1 = std::min(x0 + 1, medianWidth - 1);
                const int y1 = std::min(y0 + 1, medianHeight - 1);
                const qreal tx = half.x() - x0;
                const qreal ty = half.y() - y0;
                const auto &p00 = filteredLinearGather[
                    static_cast<std::size_t>(y0) * medianWidth + x0];
                const auto &p10 = filteredLinearGather[
                    static_cast<std::size_t>(y0) * medianWidth + x1];
                const auto &p01 = filteredLinearGather[
                    static_cast<std::size_t>(y1) * medianWidth + x0];
                const auto &p11 = filteredLinearGather[
                    static_cast<std::size_t>(y1) * medianWidth + x1];
                const auto blend = [tx, ty](float a, float b, float c,
                                            float d) {
                    return static_cast<float>(
                        (a * (1.0 - tx) + b * tx) * (1.0 - ty) +
                        (c * (1.0 - tx) + d * tx) * ty);
                };
                return LinearGatherPixel{
                    blend(p00.red, p10.red, p01.red, p11.red),
                    blend(p00.green, p10.green, p01.green, p11.green),
                    blend(p00.blue, p10.blue, p01.blue, p11.blue),
                    blend(p00.alpha, p10.alpha, p01.alpha, p11.alpha)};
            };
            QImage dofResult = source.copy();
            for (int y = 0; y < outputHeight; ++y) {
                QRgb *out = reinterpret_cast<QRgb *>(dofResult.scanLine(y));
                for (int x = 0; x < outputWidth; ++x) {
                    const qsizetype pixelIndex =
                        qsizetype(y) * outputWidth + x;
                    const float mix = dofMixes[pixelIndex];
                    if (mix <= 0.0f) continue;
                    if (sceneLinearDofActive) {
                        RasterWorkbenchLinearPixel &original =
                            surfaceLayers->sceneLinearColor[
                                static_cast<std::size_t>(pixelIndex)];
                        const LinearGatherPixel blurred =
                            sampleFilteredLinearGather(x, y);
                        original.red = original.red * (1.0f - mix) +
                            blurred.red * mix;
                        original.green = original.green * (1.0f - mix) +
                            blurred.green * mix;
                        original.blue = original.blue * (1.0f - mix) +
                            blurred.blue * mix;
                        original.alpha = original.alpha * (1.0f - mix) +
                            blurred.alpha * mix;
                        continue;
                    }
                    const QRgb original = source.pixel(x, y);
                    const QRgb blurred = sampleFilteredGather(x, y);
                    const float originalAlpha = qAlpha(original) / 255.0f;
                    const float blurredAlpha = qAlpha(blurred) / 255.0f;
                    const float outputAlpha = originalAlpha * (1.0f - mix) +
                                              blurredAlpha * mix;
                    const auto resolveChannel = [&](int originalChannel,
                                                    int blurredPremultiplied) {
                        const float originalPremultiplied =
                            decodeDisplayChannel(originalChannel) / 255.0f *
                            originalAlpha;
                        const float blurredLinear =
                            blurredPremultiplied / 255.0f;
                        const float outputPremultiplied =
                            originalPremultiplied * (1.0f - mix) +
                            blurredLinear * mix;
                        const float outputLinear = outputAlpha > 1.0e-6f
                            ? outputPremultiplied / outputAlpha : 0.0f;
                        return std::clamp(qRound(
                            workbenchSceneLinearToSrgb(outputLinear) * 255.0f),
                            0, 255);
                    };
                    out[x] = qRgba(
                        resolveChannel(qRed(original), qRed(blurred)),
                        resolveChannel(qGreen(original), qGreen(blurred)),
                        resolveChannel(qBlue(original), qBlue(blurred)),
                        qRound(outputAlpha * 255.0f));
                }
            }
            if (!sceneLinearDofActive)
                *colorImage = dofResult.convertToFormat(colorImage->format());
        }
    }
    if (surfaceLayers != nullptr && surfaceLayers->sceneLinearDofPending) {
        (void)displayRetainedSceneLinear();
        surfaceLayers->sceneLinearDofPending = false;
    }
    // Selection contours belong to the editor overlay. Keep them sharp by
    // compositing them after the Workbench scene's depth-of-field resolve.
    applyOutline(false, selectedOutlineEnabled);
}

} // namespace classiCAD
