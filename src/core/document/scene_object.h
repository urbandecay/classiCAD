#pragma once

#include "collection_id.h"
#include "object_id.h"
#include "shape.h"

#include <QVector3D>
#include <QJsonObject>
#include <QVector4D>
#include <QString>
#include <QStringList>
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
    // Byte images converted through OCIO use a config/source-specific hash;
    // float buffers use Blender's direct-upload semantics identifier.
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

struct BlenderSourceNlaTrack {
    QString name;
    QStringList actionNames;
};

struct BlenderSourceModifier {
    QString name;
    QString linkedName;
    QString linkedType;
};

struct BlenderSourceShaderEffect {
    QString name;
    QString linkedName;
    QString linkedType;
};

struct BlenderSourceAnimationData {
    bool hasAnimationData = false;
    QString actionName;
    QString actionSlotName;
    bool hasDrivers = false;
    QStringList driverTargetNames;
    QVector<BlenderSourceNlaTrack> nlaTracks;
};

struct SceneObject {
    ObjectId id = ObjectId::invalid();
    QVector<CollectionId> collectionIds;
    bool hideViewport = false;
    bool hideRender = false;
    bool hiddenInViewport = false;
    bool hideSelect = false;
    // Blender Object.hide_surface_pick: keep the object visible, but remove
    // its surface from selection and depth-picking passes.
    bool hideSurfacePick = false;
    // Blender Object.show_in_front: draw this object through other geometry.
    bool showInFront = false;
    // Blender object-level Solid wire overlays.
    bool showWire = false;
    bool showAllEdges = false;
    bool wireDisplay = false;
    // Imported Blender mesh loose edges are represented by a curve companion
    // but still follow the source mesh's overlay visibility.
    bool blenderLooseMeshEdgeCompanion = false;
    QVector<int> blenderLooseMeshEdgeSourceIndices;
    // Original Blender object identity for derived geometry such as loose-edge
    // companions. Kept so viewport selection can follow the source object.
    QString blenderSourceObjectId;
    // Blender parent identity retained for Outliner hierarchy. Geometry has
    // already been imported in world space, so this does not apply transforms.
    QString blenderParentObjectId;
    // Name of the Blender ID stored in Object.data, used by the Outliner.
    QString blenderSourceDataName;
    QString blenderSourceShapeKeyDataName;
    QStringList blenderSourceShapeKeyNames;
    QStringList blenderSourceDataMaterialNames;
    QVector<BlenderSourceAnimationData> blenderSourceDataMaterialAnimations;
    BlenderSourceAnimationData blenderSourceDataAnimation;
    // Assigned Blender materials shown beneath any supported Outliner object.
    QStringList blenderSourceMaterialNames;
    QVector<BlenderSourceAnimationData> blenderSourceMaterialAnimations;
    // Blender Outliner object subtrees retained from the source file.
    QStringList blenderSourceModifierNames;
    QVector<BlenderSourceModifier> blenderSourceModifiers;
    QVector<BlenderSourceShaderEffect> blenderSourceShaderEffects;
    QStringList blenderSourceConstraintNames;
    QStringList blenderSourceVertexGroupNames;
    bool blenderSourceHasAnimationData = false;
    QString blenderSourceAnimationActionName;
    QString blenderSourceAnimationActionSlotName;
    bool blenderSourceHasDrivers = false;
    QStringList blenderSourceDriverTargetNames;
    QVector<BlenderSourceNlaTrack> blenderSourceNlaTracks;
    // Blender Object display bounds shape (BOX/SPHERE/CYLINDER/CONE/CAPSULE).
    // Empty means the object's imported geometry is drawn normally.
    QString displayBoundsType;
    bool hasDisplayBoundsData = false;
    Point3D displayBoundsMinimumLocal;
    Point3D displayBoundsMaximumLocal;
    MeshObjectTransform displayBoundsTransformWorld =
        identityMeshObjectTransform;
    QJsonObject viewLayerHidden;
    QString defaultViewLayerKey;
    // Blender ID identity used by Workbench Random color. Imported linked
    // objects retain the source library path when the importer provides it.
    QString displayName;
    QString libraryPath;
    // Blender's in-memory linked ID filepath used by Workbench's Random hash.
    // This may be absolute even when libraryPath retains Blender's // form.
    QString workbenchRandomLibraryPath;
    Shape geometry;
    // Blender-evaluated SURFACE wire edges in world coordinates. Display-only;
    // the exact NURBS surface remains authoritative.
    QVector<Point3D> blenderSurfaceWireVerticesWorld;
    // Hash of the exact surface at import. Stale evaluated wires are ignored
    // after control-net or trim edits.
    QByteArray blenderSurfaceWireGeometryFingerprint;
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
    // Original Blender object type. Kept for viewport behaviors that differ
    // between legacy Blender object families and native classiCAD geometry.
    QString blenderSourceObjectType;
};

} // namespace classiCAD
