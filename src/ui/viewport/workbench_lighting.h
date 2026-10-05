/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The studio-light shading equations are adapted from Blender Workbench's
 * workbench_world_light.bsl.hh (GPL-2.0-or-later). The default light rig below
 * matches Blender's factory-startup "Default" studio light. */
#pragma once

#include <QFile>
#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector2D>
#include <QVector3D>
#include <QVector>
#include <QtEndian>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace classiCAD {

struct WorkbenchStudioLight {
    QVector3D direction;
    QVector3D diffuseColor;
    QVector3D specularColor;
    float wrap = 0.0f;
    bool enabled = false;
};

struct WorkbenchStudioLighting {
    std::array<WorkbenchStudioLight, 4> lights;
    QVector3D ambientColor;
    bool useSpecular = true;
};

inline const WorkbenchStudioLighting &defaultWorkbenchStudioLighting();
inline QVector3D workbenchNormalize(const QVector3D &value,
                                   const QVector3D &fallback);

inline float workbenchSrgbToSceneLinear(float value)
{
    const float channel = std::clamp(value, 0.0f, 1.0f);
    return channel <= 0.04045f
               ? channel / 12.92f
               : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

inline float workbenchSceneLinearToSrgb(float value)
{
    const float channel = std::max(value, 0.0f);
    return channel <= 0.0031308f
               ? channel * 12.92f
               : 1.055f * std::pow(channel, 1.0f / 2.4f) - 0.055f;
}

inline QVector3D workbenchSrgbToSceneLinear(const QVector3D &color)
{
    return {workbenchSrgbToSceneLinear(color.x()),
            workbenchSrgbToSceneLinear(color.y()),
            workbenchSrgbToSceneLinear(color.z())};
}

inline QVector3D workbenchSceneLinearToSrgb(const QVector3D &color)
{
    return {workbenchSceneLinearToSrgb(color.x()),
            workbenchSceneLinearToSrgb(color.y()),
            workbenchSceneLinearToSrgb(color.z())};
}

inline constexpr int workbenchAgxDisplayLutSize = 65;
inline constexpr float workbenchAgxDisplayLutMaximum = 2.0f;

inline const QVector<float> &workbenchAgxDisplayLutData()
{
    static const QVector<float> values = [] {
        QFile file(QStringLiteral(
            ":/workbench-lighting/color/agx_srgb_default_65.bin"));
        if (!file.open(QIODevice::ReadOnly)) {
            return QVector<float>{};
        }
        const QByteArray bytes = file.readAll();
        constexpr qsizetype valueCount =
            workbenchAgxDisplayLutSize * workbenchAgxDisplayLutSize *
            workbenchAgxDisplayLutSize * 3;
        constexpr qsizetype byteCount = valueCount * sizeof(quint32);
        if (bytes.size() != byteCount) {
            return QVector<float>{};
        }

        QVector<float> result(valueCount);
        const auto *source = reinterpret_cast<const uchar *>(bytes.constData());
        for (qsizetype index = 0; index < valueCount; ++index) {
            const quint32 bits = qFromLittleEndian<quint32>(
                source + index * sizeof(quint32));
            std::memcpy(&result[index], &bits, sizeof(bits));
        }
        return result;
    }();
    return values;
}

inline QVector3D workbenchSceneLinearToAgxSrgb(const QVector3D &color)
{
    const QVector<float> &lut = workbenchAgxDisplayLutData();
    if (lut.isEmpty()) {
        return workbenchSceneLinearToSrgb(color);
    }

    const float maximumIndex = workbenchAgxDisplayLutSize - 1.0f;
    const auto coordinate = [maximumIndex](float value) {
        return std::clamp(value, 0.0f, workbenchAgxDisplayLutMaximum) *
               maximumIndex / workbenchAgxDisplayLutMaximum;
    };
    const float rx = coordinate(color.x());
    const float gy = coordinate(color.y());
    const float bz = coordinate(color.z());
    const int x0 = static_cast<int>(rx);
    const int y0 = static_cast<int>(gy);
    const int z0 = static_cast<int>(bz);
    const int x1 = std::min(x0 + 1, workbenchAgxDisplayLutSize - 1);
    const int y1 = std::min(y0 + 1, workbenchAgxDisplayLutSize - 1);
    const int z1 = std::min(z0 + 1, workbenchAgxDisplayLutSize - 1);
    const float tx = rx - x0;
    const float ty = gy - y0;
    const float tz = bz - z0;
    const auto sample = [&lut](int red, int green, int blue) {
        const qsizetype index =
            ((static_cast<qsizetype>(blue) * workbenchAgxDisplayLutSize +
              green) * workbenchAgxDisplayLutSize + red) * 3;
        return QVector3D(lut[index], lut[index + 1], lut[index + 2]);
    };
    const auto interpolate = [](const QVector3D &first,
                                const QVector3D &second,
                                float fraction) {
        return first * (1.0f - fraction) + second * fraction;
    };
    const QVector3D lowerBlue = interpolate(
        interpolate(sample(x0, y0, z0), sample(x1, y0, z0), tx),
        interpolate(sample(x0, y1, z0), sample(x1, y1, z0), tx), ty);
    const QVector3D upperBlue = interpolate(
        interpolate(sample(x0, y0, z1), sample(x1, y0, z1), tx),
        interpolate(sample(x0, y1, z1), sample(x1, y1, z1), tx), ty);
    return interpolate(lowerBlue, upperBlue, tz);
}

inline QVector3D workbenchDefaultSolidMaterialDiffuseColor()
{
    // Blender's factory-startup material uses diffuse_color = (0.8, 0.8, 0.8).
    // This value is already scene-linear; it must not be decoded as sRGB.
    return {0.8f, 0.8f, 0.8f};
}

inline WorkbenchStudioLighting workbenchStudioLightingPreset(
    const QString &presetName)
{
    if (presetName.compare(QStringLiteral("Default"),
                           Qt::CaseInsensitive) == 0) {
        return defaultWorkbenchStudioLighting();
    }

    static QHash<QString, WorkbenchStudioLighting> cache;
    const QString key = presetName.toLower();
    const auto cached = cache.constFind(key);
    if (cached != cache.cend()) {
        return cached.value();
    }

    WorkbenchStudioLighting lighting{};
    QFile file(QStringLiteral(":/workbench-lighting/studio/%1.sl")
                   .arg(presetName.toLower()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return defaultWorkbenchStudioLighting();
    }

    const QRegularExpression lightExpression(
        QStringLiteral(R"(^light\[(\d+)\]\.(flag|smooth|col|spec|vec)(?:\.([xyz]))?$)"));
    const QRegularExpression ambientExpression(
        QStringLiteral(R"(^light_ambient\.([xyz])$)"));
    const auto assignComponent = [](QVector3D *vector,
                                    QChar component,
                                    float value) {
        if (component == QLatin1Char('x')) {
            vector->setX(value);
        } else if (component == QLatin1Char('y')) {
            vector->setY(value);
        } else if (component == QLatin1Char('z')) {
            vector->setZ(value);
        }
    };

    while (!file.atEnd()) {
        const QStringList fields = QString::fromUtf8(file.readLine())
                                       .trimmed()
                                       .split(QRegularExpression(QStringLiteral("\\s+")),
                                              Qt::SkipEmptyParts);
        if (fields.size() != 2) {
            continue;
        }
        bool ok = false;
        const float value = fields[1].toFloat(&ok);
        if (!ok || !std::isfinite(value)) {
            continue;
        }
        const QRegularExpressionMatch ambientMatch =
            ambientExpression.match(fields[0]);
        if (ambientMatch.hasMatch()) {
            assignComponent(&lighting.ambientColor,
                            ambientMatch.captured(1).at(0), value);
            continue;
        }
        const QRegularExpressionMatch match = lightExpression.match(fields[0]);
        if (!match.hasMatch()) {
            continue;
        }
        const int index = match.captured(1).toInt();
        if (index < 0 || index >= static_cast<int>(lighting.lights.size())) {
            continue;
        }
        WorkbenchStudioLight &light = lighting.lights[static_cast<size_t>(index)];
        const QString property = match.captured(2);
        if (property == QStringLiteral("flag")) {
            light.enabled = value != 0.0f;
        } else if (property == QStringLiteral("smooth")) {
            light.wrap = value;
        } else if (match.captured(3).size() == 1) {
            const QChar component = match.captured(3).at(0);
            if (property == QStringLiteral("col")) {
                assignComponent(&light.diffuseColor, component, value);
            } else if (property == QStringLiteral("spec")) {
                assignComponent(&light.specularColor, component, value);
            } else if (property == QStringLiteral("vec")) {
                assignComponent(&light.direction, component, value);
            }
        }
    }

    lighting.useSpecular = true;
    cache.insert(key, lighting);
    return lighting;
}

inline WorkbenchStudioLighting workbenchStudioLightingForView(
    const WorkbenchStudioLighting &source,
    int rotationDegrees,
    bool worldSpace,
    const QVector3D &viewRight,
    const QVector3D &viewUp,
    const QVector3D &viewFacing)
{
    WorkbenchStudioLighting lighting = source;
    if (!worldSpace) {
        // Blender only applies the Studio Light rotation when world-space
        // lighting is enabled. Otherwise the rig follows the viewport and its
        // stored view-space directions remain unchanged.
        return lighting;
    }
    constexpr float degreesToRadians = 0.01745329251994329577f;
    const float angle = -static_cast<float>(rotationDegrees) *
                        degreesToRadians;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    for (WorkbenchStudioLight &light : lighting.lights) {
        if (!light.enabled) {
            continue;
        }
        const QVector3D rotated(
            cosine * light.direction.x() - sine * light.direction.y(),
            sine * light.direction.x() + cosine * light.direction.y(),
            light.direction.z());
        light.direction = QVector3D(
            QVector3D::dotProduct(rotated, viewRight),
            QVector3D::dotProduct(rotated, viewUp),
            QVector3D::dotProduct(rotated, viewFacing));
    }
    return lighting;
}

inline QStringList workbenchMatcapPresets()
{
    return {QStringLiteral("basic_bright"),
            QStringLiteral("basic_dark"),
            QStringLiteral("basic_grey"),
            QStringLiteral("basic_side"),
            QStringLiteral("ceramic_dark"),
            QStringLiteral("ceramic_lightbulb"),
            QStringLiteral("clay_brown"),
            QStringLiteral("clay_green"),
            QStringLiteral("clay_studio"),
            QStringLiteral("clay_warm"),
            QStringLiteral("fullmetal"),
            QStringLiteral("hard_surface_grey"),
            QStringLiteral("hard_surface_red"),
            QStringLiteral("metal_bronze"),
            QStringLiteral("metal_carpaint"),
            QStringLiteral("pearl"),
            QStringLiteral("red_wax"),
            QStringLiteral("resin"),
            QStringLiteral("toon_dark"),
            QStringLiteral("toon_light")};
}

inline QImage workbenchMatcapLayerImage(const QString &presetName,
                                       const QString &layer)
{
    static QHash<QString, QImage> cache;
    const QString key = presetName + QLatin1Char('/') + layer;
    const auto cached = cache.constFind(key);
    if (cached != cache.cend()) {
        return cached.value();
    }
    QImage image(QStringLiteral(":/workbench-lighting/matcap/%1_%2.png")
                     .arg(presetName, layer));
    if (!image.isNull()) {
        cache.insert(key, image);
    }
    return image;
}

inline QImage workbenchMatcapDiffuseImage(const QString &presetName)
{
    return workbenchMatcapLayerImage(presetName, QStringLiteral("diffuse"));
}

inline QImage workbenchMatcapSpecularImage(const QString &presetName)
{
    return workbenchMatcapLayerImage(presetName, QStringLiteral("specular"));
}

inline QVector2D workbenchMatcapUv(const QVector3D &sourceIncident,
                                   const QVector3D &sourceNormal)
{
    const QVector3D incident = workbenchNormalize(sourceIncident,
                                                   {0.0f, 0.0f, 1.0f});
    const QVector3D normal = workbenchNormalize(sourceNormal,
                                                 {0.0f, 0.0f, 1.0f});
    const float a = 1.0f / (1.0f + incident.z());
    const float b = -incident.x() * incident.y() * a;
    const QVector3D basis1(1.0f - incident.x() * incident.x() * a,
                           b,
                           -incident.x());
    const QVector3D basis2(b,
                           1.0f - incident.y() * incident.y() * a,
                           -incident.y());
    return {QVector3D::dotProduct(basis1, normal) * 0.496f + 0.5f,
            QVector3D::dotProduct(basis2, normal) * 0.496f + 0.5f};
}

inline QVector3D workbenchMatcapShade(const QString &presetName,
                                      const QVector3D &baseColor,
                                      const QVector3D &normal,
                                      const QVector3D &incident,
                                      bool useSpecular = true)
{
    const QImage diffuseImage = workbenchMatcapDiffuseImage(presetName);
    const QImage specularImage = workbenchMatcapSpecularImage(presetName);
    if (diffuseImage.isNull() || specularImage.isNull()) {
        return baseColor;
    }
    const QVector2D uv = workbenchMatcapUv(incident, normal);
    const int x = std::clamp(qRound(uv.x() * (diffuseImage.width() - 1)),
                             0, diffuseImage.width() - 1);
    const int y = std::clamp(qRound((1.0f - uv.y()) *
                                    (diffuseImage.height() - 1)),
                             0, diffuseImage.height() - 1);
    const QColor diffuseSample = diffuseImage.pixelColor(x, y);
    const QColor specularSample = useSpecular
                                      ? specularImage.pixelColor(x, y)
                                      : QColor(Qt::black);
    const QVector3D diffuse = workbenchSrgbToSceneLinear(
        QVector3D(diffuseSample.redF(), diffuseSample.greenF(),
                  diffuseSample.blueF()));
    const QVector3D specular = workbenchSrgbToSceneLinear(
        QVector3D(specularSample.redF(), specularSample.greenF(),
                  specularSample.blueF()));
    return {diffuse.x() * baseColor.x() + specular.x(),
            diffuse.y() * baseColor.y() + specular.y(),
            diffuse.z() * baseColor.z() + specular.z()};
}

inline const WorkbenchStudioLighting &defaultWorkbenchStudioLighting()
{
    // Blender's factory-startup SolidLight preferences, in view-space.
    static const WorkbenchStudioLighting lighting{
        {{{{-0.352546006f, 0.170930997f, -0.920050979f},
           {0.033103000f, 0.033103000f, 0.033103000f},
           {0.266761005f, 0.266761005f, 0.266761005f},
           0.526619971f,
           true},
          {{-0.408163011f, 0.346938997f, 0.844415009f},
           {0.521082997f, 0.538226008f, 0.538226008f},
           {0.599030018f, 0.599030018f, 0.599030018f},
           0.0f,
           true},
          {{0.521739006f, 0.826086998f, 0.212999001f},
           {0.038403001f, 0.034357000f, 0.049529999f},
           {0.106101997f, 0.125981003f, 0.158522993f},
           0.478260994f,
           true},
          {{0.624518991f, -0.562066972f, -0.542268991f},
           {0.090838000f, 0.082079999f, 0.072255000f},
           {0.106535003f, 0.084770999f, 0.066079997f},
           0.200000003f,
           true}}},
        {0.0f, 0.0f, 0.0f},
        true};
    return lighting;
}

inline QVector3D workbenchNormalize(const QVector3D &value,
                                   const QVector3D &fallback = {})
{
    const float lengthSquared = value.lengthSquared();
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20f) {
        return fallback;
    }
    return value / std::sqrt(lengthSquared);
}

inline float workbenchFastReciprocal(float value)
{
    // Preserve Workbench's fast reciprocal approximation used by wrapped
    // diffuse/specular lighting. Its inputs here are positive and finite.
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    bits = 0x7eef370bU - bits;
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline float workbenchWrappedLighting(float normalLight, float wrap)
{
    const float wrapPlusOne = wrap + 1.0f;
    return std::clamp((normalLight + wrap) *
                          workbenchFastReciprocal(wrapPlusOne * wrapPlusOne),
                      0.0f,
                      1.0f);
}

inline QVector3D workbenchStudioShade(
    const QVector3D &baseColor,
    const QVector3D &sourceNormalView,
    const QVector3D &sourceIncidentView,
    float roughness = 161.0f / 255.0f,
    float metallic = 0.0f,
    const WorkbenchStudioLighting &lighting = defaultWorkbenchStudioLighting())
{
    const QVector3D normal = workbenchNormalize(sourceNormalView,
                                                {0.0f, 0.0f, 1.0f});
    const QVector3D incident = workbenchNormalize(sourceIncidentView,
                                                  {0.0f, 0.0f, 1.0f});
    const float clampedRoughness = std::clamp(roughness, 0.0f, 1.0f);
    const float clampedMetallic = std::clamp(metallic, 0.0f, 1.0f);

    QVector3D diffuseColor = baseColor;
    QVector3D specularColor;
    if (lighting.useSpecular) {
        diffuseColor *= 1.0f - clampedMetallic;
        specularColor = QVector3D(0.05f, 0.05f, 0.05f) *
                        (1.0f - clampedMetallic) +
                        baseColor * clampedMetallic;
    }

    QVector3D specularLight = lighting.ambientColor;
    QVector3D diffuseLight = lighting.ambientColor;
    const QVector3D reflectedIncident =
        -(incident - 2.0f * QVector3D::dotProduct(normal, incident) * normal);

    if (lighting.useSpecular) {
        const float normalView = std::clamp(
            QVector3D::dotProduct(normal, incident), 0.0f, 1.0f);
        const float fresnel = std::exp2(-8.35f * normalView) *
                              (1.0f - clampedRoughness);
        specularColor = specularColor * (1.0f - fresnel) +
                        QVector3D(fresnel, fresnel, fresnel);

        for (const WorkbenchStudioLight &light : lighting.lights) {
            if (!light.enabled) {
                continue;
            }
            const QVector3D direction = light.direction;
            const QVector3D halfDirection =
                workbenchNormalize(direction + incident);
            const float specularAngle = std::clamp(
                QVector3D::dotProduct(halfDirection, normal), 0.0f, 1.0f);
            const float specularNormalLight = std::clamp(
                QVector3D::dotProduct(direction, normal), 0.0f, 1.0f);
            const float wrappedNormalLight = QVector3D::dotProduct(
                direction, reflectedIncident);

            float gloss = (1.0f - clampedRoughness) * (1.0f - light.wrap);
            const float shininess = std::exp2(10.0f * gloss + 1.0f);
            const float blinn = std::pow(specularAngle, shininess) *
                                specularNormalLight *
                                (shininess * 0.125f + 1.0f);
            const float environmentWrap = light.wrap +
                (1.0f - light.wrap) * clampedRoughness;
            const float environment = workbenchWrappedLighting(
                wrappedNormalLight, environmentWrap);
            const float blend = light.wrap * light.wrap;
            const float specularAmount = blinn * (1.0f - blend) +
                                         environment * blend;
            specularLight += light.specularColor * specularAmount;
        }
        specularLight *= specularColor;
    }

    for (const WorkbenchStudioLight &light : lighting.lights) {
        if (!light.enabled) {
            continue;
        }
        const QVector3D direction = light.direction;
        const float normalLight =
            QVector3D::dotProduct(direction, normal);
        diffuseLight += light.diffuseColor *
                        workbenchWrappedLighting(normalLight, light.wrap);
    }

    if (lighting.useSpecular) {
        const float specularEnergy =
            (specularColor.x() + specularColor.y() + specularColor.z()) /
            3.0f;
        diffuseLight *= diffuseColor * (1.0f - specularEnergy);
    } else {
        diffuseLight *= diffuseColor;
    }
    return diffuseLight + specularLight;
}

} // namespace classiCAD
