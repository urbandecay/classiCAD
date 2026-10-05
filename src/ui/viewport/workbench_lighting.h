/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The studio-light shading equations are adapted from Blender Workbench's
 * workbench_world_light.bsl.hh (GPL-2.0-or-later). The default light rig below
 * matches Blender's factory-startup "Default" studio light. */
#pragma once

#include <QVector3D>

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

inline QVector3D workbenchDefaultSolidMaterialDiffuseColor()
{
    // Blender's factory-startup material uses diffuse_color = (0.8, 0.8, 0.8).
    // This value is already scene-linear; it must not be decoded as sRGB.
    return {0.8f, 0.8f, 0.8f};
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
