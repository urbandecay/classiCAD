#pragma once

#include "layer_id.h"
#include "collection_id.h"
#include "object_id.h"
#include "shape.h"

#include <QVector3D>
#include <QVector4D>
#include <QString>
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

// Workbench material inputs are stored in scene-linear RGB, matching Blender's
// material data. These values affect viewport display only; they do not alter
// the exact NURBS geometry stored in SceneObject::geometry.
struct WorkbenchMaterialData {
    QVector3D baseColorSceneLinear{0.8f, 0.8f, 0.8f};
    qreal roughness = 0.4;
    qreal metallic = 0.0;
    qreal alpha = 1.0;
    // First external image reference retained for the Workbench Texture mode.
    QString textureImagePath;
    bool textureImageSrgb = true;
    QString textureImageAlphaMode = QStringLiteral("STRAIGHT");
    QString textureImagePackedDataBase64;
};

// Shared imported material datablock. Mesh slots refer to this record by id.
struct DocumentImageTextureReference {
    QString nodeName;
    QString imageName;
    QString imageLibraryPath;
    QString filepath;
    QString colorSpace;
    // Blender Image.alpha_mode: STRAIGHT, PREMUL, CHANNEL_PACKED, or NONE.
    QString alphaMode = QStringLiteral("STRAIGHT");
    // Base64 packed image bytes from Blender scene import, empty for external
    // images or packed data omitted by the transport size limit.
    QString packedDataBase64;
    bool packed = false;
    bool sourceFileExists = false;
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
    // The GPU Texture path currently consumes only an active external image.
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
    MeshObjectTransform meshObjectTransform = identityMeshObjectTransform;
    QString blenderSourceVersion;
    QString blenderMeshImportMode;
};

} // namespace classiCAD
