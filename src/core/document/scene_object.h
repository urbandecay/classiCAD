#pragma once

#include "layer_id.h"
#include "collection_id.h"
#include "object_id.h"
#include "shape.h"

#include <QVector3D>
#include <QJsonObject>
#include <QVector4D>
#include <QString>
#include <QHash>
#include <array>

namespace classiCAD {

// Row-major local-to-world affine matrix used by imported polygon meshes.
// Other geometry types continue to use their existing placement/frame data.
using MeshObjectTransform = std::array<double, 16>;
inline constexpr MeshObjectTransform identityMeshObjectTransform{
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0};

// One Blender tiled-image entry. Tile numbers follow Blender's UDIM numbering
// (1001 is UV tile (0, 0)); renderers prefer packed bytes and fall back to path.
struct DocumentImageTextureTile {
    int number = 1001;
    QString filepath;
    bool sourceFileExists = false;
    // Base64 encoded packed bytes for this exact UDIM tile, empty when the
    // tile is external or Blender omitted it due to the aggregate byte cap.
    QString packedDataBase64;
    // Optional exact scene-linear RGBA32F Workbench payload (base64 encoded,
    // little-endian), used when an OCIO source-space conversion is needed.
    QString sceneLinearFloatDataBase64;
    int sceneLinearFloatWidth = 0;
    int sceneLinearFloatHeight = 0;
};

// Workbench material inputs are stored in scene-linear RGB, matching Blender's
// material data. These values affect viewport display only; they do not alter
// the exact NURBS geometry stored in SceneObject::geometry.
struct WorkbenchMaterialData {
    QVector3D baseColorSceneLinear{0.8f, 0.8f, 0.8f};
    qreal roughness = 0.4;
    qreal metallic = 0.0;
    qreal alpha = 1.0;
    // Single-image input retained for Workbench Texture mode. Tiled-image
    // entries travel separately in textureImageTiles.
    QString textureImagePath;
    bool textureImageSrgb = true;
    // Identifies the source-space conversion used for the float texel payload.
    // ACEScg byte images use a config-specific OCIO transform hash; float
    // buffers use Blender's direct-upload semantics identifier.
    QString textureImageColorTransformId = QStringLiteral("identity");
    QString textureImageSceneLinearFloatDataBase64;
    int textureImageSceneLinearFloatWidth = 0;
    int textureImageSceneLinearFloatHeight = 0;
    QString textureImageAlphaMode = QStringLiteral("STRAIGHT");
    QString textureImageInterpolation = QStringLiteral("Linear");
    QString textureImageExtension = QStringLiteral("REPEAT");
    QString textureImagePackedDataBase64;
    QVector<DocumentImageTextureTile> textureImageTiles;
};

// Shared imported material datablock. Mesh slots refer to this record by id.
struct DocumentImageTextureReference {
    QString nodeName;
    // Active Blender node type retained for .blend proxy reconstruction.
    QString nodeType = QStringLiteral("TEX_IMAGE");
    QString imageName;
    QString imageLibraryPath;
    QString filepath;
    QString colorSpace;
    QString sourceTransformId = QStringLiteral("identity");
    // Blender Image.alpha_mode: STRAIGHT, PREMUL, CHANNEL_PACKED, or NONE.
    QString alphaMode = QStringLiteral("STRAIGHT");
    // Blender image-texture node sampling settings. Environment Texture uses
    // the Workbench sampler's Extend default when no extension is exposed.
    QString interpolation = QStringLiteral("Linear");
    QString extension = QStringLiteral("REPEAT");
    // Base64 packed image bytes from Blender scene import, empty for external
    // images or packed data omitted by the transport size limit.
    QString packedDataBase64;
    bool packed = false;
    bool sourceFileExists = false;
    QVector<DocumentImageTextureTile> tiles;
    QString sceneLinearFloatDataBase64;
    int sceneLinearFloatWidth = 0;
    int sceneLinearFloatHeight = 0;
};

struct DocumentMaterial {
    QString id;
    QString name;
    QString libraryPath;
    QVector<float> baseColorSceneLinear{0.8f, 0.8f, 0.8f, 1.0f};
    float roughness = 0.4f;
    float metallic = 0.0f;
    float alpha = 1.0f;
    // Source image references are retained without evaluating node graphs.
    // Workbench Texture mode resolves the active Image Texture or Environment
    // Texture node's image, without evaluating the surrounding node graph.
    QVector<DocumentImageTextureReference> imageTextures;
};

bool isValidDocumentMaterial(const DocumentMaterial &material);

struct SceneObject {
    ObjectId id = ObjectId::invalid();
    LayerId layerId = LayerId::invalid();
    QVector<CollectionId> collectionIds;
    bool hideViewport = false;
    bool hideRender = false;
    bool hiddenInViewport = false;
    bool hideSelect = false;
    // Blender Object.show_in_front: draw this object through other geometry.
    bool showInFront = false;
    QJsonObject viewLayerHidden;
    QString defaultViewLayerKey;
    // Blender ID identity used by Workbench Random color. Imported linked
    // objects retain the source library path when the importer provides it.
    QString displayName;
    QString libraryPath;
    Shape geometry;
    // World-space translation applied after evaluating spatial NURBS surface
    // or solid geometry. Planar curves keep their existing workplane mapping.
    Point3D placementTranslation;
    // Blender Object.color default: opaque white in scene-linear RGBA.
    QVector4D displayColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
    WorkbenchMaterialData workbenchMaterial;
    // True when imported or explicitly assigned Workbench material values
    // override the object's display color in Material color mode.
    bool hasWorkbenchMaterial = false;
    // NurbsSolid seed-face identity to shared document material identity.
    // Materialized boundary faces have no seed-face lineage and ignore this.
    QHash<QString, QString> nurbsSolidFaceMaterialIds;
    MeshObjectTransform meshObjectTransform = identityMeshObjectTransform;
    QString blenderSourceVersion;
    QString blenderMeshImportMode;
};

} // namespace classiCAD
