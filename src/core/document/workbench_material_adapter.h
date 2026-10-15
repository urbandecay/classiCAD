#pragma once

#include "scene_object.h"

namespace classiCAD {

// Project the shared document material into the Workbench-facing material
// contract. Blender import and viewport frame construction use this same
// conversion so supported values and image-source precedence cannot drift.
inline WorkbenchMaterialData workbenchMaterialDataFromDocumentMaterial(
    const DocumentMaterial &material)
{
    WorkbenchMaterialData result;
    if (material.baseColorSceneLinear.size() >= 3) {
        result.baseColorSceneLinear = QVector3D(
            material.baseColorSceneLinear[0], material.baseColorSceneLinear[1],
            material.baseColorSceneLinear[2]);
    }
    result.roughness = material.roughness;
    result.metallic = material.metallic;
    result.alpha = material.alpha;
    for (const DocumentImageTextureReference &image : material.imageTextures) {
        result.textureImageSrgb = image.colorSpace.compare(
            QStringLiteral("sRGB"), Qt::CaseInsensitive) == 0;
        result.textureImageColorTransformId = image.sourceTransformId;
        result.textureImageSceneLinearFloatDataBase64 =
            image.sceneLinearFloatDataBase64;
        result.textureImageSceneLinearFloatWidth = image.sceneLinearFloatWidth;
        result.textureImageSceneLinearFloatHeight = image.sceneLinearFloatHeight;
        result.textureImageAlphaMode = image.alphaMode;
        result.textureImageInterpolation = image.interpolation;
        result.textureImageExtension = image.extension;
        if (!image.tiles.isEmpty()) {
            result.textureImageTiles = image.tiles;
            break;
        }
        if (image.packed && !image.packedDataBase64.isEmpty()) {
            result.textureImagePackedDataBase64 = image.packedDataBase64;
            break;
        }
        if (image.sourceFileExists && !image.filepath.isEmpty()) {
            result.textureImagePath = image.filepath;
            break;
        }
    }
    return result;
}

} // namespace classiCAD
