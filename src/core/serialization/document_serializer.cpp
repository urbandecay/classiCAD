#include "document_serializer.h"

#include "core/serialization/shape_json_codec.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace classiCAD {

namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

QJsonValue idToJson(quint64 value)
{
    return QString::number(value);
}

bool idFromJson(const QJsonValue &value, quint64 *id)
{
    if (id == nullptr) {
        return false;
    }

    if (value.isString()) {
        bool converted = false;
        const quint64 parsed = value.toString().toULongLong(&converted);
        if (converted && parsed != 0) {
            *id = parsed;
            return true;
        }
        return false;
    }

    if (value.isDouble()) {
        const double parsed = value.toDouble();
        if (parsed > 0.0 && parsed <= std::numeric_limits<quint64>::max() &&
            std::floor(parsed) == parsed) {
            *id = static_cast<quint64>(parsed);
            return true;
        }
    }
    return false;
}

QJsonArray vector3ToJson(const QVector3D &value)
{
    return {value.x(), value.y(), value.z()};
}

QJsonArray vector4ToJson(const QVector4D &value)
{
    return {value.x(), value.y(), value.z(), value.w()};
}

bool unitIntervalArrayFromJson(const QJsonValue &value,
                               int componentCount,
                               float *components)
{
    if (!value.isArray() || components == nullptr) {
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != componentCount) {
        return false;
    }
    for (int index = 0; index < componentCount; ++index) {
        if (!array[index].isDouble()) {
            return false;
        }
        const double component = array[index].toDouble();
        if (!std::isfinite(component) || component < 0.0 || component > 1.0) {
            return false;
        }
        components[index] = static_cast<float>(component);
    }
    return true;
}

bool workbenchMaterialFromJson(const QJsonValue &value,
                               WorkbenchMaterialData *material)
{
    if (material == nullptr || !value.isObject()) {
        return false;
    }

    const QJsonObject object = value.toObject();
    float baseColor[3] = {};
    if (!unitIntervalArrayFromJson(
            object.value(QStringLiteral("baseColorSceneLinear")), 3,
            baseColor)) {
        return false;
    }

    const auto scalar = [&object](const QString &key, qreal *result) {
        const QJsonValue value = object.value(key);
        if (result == nullptr || !value.isDouble()) {
            return false;
        }
        const double parsed = value.toDouble();
        if (!std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
            return false;
        }
        *result = parsed;
        return true;
    };

    WorkbenchMaterialData restored;
    restored.baseColorSceneLinear =
        QVector3D(baseColor[0], baseColor[1], baseColor[2]);
    if (!scalar(QStringLiteral("roughness"), &restored.roughness) ||
        !scalar(QStringLiteral("metallic"), &restored.metallic) ||
        !scalar(QStringLiteral("alpha"), &restored.alpha)) {
        return false;
    }
    *material = restored;
    return true;
}

} // namespace

QJsonObject documentToJson(const Document &document)
{
    QJsonObject serialized;
    serialized.insert(QStringLiteral("version"), 9);
    QJsonObject settings;
    settings.insert(QStringLiteral("lengthUnit"),
                    documentLengthUnitKey(document.settings().lengthUnit));
    settings.insert(QStringLiteral("gridSpacing"), document.settings().gridSpacing);
    serialized.insert(QStringLiteral("settings"), settings);
    serialized.insert(QStringLiteral("activeLayerId"),
                      idToJson(document.activeLayerId().value()));

    QJsonArray layers;
    for (const Layer &layer : document.layers()) {
        QJsonObject serializedLayer;
        serializedLayer.insert(QStringLiteral("id"), idToJson(layer.id.value()));
        serializedLayer.insert(QStringLiteral("name"), layer.name);
        serializedLayer.insert(QStringLiteral("color"), layer.color.name(QColor::HexArgb));
        serializedLayer.insert(QStringLiteral("lineType"), layer.lineType);
        serializedLayer.insert(QStringLiteral("lineWeightMm"), layer.lineWeightMm);
        serializedLayer.insert(QStringLiteral("description"), layer.description);
        serializedLayer.insert(QStringLiteral("visible"), layer.visible);
        serializedLayer.insert(QStringLiteral("frozen"), layer.frozen);
        serializedLayer.insert(QStringLiteral("locked"), layer.locked);
        serializedLayer.insert(QStringLiteral("plotted"), layer.plotted);

        QJsonArray objectIds;
        for (const ObjectId objectId : layer.objectIds) {
            objectIds.append(idToJson(objectId.value()));
        }
        serializedLayer.insert(QStringLiteral("objectIds"), objectIds);
        layers.append(serializedLayer);
    }
    serialized.insert(QStringLiteral("layers"), layers);

    QJsonArray materials;
    for (const DocumentMaterial &material : document.materials()) {
        QJsonObject record;
        record.insert(QStringLiteral("id"), material.id);
        record.insert(QStringLiteral("name"), material.name);
        record.insert(QStringLiteral("libraryPath"), material.libraryPath);
        QJsonArray color;
        for (const float component : material.baseColorSceneLinear) color.append(component);
        record.insert(QStringLiteral("baseColorSceneLinear"), color);
        record.insert(QStringLiteral("roughness"), material.roughness);
        record.insert(QStringLiteral("metallic"), material.metallic);
        record.insert(QStringLiteral("alpha"), material.alpha);
        QJsonArray imageTextures;
        for (const DocumentImageTextureReference &reference : material.imageTextures) {
            QJsonObject image;
            image.insert(QStringLiteral("nodeName"), reference.nodeName);
            image.insert(QStringLiteral("imageName"), reference.imageName);
            image.insert(QStringLiteral("imageLibraryPath"), reference.imageLibraryPath);
            image.insert(QStringLiteral("filepath"), reference.filepath);
            image.insert(QStringLiteral("colorSpace"), reference.colorSpace);
            image.insert(QStringLiteral("packed"), reference.packed);
            image.insert(QStringLiteral("sourceFileExists"), reference.sourceFileExists);
            imageTextures.append(image);
        }
        record.insert(QStringLiteral("imageTextures"), imageTextures);
        materials.append(record);
    }
    serialized.insert(QStringLiteral("materials"), materials);

    QJsonArray objects;
    for (const SceneObject &sceneObject : document.objects()) {
        QJsonObject serializedObject;
        serializedObject.insert(QStringLiteral("id"), idToJson(sceneObject.id.value()));
        serializedObject.insert(QStringLiteral("layerId"), idToJson(sceneObject.layerId.value()));
        serializedObject.insert(QStringLiteral("displayName"), sceneObject.displayName);
        serializedObject.insert(QStringLiteral("libraryPath"), sceneObject.libraryPath);
        serializedObject.insert(QStringLiteral("blenderSourceVersion"),
                                sceneObject.blenderSourceVersion);
        serializedObject.insert(QStringLiteral("blenderMeshImportMode"),
                                sceneObject.blenderMeshImportMode);
        serializedObject.insert(QStringLiteral("geometry"), shapeToJson(sceneObject.geometry));
        QJsonObject placement;
        placement.insert(QStringLiteral("x"), sceneObject.placementTranslation.x);
        placement.insert(QStringLiteral("y"), sceneObject.placementTranslation.y);
        placement.insert(QStringLiteral("z"), sceneObject.placementTranslation.z);
        serializedObject.insert(QStringLiteral("placementTranslation"), placement);
        serializedObject.insert(
            QStringLiteral("displayColorSceneLinear"),
            vector4ToJson(sceneObject.displayColorSceneLinear));
        QJsonObject workbenchMaterial;
        workbenchMaterial.insert(
            QStringLiteral("baseColorSceneLinear"),
            vector3ToJson(sceneObject.workbenchMaterial.baseColorSceneLinear));
        workbenchMaterial.insert(QStringLiteral("roughness"),
                                 sceneObject.workbenchMaterial.roughness);
        workbenchMaterial.insert(QStringLiteral("metallic"),
                                 sceneObject.workbenchMaterial.metallic);
        workbenchMaterial.insert(QStringLiteral("alpha"),
                                 sceneObject.workbenchMaterial.alpha);
        serializedObject.insert(QStringLiteral("workbenchMaterial"),
                                workbenchMaterial);
        if (sceneObject.geometry.geometryType == GeometryType::PolygonMesh) {
            QJsonArray transform;
            for (const double value : sceneObject.meshObjectTransform) {
                transform.append(value);
            }
            serializedObject.insert(QStringLiteral("meshObjectTransform"), transform);
        }
        objects.append(serializedObject);
    }
    serialized.insert(QStringLiteral("objects"), objects);
    return serialized;
}

bool documentFromJson(const QJsonValue &value,
                      Document *document,
                      QString *errorMessage)
{
    if (document == nullptr || !value.isObject()) {
        setError(errorMessage, QStringLiteral("document must be an object"));
        return false;
    }

    const QJsonObject serialized = value.toObject();
    const int version = serialized.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > 9) {
        setError(errorMessage, QStringLiteral("unsupported document version"));
        return false;
    }

    const QJsonValue layersValue = serialized.value(QStringLiteral("layers"));
    const QJsonValue objectsValue = serialized.value(QStringLiteral("objects"));
    if (!layersValue.isArray() || !objectsValue.isArray()) {
        setError(errorMessage, QStringLiteral("document layers and objects must be arrays"));
        return false;
    }

    Document::Snapshot snapshot;
    QSet<QString> materialIds;
    if (version >= 9) {
        const QJsonValue materialsValue = serialized.value(QStringLiteral("materials"));
        if (!materialsValue.isArray()) {
            setError(errorMessage, QStringLiteral("document materials must be an array"));
            return false;
        }
        for (const QJsonValue &materialValue : materialsValue.toArray()) {
            if (!materialValue.isObject()) {
                setError(errorMessage, QStringLiteral("invalid document material"));
                return false;
            }
            const QJsonObject record = materialValue.toObject();
            const QJsonValue id = record.value(QStringLiteral("id"));
            const QJsonValue name = record.value(QStringLiteral("name"));
            const QJsonValue libraryPath = record.value(QStringLiteral("libraryPath"));
            const QJsonValue color = record.value(QStringLiteral("baseColorSceneLinear"));
            const QJsonValue roughness = record.value(QStringLiteral("roughness"));
            const QJsonValue metallic = record.value(QStringLiteral("metallic"));
            const QJsonValue alpha = record.value(QStringLiteral("alpha"));
            if (!id.isString() || id.toString().isEmpty() || materialIds.contains(id.toString()) ||
                !name.isString() || !libraryPath.isString() || !color.isArray() ||
                color.toArray().size() != 4 || !roughness.isDouble() ||
                !metallic.isDouble() || !alpha.isDouble()) {
                setError(errorMessage, QStringLiteral("invalid document material"));
                return false;
            }
            DocumentMaterial material;
            material.id = id.toString();
            material.name = name.toString();
            material.libraryPath = libraryPath.toString();
            material.baseColorSceneLinear.clear();
            for (const QJsonValue &component : color.toArray()) {
                if (!component.isDouble() || !std::isfinite(component.toDouble())) {
                    setError(errorMessage, QStringLiteral("invalid document material color"));
                    return false;
                }
                material.baseColorSceneLinear.append(static_cast<float>(component.toDouble()));
            }
            material.roughness = static_cast<float>(roughness.toDouble());
            material.metallic = static_cast<float>(metallic.toDouble());
            material.alpha = static_cast<float>(alpha.toDouble());
            const QJsonValue imageTexturesValue = record.value(QStringLiteral("imageTextures"));
            if (!imageTexturesValue.isUndefined()) {
                if (!imageTexturesValue.isArray()) {
                    setError(errorMessage, QStringLiteral("invalid document image texture references"));
                    return false;
                }
                for (const QJsonValue &imageValue : imageTexturesValue.toArray()) {
                    if (!imageValue.isObject()) {
                        setError(errorMessage, QStringLiteral("invalid document image texture reference"));
                        return false;
                    }
                    const QJsonObject image = imageValue.toObject();
                    const QStringList stringKeys{QStringLiteral("nodeName"), QStringLiteral("imageName"),
                        QStringLiteral("imageLibraryPath"), QStringLiteral("filepath"), QStringLiteral("colorSpace")};
                    bool validImage = true;
                    for (const QString &key : stringKeys) validImage &= image.value(key).isString();
                    if (!validImage || !image.value(QStringLiteral("packed")).isBool() ||
                        !image.value(QStringLiteral("sourceFileExists")).isBool() ||
                        image.value(QStringLiteral("imageName")).toString().isEmpty()) {
                        setError(errorMessage, QStringLiteral("invalid document image texture reference"));
                        return false;
                    }
                    DocumentImageTextureReference reference;
                    reference.nodeName = image.value(QStringLiteral("nodeName")).toString();
                    reference.imageName = image.value(QStringLiteral("imageName")).toString();
                    reference.imageLibraryPath = image.value(QStringLiteral("imageLibraryPath")).toString();
                    reference.filepath = image.value(QStringLiteral("filepath")).toString();
                    reference.colorSpace = image.value(QStringLiteral("colorSpace")).toString();
                    reference.packed = image.value(QStringLiteral("packed")).toBool();
                    reference.sourceFileExists = image.value(QStringLiteral("sourceFileExists")).toBool();
                    material.imageTextures.append(std::move(reference));
                }
            }
            if (!isValidDocumentMaterial(material)) {
                setError(errorMessage, QStringLiteral("invalid document material values"));
                return false;
            }
            materialIds.insert(material.id);
            snapshot.materials.append(std::move(material));
        }
    }
    if (version >= 4) {
        const QJsonValue settingsValue = serialized.value(QStringLiteral("settings"));
        if (!settingsValue.isObject()) {
            setError(errorMessage, QStringLiteral("document settings must be an object"));
            return false;
        }
        const QJsonObject settingsObject = settingsValue.toObject();
        DocumentSettings settings;
        const QJsonValue lengthUnitValue =
            settingsObject.value(QStringLiteral("lengthUnit"));
        const QJsonValue gridSpacingValue =
            settingsObject.value(QStringLiteral("gridSpacing"));
        if (!lengthUnitValue.isString() ||
            !documentLengthUnitFromKey(lengthUnitValue.toString(),
                                       &settings.lengthUnit) ||
            !gridSpacingValue.isDouble()) {
            setError(errorMessage, QStringLiteral("invalid document grid settings"));
            return false;
        }
        settings.gridSpacing = gridSpacingValue.toDouble();
        if (!isValidDocumentSettings(settings)) {
            setError(errorMessage, QStringLiteral("invalid document grid settings"));
            return false;
        }
        snapshot.settings = settings;
    }
    QSet<quint64> layerIds;
    quint64 nextLayerValue = 1;
    for (const QJsonValue &layerValue : layersValue.toArray()) {
        if (!layerValue.isObject()) {
            setError(errorMessage, QStringLiteral("layer record must be an object"));
            return false;
        }

        const QJsonObject serializedLayer = layerValue.toObject();
        quint64 layerValueId = 0;
        const QString name = serializedLayer.value(QStringLiteral("name")).toString().trimmed();
        if (!idFromJson(serializedLayer.value(QStringLiteral("id")), &layerValueId) ||
            layerIds.contains(layerValueId) || name.isEmpty() ||
            !serializedLayer.value(QStringLiteral("visible")).isBool() ||
            !serializedLayer.value(QStringLiteral("locked")).isBool()) {
            setError(errorMessage, QStringLiteral("invalid layer record"));
            return false;
        }

        Layer layer;
        layer.id = LayerId::fromValue(layerValueId);
        layer.name = name;
        const QJsonValue colorValue = serializedLayer.value(QStringLiteral("color"));
        if (colorValue.isUndefined()) {
            if (version >= 2) {
                setError(errorMessage, QStringLiteral("layer color is missing"));
                return false;
            }
            // Keep the version-1 file default stable when the current default
            // for newly created layers changes.
            layer.color = QColor(QStringLiteral("#d28b45"));
        } else {
            if (!colorValue.isString()) {
                setError(errorMessage, QStringLiteral("invalid layer color"));
                return false;
            }
            const QColor color(colorValue.toString());
            if (!color.isValid()) {
                setError(errorMessage, QStringLiteral("invalid layer color"));
                return false;
            }
            layer.color = color;
        }
        if (version >= 3) {
            const QJsonValue lineTypeValue =
                serializedLayer.value(QStringLiteral("lineType"));
            const QJsonValue lineWeightValue =
                serializedLayer.value(QStringLiteral("lineWeightMm"));
            const QJsonValue descriptionValue =
                serializedLayer.value(QStringLiteral("description"));
            const QJsonValue frozenValue = serializedLayer.value(QStringLiteral("frozen"));
            const QJsonValue plottedValue = serializedLayer.value(QStringLiteral("plotted"));
            const QString lineType = lineTypeValue.toString().trimmed();
            const qreal lineWeightMm = lineWeightValue.toDouble(-1.0);
            if (!lineTypeValue.isString() || lineType.isEmpty() ||
                !lineWeightValue.isDouble() || !std::isfinite(lineWeightMm) ||
                lineWeightMm < 0.0 || lineWeightMm > 2.11 ||
                !descriptionValue.isString() || !frozenValue.isBool() ||
                !plottedValue.isBool()) {
                setError(errorMessage, QStringLiteral("invalid layer properties"));
                return false;
            }
            layer.lineType = lineType;
            layer.lineWeightMm = lineWeightMm;
            layer.description = descriptionValue.toString();
            layer.frozen = frozenValue.toBool();
            layer.plotted = plottedValue.toBool();
        }
        layer.visible = serializedLayer.value(QStringLiteral("visible")).toBool();
        layer.locked = serializedLayer.value(QStringLiteral("locked")).toBool();
        snapshot.layers.append(layer);
        layerIds.insert(layerValueId);
        if (layerValueId < std::numeric_limits<quint64>::max()) {
            nextLayerValue = std::max(nextLayerValue, layerValueId + 1);
        }
    }

    if (snapshot.layers.isEmpty()) {
        setError(errorMessage, QStringLiteral("document must contain a layer"));
        return false;
    }

    quint64 activeLayerValue = 0;
    if (!idFromJson(serialized.value(QStringLiteral("activeLayerId")), &activeLayerValue) ||
        !layerIds.contains(activeLayerValue)) {
        setError(errorMessage, QStringLiteral("invalid active layer"));
        return false;
    }
    snapshot.activeLayerId = LayerId::fromValue(activeLayerValue);
    const auto activeLayer = std::find_if(
        snapshot.layers.cbegin(),
        snapshot.layers.cend(),
        [activeLayerValue](const Layer &layer) {
            return layer.id.value() == activeLayerValue;
        });
    if (activeLayer == snapshot.layers.cend() || !activeLayer->visible ||
        activeLayer->frozen || activeLayer->locked) {
        setError(errorMessage, QStringLiteral("active layer must be visible and unlocked"));
        return false;
    }

    QSet<quint64> objectIds;
    quint64 nextObjectValue = 1;
    for (const QJsonValue &objectValue : objectsValue.toArray()) {
        if (!objectValue.isObject()) {
            setError(errorMessage, QStringLiteral("scene object record must be an object"));
            return false;
        }

        const QJsonObject serializedObject = objectValue.toObject();
        quint64 objectValueId = 0;
        quint64 objectLayerValue = 0;
        Shape shape;
        if (!idFromJson(serializedObject.value(QStringLiteral("id")), &objectValueId) ||
            objectIds.contains(objectValueId) ||
            !idFromJson(serializedObject.value(QStringLiteral("layerId")), &objectLayerValue) ||
            !layerIds.contains(objectLayerValue) ||
            !shapeFromJson(serializedObject.value(QStringLiteral("geometry")), &shape)) {
            setError(errorMessage, QStringLiteral("invalid scene object record"));
            return false;
        }

        if (shape.geometryType == GeometryType::PolygonMesh) {
            PolygonMesh3D &mesh = shape.polygonMesh;
            if (version == 8 && !mesh.materialSlots.isEmpty()) {
                mesh.materialSlotIds.clear();
                mesh.materialSlotIds.reserve(mesh.materialSlots.size());
                for (int slotIndex = 0; slotIndex < mesh.materialSlots.size(); ++slotIndex) {
                    const PolygonMeshMaterialSlot &legacySlot = mesh.materialSlots[slotIndex];
                    DocumentMaterial material;
                    material.id = QStringLiteral("legacy-v8:%1:%2")
                                      .arg(objectValueId).arg(slotIndex);
                    material.name = legacySlot.name;
                    material.libraryPath = legacySlot.libraryPath;
                    material.baseColorSceneLinear = legacySlot.baseColorSceneLinear;
                    material.roughness = legacySlot.roughness;
                    material.metallic = legacySlot.metallic;
                    material.alpha = legacySlot.alpha;
                    if (!isValidDocumentMaterial(material)) {
                        setError(errorMessage, QStringLiteral("invalid legacy mesh material"));
                        return false;
                    }
                    materialIds.insert(material.id);
                    snapshot.materials.append(std::move(material));
                    mesh.materialSlotIds.append(
                        snapshot.materials.last().id);
                }
                mesh.materialSlots.clear();
                if (!validatePolygonMesh(mesh)) {
                    setError(errorMessage, QStringLiteral("invalid mesh material references"));
                    return false;
                }
            }
            for (const QString &materialId : mesh.materialSlotIds) {
                if (!materialId.isEmpty() && !materialIds.contains(materialId)) {
                    setError(errorMessage, QStringLiteral("mesh references missing material"));
                    return false;
                }
            }
        }

        Point3D placementTranslation;
        if (version >= 5) {
            const QJsonValue placementValue =
                serializedObject.value(QStringLiteral("placementTranslation"));
            if (!placementValue.isObject()) {
                setError(errorMessage, QStringLiteral("invalid object placement"));
                return false;
            }
            const QJsonObject placementObject = placementValue.toObject();
            const QJsonValue xValue = placementObject.value(QStringLiteral("x"));
            const QJsonValue yValue = placementObject.value(QStringLiteral("y"));
            const QJsonValue zValue = placementObject.value(QStringLiteral("z"));
            if (!xValue.isDouble() || !yValue.isDouble() || !zValue.isDouble() ||
                !std::isfinite(xValue.toDouble()) ||
                !std::isfinite(yValue.toDouble()) ||
                !std::isfinite(zValue.toDouble())) {
                setError(errorMessage, QStringLiteral("invalid object placement"));
                return false;
            }
            placementTranslation = {xValue.toDouble(), yValue.toDouble(),
                                    zValue.toDouble()};
            const bool hasSpatialPlacement =
                placementTranslation.x != 0.0 || placementTranslation.y != 0.0 ||
                placementTranslation.z != 0.0;
            if (hasSpatialPlacement &&
                shape.geometryType != GeometryType::NurbsSurface &&
                shape.geometryType != GeometryType::NurbsSolid) {
                setError(errorMessage,
                         QStringLiteral("placement is only supported for spatial NURBS objects"));
                return false;
            }
        }

        QVector4D displayColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
        WorkbenchMaterialData workbenchMaterial;
        if (version >= 6) {
            float displayColor[4] = {};
            if (!unitIntervalArrayFromJson(
                    serializedObject.value(
                        QStringLiteral("displayColorSceneLinear")),
                    4, displayColor) ||
                !workbenchMaterialFromJson(
                    serializedObject.value(QStringLiteral("workbenchMaterial")),
                    &workbenchMaterial)) {
                setError(errorMessage,
                         QStringLiteral("invalid scene object appearance"));
                return false;
            }
            displayColorSceneLinear = QVector4D(
                displayColor[0], displayColor[1], displayColor[2],
                displayColor[3]);
        }

        SceneObject sceneObject;
        sceneObject.id = ObjectId::fromValue(objectValueId);
        sceneObject.layerId = LayerId::fromValue(objectLayerValue);
        if (version >= 7) {
            const QJsonValue displayName = serializedObject.value(QStringLiteral("displayName"));
            const QJsonValue libraryPath = serializedObject.value(QStringLiteral("libraryPath"));
            if (!displayName.isString() || !libraryPath.isString()) {
                setError(errorMessage, QStringLiteral("invalid Blender object identity"));
                return false;
            }
            sceneObject.displayName = displayName.toString();
            sceneObject.libraryPath = libraryPath.toString();
        }
        if (version >= 9) {
            const QJsonValue sourceVersion =
                serializedObject.value(QStringLiteral("blenderSourceVersion"));
            const QJsonValue importMode =
                serializedObject.value(QStringLiteral("blenderMeshImportMode"));
            if (!sourceVersion.isString() || !importMode.isString() ||
                (!importMode.toString().isEmpty() &&
                 importMode.toString() != QStringLiteral("original") &&
                 importMode.toString() != QStringLiteral("evaluated"))) {
                setError(errorMessage, QStringLiteral("invalid Blender import metadata"));
                return false;
            }
            sceneObject.blenderSourceVersion = sourceVersion.toString();
            sceneObject.blenderMeshImportMode = importMode.toString();
        }
        if (shape.geometryType == GeometryType::PolygonMesh) {
            const QJsonValue transformValue =
                serializedObject.value(QStringLiteral("meshObjectTransform"));
            if (version < 8 || !transformValue.isArray() ||
                transformValue.toArray().size() != 16) {
                setError(errorMessage, QStringLiteral("invalid mesh object transform"));
                return false;
            }
            for (int index = 0; index < 16; ++index) {
                const QJsonValue component = transformValue.toArray()[index];
                if (!component.isDouble() || !std::isfinite(component.toDouble())) {
                    setError(errorMessage, QStringLiteral("invalid mesh object transform"));
                    return false;
                }
                sceneObject.meshObjectTransform[static_cast<size_t>(index)] =
                    component.toDouble();
            }
            const auto &matrix = sceneObject.meshObjectTransform;
            if (matrix[12] != 0.0 || matrix[13] != 0.0 || matrix[14] != 0.0 ||
                matrix[15] != 1.0) {
                setError(errorMessage, QStringLiteral("mesh transform must be affine"));
                return false;
            }
        }
        sceneObject.geometry = std::move(shape);
        sceneObject.placementTranslation = placementTranslation;
        sceneObject.displayColorSceneLinear = displayColorSceneLinear;
        sceneObject.workbenchMaterial = workbenchMaterial;
        snapshot.objects.append(std::move(sceneObject));
        objectIds.insert(objectValueId);
        if (objectValueId < std::numeric_limits<quint64>::max()) {
            nextObjectValue = std::max(nextObjectValue, objectValueId + 1);
        }
    }

    snapshot.nextLayerValue = nextLayerValue;
    snapshot.nextObjectValue = nextObjectValue;
    document->restoreSnapshot(snapshot);
    if (document->activeLayerId() != snapshot.activeLayerId) {
        setError(errorMessage, QStringLiteral("restored active layer is invalid"));
        return false;
    }
    return true;
}

} // namespace classiCAD
