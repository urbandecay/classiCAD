#include "core/serialization/blender_project_file.h"

#include <QDebug>

#include <cmath>

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) qCritical("%s", message);
    return condition;
}

} // namespace

int main()
{
    using namespace classiCAD;

    DocumentMaterial material;
    material.baseColorSceneLinear = {0.21f, 0.43f, 0.65f, 0.77f};
    material.roughness = 0.28f;
    material.metallic = 0.39f;
    material.alpha = 0.77f;

    DocumentImageTextureReference regularImage;
    regularImage.imageName = QStringLiteral("PackedSurfaceTexture");
    regularImage.colorSpace = QStringLiteral("ACEScg");
    regularImage.sourceTransformId = QStringLiteral("aces-transform-hash");
    regularImage.sceneLinearFloatDataBase64 = QStringLiteral("float-payload");
    regularImage.sceneLinearFloatWidth = 2;
    regularImage.sceneLinearFloatHeight = 1;
    regularImage.alphaMode = QStringLiteral("PREMUL");
    regularImage.interpolation = QStringLiteral("Closest");
    regularImage.extension = QStringLiteral("EXTEND");
    regularImage.packed = true;
    regularImage.packedDataBase64 = QStringLiteral("exact-packed-bytes");
    regularImage.sourceFileExists = true;
    regularImage.filepath = QStringLiteral("/texture/fallback.png");
    material.imageTextures.append(regularImage);

    const WorkbenchMaterialData resolved =
        workbenchMaterialForBlenderImport(material);
    bool passed = true;
    passed &= check(resolved.baseColorSceneLinear == QVector3D(0.21f, 0.43f, 0.65f) &&
                        std::abs(resolved.roughness - 0.28) < 1.0e-6 &&
                        std::abs(resolved.metallic - 0.39) < 1.0e-6 &&
                        std::abs(resolved.alpha - 0.77) < 1.0e-6,
                    "surface material resolution must retain scalar Workbench fields");
    passed &= check(!resolved.textureImageSrgb &&
                        resolved.textureImageColorTransformId ==
                            QStringLiteral("aces-transform-hash") &&
                        resolved.textureImageSceneLinearFloatDataBase64 ==
                            QStringLiteral("float-payload") &&
                        resolved.textureImageSceneLinearFloatWidth == 2 &&
                        resolved.textureImageSceneLinearFloatHeight == 1 &&
                        resolved.textureImageAlphaMode == QStringLiteral("PREMUL") &&
                        resolved.textureImageInterpolation == QStringLiteral("Closest") &&
                        resolved.textureImageExtension == QStringLiteral("EXTEND"),
                    "regular active image resolution must retain color and sampler metadata");
    passed &= check(resolved.textureImagePackedDataBase64 ==
                            QStringLiteral("exact-packed-bytes") &&
                        resolved.textureImagePath.isEmpty() &&
                        resolved.textureImageTiles.isEmpty(),
                    "packed regular images must keep their exact bytes before external-path fallback");

    DocumentImageTextureReference externalImage;
    externalImage.sourceFileExists = true;
    externalImage.filepath = QStringLiteral("/texture/external.png");
    material.imageTextures = {externalImage};
    const WorkbenchMaterialData externalResolved =
        workbenchMaterialForBlenderImport(material);
    passed &= check(externalResolved.textureImagePath == externalImage.filepath,
                    "available external regular images must retain their resolved path");

    DocumentImageTextureReference tiledImage;
    tiledImage.tiles = {{1001, QStringLiteral("/texture/tile.1001.png"), true,
                         QStringLiteral("tile-bytes"), {}, 0, 0}};
    tiledImage.packed = true;
    tiledImage.packedDataBase64 = QStringLiteral("must-not-replace-tiles");
    material.imageTextures = {tiledImage};
    const WorkbenchMaterialData tiledResolved =
        workbenchMaterialForBlenderImport(material);
    passed &= check(tiledResolved.textureImageTiles.size() == 1 &&
                        tiledResolved.textureImageTiles[0].packedDataBase64 ==
                            QStringLiteral("tile-bytes") &&
                        tiledResolved.textureImagePackedDataBase64.isEmpty(),
                    "UDIM tiles must retain precedence over single-image payload fields");
    return passed ? 0 : 1;
}
