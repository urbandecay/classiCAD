#include "document_serializer.h"

#include "core/serialization/shape_json_codec.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

namespace classiCAD {

namespace {

constexpr qsizetype kColorLutTotalByteLimit = 32 * 1024 * 1024;
constexpr quint64 kSceneLinearImageByteLimit = 32ULL * 1024 * 1024;

bool readDisplayBoundsPoint(const QJsonValue &value, Point3D *point)
{
    if (point == nullptr || !value.isArray()) return false;
    const QJsonArray values = value.toArray();
    if (values.size() != 3 || !values[0].isDouble() ||
        !values[1].isDouble() || !values[2].isDouble() ||
        !std::isfinite(values[0].toDouble()) ||
        !std::isfinite(values[1].toDouble()) ||
        !std::isfinite(values[2].toDouble())) return false;
    *point = {values[0].toDouble(), values[1].toDouble(), values[2].toDouble()};
    return true;
}

bool readDisplayBoundsTransform(const QJsonValue &value,
                               MeshObjectTransform *transform)
{
    if (transform == nullptr || !value.isArray()) return false;
    const QJsonArray rows = value.toArray();
    if (rows.size() != 4) return false;
    for (int row = 0; row < 4; ++row) {
        if (!rows[row].isArray()) return false;
        const QJsonArray columns = rows[row].toArray();
        if (columns.size() != 4) return false;
        for (int column = 0; column < 4; ++column) {
            if (!columns[column].isDouble() ||
                !std::isfinite(columns[column].toDouble())) return false;
            (*transform)[static_cast<size_t>(row * 4 + column)] =
                columns[column].toDouble();
        }
    }
    return std::abs((*transform)[12]) <= 1.0e-9 &&
           std::abs((*transform)[13]) <= 1.0e-9 &&
           std::abs((*transform)[14]) <= 1.0e-9 &&
           std::abs((*transform)[15] - 1.0) <= 1.0e-9;
}

bool decodeFiniteSceneLinearFloatPayload(const QString &encoded,
                                         const QJsonValue &widthValue,
                                         const QJsonValue &heightValue,
                                         QByteArray *decoded,
                                         int *widthOut = nullptr,
                                         int *heightOut = nullptr)
{
    if (decoded == nullptr || !widthValue.isDouble() || !heightValue.isDouble())
        return false;
    const double widthNumber = widthValue.toDouble();
    const double heightNumber = heightValue.toDouble();
    if (!std::isfinite(widthNumber) || !std::isfinite(heightNumber) ||
        std::floor(widthNumber) != widthNumber ||
        std::floor(heightNumber) != heightNumber ||
        widthNumber < 1.0 || heightNumber < 1.0 ||
        widthNumber > 16384.0 || heightNumber > 16384.0)
        return false;
    const int width = static_cast<int>(widthNumber);
    const int height = static_cast<int>(heightNumber);
    const quint64 expected = quint64(width) * quint64(height) * 16ULL;
    if (expected > kSceneLinearImageByteLimit || encoded.size() > 44 * 1024 * 1024)
        return false;
    const QByteArray bytes = QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (quint64(bytes.size()) != expected) return false;
    const auto *source = reinterpret_cast<const uchar *>(bytes.constData());
    for (qsizetype offset = 0; offset < bytes.size(); offset += 4) {
        const quint32 bits = qFromLittleEndian<quint32>(source + offset);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) return false;
    }
    *decoded = bytes;
    if (widthOut != nullptr) *widthOut = width;
    if (heightOut != nullptr) *heightOut = height;
    return true;
}

bool isValidImportedColorLuts(
    const QHash<QString, QByteArray> &luts,
    const QHash<QString, ImportedBlenderColorLutMetadata> &metadata)
{
    if (luts.size() > 8 || metadata.size() != luts.size()) return false;
    qsizetype totalBytes = 0;
    for (auto it = luts.cbegin(); it != luts.cend(); ++it) {
        const QString &id = it.key();
        if (id.size() != 64 || !std::all_of(id.cbegin(), id.cend(), [](QChar c) {
                return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                       (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
            }) || !metadata.contains(it.key()) ||
            QString::fromLatin1(QCryptographicHash::hash(
                it.value(), QCryptographicHash::Sha256).toHex()) != id) {
            return false;
        }
        const ImportedBlenderColorLutMetadata &lut = metadata[it.key()];
        if (lut.size < 2 || lut.size > 140 ||
            !std::isfinite(lut.domainMin) || !std::isfinite(lut.domainMax) ||
            lut.domainMax <= lut.domainMin ||
            it.value().size() != qsizetype(lut.size) * lut.size * lut.size * 3 * 4)
            return false;
        totalBytes += it.value().size();
        if (totalBytes > kColorLutTotalByteLimit) return false;
    }
    return true;
}

bool isValidImportedBlenderSequencer(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject record = value.toObject();
    if (record.isEmpty()) return true;
    const QJsonValue workspaceName = record.value(
        QStringLiteral("workspace_name"));
    const QJsonValue sceneId = record.value(QStringLiteral("scene_id"));
    const QJsonValue sceneName = record.value(QStringLiteral("scene_name"));
    const QJsonValue strips = record.value(QStringLiteral("strips"));
    const QJsonValue truncated = record.value(QStringLiteral("truncated"));
    if (!workspaceName.isString() || workspaceName.toString().size() > 4096 ||
        !sceneId.isString() || sceneId.toString().size() > 4096 ||
        !sceneName.isString() || sceneName.toString().size() > 4096 ||
        !strips.isArray() || strips.toArray().size() > 10000 ||
        !truncated.isBool()) {
        return false;
    }
    int nodeCount = 0;
    const auto validateRows = [&](const auto &self, const QJsonArray &rows,
                                  int depth) -> bool {
        if (rows.isEmpty()) return true;
        if (depth > 128 || nodeCount + rows.size() > 20000) return false;
        for (const QJsonValue &rowValue : rows) {
            if (!rowValue.isObject()) return false;
            const QJsonObject row = rowValue.toObject();
            const QJsonValue kind = row.value(QStringLiteral("kind"));
            const QJsonValue name = row.value(QStringLiteral("name"));
            const QJsonValue type = row.value(QStringLiteral("type"));
            const QJsonValue filename = row.value(
                QStringLiteral("media_filename"));
            const QJsonValue directory = row.value(
                QStringLiteral("data_directory"));
            const QJsonValue meta = row.value(QStringLiteral("is_meta"));
            const QJsonValue effect = row.value(QStringLiteral("is_effect"));
            const QJsonValue selected = row.value(QStringLiteral("selected"));
            const QJsonValue children = row.value(QStringLiteral("children"));
            if (!kind.isString() ||
                (kind.toString() != QLatin1String("STRIP") &&
                 kind.toString() != QLatin1String("DUPLICATE")) ||
                !name.isString() || name.toString().size() > 4096 ||
                !type.isString() || type.toString().size() > 128 ||
                !filename.isString() || filename.toString().size() > 4096 ||
                !directory.isString() || directory.toString().size() > 4096 ||
                !meta.isBool() || !effect.isBool() || !selected.isBool() ||
                !children.isArray()) {
                return false;
            }
            ++nodeCount;
            if (!self(self, children.toArray(), depth + 1)) return false;
        }
        return true;
    };
    return validateRows(validateRows, strips.toArray(), 0);
}

bool isValidImportedBlenderDataApi(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject record = value.toObject();
    if (record.isEmpty()) return true;
    const QJsonValue root = record.value(QStringLiteral("root"));
    const QJsonValue truncated = record.value(QStringLiteral("truncated"));
    if (!root.isObject() || !truncated.isBool()) return false;
    int nodeCount = 0;
    const auto validateNode = [&](const auto &self,
                                  const QJsonObject &node,
                                  int depth) -> bool {
        if (depth > 64 || ++nodeCount > 20000) return false;
        const QJsonValue kind = node.value(QStringLiteral("kind"));
        const QJsonValue key = node.value(QStringLiteral("key"));
        const QJsonValue name = node.value(QStringLiteral("name"));
        const QJsonValue type = node.value(QStringLiteral("type"));
        const QJsonValue display = node.value(QStringLiteral("value"));
        const QJsonValue children = node.value(QStringLiteral("children"));
        if (!kind.isString() ||
            (kind.toString() != QLatin1String("STRUCT") &&
             kind.toString() != QLatin1String("PROPERTY") &&
             kind.toString() != QLatin1String("ARRAY_ELEM")) ||
            !key.isString() || key.toString().size() > 512 ||
            !name.isString() || name.toString().size() > 4096 ||
            !type.isString() || type.toString().size() > 256 ||
            !display.isString() || display.toString().size() > 4096 ||
            !children.isArray() || children.toArray().size() > 20000) {
            return false;
        }
        for (const QJsonValue &child : children.toArray()) {
            if (!child.isObject() ||
                !self(self, child.toObject(), depth + 1)) {
                return false;
            }
        }
        return true;
    };
    return validateNode(validateNode, root.toObject(), 0);
}

bool isValidImportedBlenderLibraryOverrides(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject record = value.toObject();
    if (record.isEmpty()) return true;
    const QJsonObject properties = record.value(
        QStringLiteral("properties")).toObject();
    const QJsonValue propertyGroups = properties.value(QStringLiteral("groups"));
    const QJsonValue hierarchies = record.value(QStringLiteral("hierarchies"));
    if (!record.value(QStringLiteral("truncated")).isBool() ||
        !record.value(QStringLiteral("has_libraries")).isBool() ||
        !properties.value(QStringLiteral("truncated")).isBool() ||
        !propertyGroups.isArray() || !hierarchies.isArray() ||
        propertyGroups.toArray().size() > 1024 || hierarchies.toArray().size() > 1024) {
        return false;
    }
    int propertyItemCount = 0;
    for (const QJsonValue &groupValue : propertyGroups.toArray()) {
        if (!groupValue.isObject()) return false;
        const QJsonObject group = groupValue.toObject();
        const QJsonValue items = group.value(QStringLiteral("items"));
        if (!group.value(QStringLiteral("name")).isString() ||
            group.value(QStringLiteral("name")).toString().size() > 256 ||
            !items.isArray()) return false;
        for (const QJsonValue &itemValue : items.toArray()) {
            if (!itemValue.isObject() || ++propertyItemCount > 20000) return false;
            const QJsonObject item = itemValue.toObject();
            const QStringList requiredStrings{
                QStringLiteral("id"), QStringLiteral("name"),
                QStringLiteral("type"), QStringLiteral("rna_type"),
                QStringLiteral("library_path"), QStringLiteral("reference_name"),
                QStringLiteral("reference_library_path"), QStringLiteral("root_id")};
            for (const QString &field : requiredStrings) {
                if (!item.value(field).isString() ||
                    item.value(field).toString().size() > 4096) return false;
            }
            const QJsonValue itemProperties = item.value(QStringLiteral("properties"));
            if (!item.value(QStringLiteral("is_root")).isBool() ||
                !item.value(QStringLiteral("is_system_id")).isBool() ||
                !itemProperties.isArray() || itemProperties.toArray().size() > 512) {
                return false;
            }
            for (const QJsonValue &propertyValue : itemProperties.toArray()) {
                if (!propertyValue.isObject()) return false;
                const QJsonObject property = propertyValue.toObject();
                for (const QString &field : {QStringLiteral("path"),
                                             QStringLiteral("name"),
                                             QStringLiteral("value")}) {
                    if (!property.value(field).isString() ||
                        property.value(field).toString().size() > 4096) return false;
                }
                const QJsonValue segments = property.value(QStringLiteral("segments"));
                const QJsonValue operations = property.value(QStringLiteral("operations"));
                if (!property.value(QStringLiteral("valid")).isBool() ||
                    !property.value(QStringLiteral("system")).isBool() ||
                    !segments.isArray() || segments.toArray().size() > 128 ||
                    !operations.isArray() || operations.toArray().size() > 64) return false;
                for (const QJsonValue &segmentValue : segments.toArray()) {
                    if (!segmentValue.isObject()) return false;
                    const QJsonObject segment = segmentValue.toObject();
                    const QString kind = segment.value(QStringLiteral("kind")).toString();
                    if (!segment.value(QStringLiteral("key")).isString() ||
                        segment.value(QStringLiteral("key")).toString().size() > 512 ||
                        !segment.value(QStringLiteral("name")).isString() ||
                        segment.value(QStringLiteral("name")).toString().size() > 4096 ||
                        (kind != QLatin1String("PROPERTY") &&
                         kind != QLatin1String("ITEM"))) return false;
                }
                for (const QJsonValue &operationValue : operations.toArray()) {
                    if (!operationValue.isObject()) return false;
                    const QJsonObject operation = operationValue.toObject();
                    if (!operation.value(QStringLiteral("type")).isString() ||
                        operation.value(QStringLiteral("type")).toString().size() > 128 ||
                        !operation.value(QStringLiteral("label")).isString() ||
                        operation.value(QStringLiteral("label")).toString().size() > 512) return false;
                }
            }
        }
    }

    int hierarchyNodeCount = 0;
    const auto validateNode = [&](const auto &self,
                                  const QJsonObject &node,
                                  int depth) -> bool {
        if (depth > 128 || ++hierarchyNodeCount > 20000) return false;
        const QJsonValue children = node.value(QStringLiteral("children"));
        for (const QString &field : {QStringLiteral("id"), QStringLiteral("name"),
                                     QStringLiteral("root_id")}) {
            if (!node.value(field).isString() ||
                node.value(field).toString().size() > 4096) return false;
        }
        if (!node.value(QStringLiteral("type")).isString() ||
            node.value(QStringLiteral("type")).toString().size() > 256 ||
            !children.isArray()) return false;
        for (const QJsonValue &child : children.toArray()) {
            if (!child.isObject() || !self(self, child.toObject(), depth + 1)) return false;
        }
        return true;
    };
    int currentFileCount = 0;
    for (const QJsonValue &hierarchyValue : hierarchies.toArray()) {
        if (!hierarchyValue.isObject()) return false;
        const QJsonObject hierarchy = hierarchyValue.toObject();
        const QJsonValue groups = hierarchy.value(QStringLiteral("groups"));
        const QJsonValue currentFile = hierarchy.value(QStringLiteral("current_file"));
        if (!hierarchy.value(QStringLiteral("id")).isString() ||
            hierarchy.value(QStringLiteral("id")).toString().size() > 4096 ||
            !hierarchy.value(QStringLiteral("name")).isString() ||
            hierarchy.value(QStringLiteral("name")).toString().size() > 4096 ||
            !currentFile.isBool() || !hierarchy.value(QStringLiteral("truncated")).isBool() ||
            !groups.isArray() || groups.toArray().size() > 1024) return false;
        currentFileCount += currentFile.toBool() ? 1 : 0;
        for (const QJsonValue &groupValue : groups.toArray()) {
            if (!groupValue.isObject()) return false;
            const QJsonObject group = groupValue.toObject();
            const QJsonValue items = group.value(QStringLiteral("items"));
            if (!group.value(QStringLiteral("name")).isString() ||
                group.value(QStringLiteral("name")).toString().size() > 256 ||
                !items.isArray()) return false;
            for (const QJsonValue &item : items.toArray()) {
                if (!item.isObject() || !validateNode(validateNode, item.toObject(), 0)) return false;
            }
        }
    }
    return currentFileCount == 1;
}

bool isValidImportedBlenderLibraryRows(const QJsonValue &value)
{
    if (!value.isArray() || value.toArray().size() > 1024) return false;
    int totalSlots = 0;
    int totalActionGroups = 0;
    int totalDataBlockGroups = 0;
    int totalDataBlocks = 0;
    int totalAnimationRows = 0;
    for (const QJsonValue &libraryValue : value.toArray()) {
        if (!libraryValue.isObject()) return false;
        const QJsonValue groupsValue = libraryValue.toObject().value(
            QStringLiteral("data_blocks"));
        if (!groupsValue.isArray() || groupsValue.toArray().size() > 1024) {
            return false;
        }
        for (const QJsonValue &groupValue : groupsValue.toArray()) {
            if (!groupValue.isObject() || ++totalDataBlockGroups > 20000) {
                return false;
            }
            const QJsonObject group = groupValue.toObject();
            const QJsonValue namesValue = group.value(QStringLiteral("names"));
            const QJsonValue actionSlotsValue = group.value(
                QStringLiteral("action_slots"));
            const QJsonValue animationsValue = group.value(
                QStringLiteral("animations"));
            if (!namesValue.isArray() || namesValue.toArray().size() > 20000) {
                return false;
            }
            totalDataBlocks += namesValue.toArray().size();
            if (totalDataBlocks > 20000) return false;
            QSet<QString> dataBlockNames;
            for (const QJsonValue &nameValue : namesValue.toArray()) {
                if (!nameValue.isString() || nameValue.toString().size() > 4096) {
                    return false;
                }
                dataBlockNames.insert(nameValue.toString());
            }

            if (!actionSlotsValue.isUndefined()) {
                if (!actionSlotsValue.isArray() ||
                    actionSlotsValue.toArray().size() > 20000 ||
                    totalActionGroups + actionSlotsValue.toArray().size() > 20000) {
                    return false;
                }
                QSet<QString> actionNames;
                for (const QJsonValue &actionValue : actionSlotsValue.toArray()) {
                    if (!actionValue.isObject() || ++totalActionGroups > 20000) {
                        return false;
                    }
                    const QJsonObject action = actionValue.toObject();
                    const QJsonValue actionNameValue = action.value(
                        QStringLiteral("action_name"));
                    const QJsonValue slotsValue = action.value(
                        QStringLiteral("slots"));
                    const QString actionName = actionNameValue.toString();
                    if (!actionNameValue.isString() || actionName.size() > 4096 ||
                        !dataBlockNames.contains(actionName) ||
                        actionNames.contains(actionName) || !slotsValue.isArray() ||
                        slotsValue.toArray().size() > 20000 ||
                        totalSlots + slotsValue.toArray().size() > 20000) {
                        return false;
                    }
                    actionNames.insert(actionName);
                    totalSlots += slotsValue.toArray().size();
                    for (const QJsonValue &slotValue : slotsValue.toArray()) {
                        if (!slotValue.isObject()) return false;
                        const QJsonObject slot = slotValue.toObject();
                        const QJsonValue name = slot.value(QStringLiteral("name"));
                        const QJsonValue identifier = slot.value(
                            QStringLiteral("identifier"));
                        if (!name.isString() || name.toString().isEmpty() ||
                            name.toString().size() > 4096 || !identifier.isString() ||
                            identifier.toString().size() < 3 ||
                            identifier.toString().size() > 256) return false;
                    }
                }
            }

            if (!animationsValue.isUndefined()) {
                if (!animationsValue.isArray() ||
                    animationsValue.toArray().size() > 20000) return false;
                QSet<QString> animatedDataBlockNames;
                for (const QJsonValue &animationValue : animationsValue.toArray()) {
                    if (!animationValue.isObject()) return false;
                    const QJsonObject animationRecord = animationValue.toObject();
                    const QJsonValue dataNameValue = animationRecord.value(
                        QStringLiteral("data_block_name"));
                    const QJsonValue metadataValue = animationRecord.value(
                        QStringLiteral("animation"));
                    const QString dataName = dataNameValue.toString();
                    if (!dataNameValue.isString() || dataName.size() > 4096 ||
                        !dataBlockNames.contains(dataName) ||
                        animatedDataBlockNames.contains(dataName) ||
                        !metadataValue.isObject()) return false;
                    animatedDataBlockNames.insert(dataName);
                    const QJsonObject metadata = metadataValue.toObject();
                    const QJsonValue actionName = metadata.value(
                        QStringLiteral("action_name"));
                    const QJsonValue actionSlotName = metadata.value(
                        QStringLiteral("action_slot_name"));
                    const QJsonValue driverNames = metadata.value(
                        QStringLiteral("driver_target_names"));
                    const QJsonValue tracks = metadata.value(
                        QStringLiteral("nla_tracks"));
                    if (!metadata.value(QStringLiteral("has_animation_data")).isBool() ||
                        !metadata.value(QStringLiteral("has_animation_data")).toBool() ||
                        !actionName.isString() || actionName.toString().size() > 4096 ||
                        !actionSlotName.isString() || actionSlotName.toString().size() > 4096 ||
                        !metadata.value(QStringLiteral("has_drivers")).isBool() ||
                        !metadata.value(QStringLiteral("truncated")).isBool() ||
                        !driverNames.isArray() || driverNames.toArray().size() > 1024 ||
                        !tracks.isArray() || tracks.toArray().size() > 1024) return false;
                    int animationRowCount = 1;
                    if (!actionName.toString().isEmpty()) {
                        ++animationRowCount;
                        if (!actionSlotName.toString().isEmpty()) ++animationRowCount;
                    } else if (!actionSlotName.toString().isEmpty()) {
                        return false;
                    }
                    if (metadata.value(QStringLiteral("has_drivers")).toBool()) {
                        animationRowCount += 1 + driverNames.toArray().size();
                    } else if (!driverNames.toArray().isEmpty()) {
                        return false;
                    }
                    for (const QJsonValue &targetName : driverNames.toArray()) {
                        if (!targetName.isString() ||
                            targetName.toString().size() > 4096) return false;
                    }
                    if (!tracks.toArray().isEmpty()) ++animationRowCount;
                    for (const QJsonValue &trackValue : tracks.toArray()) {
                        if (!trackValue.isObject()) return false;
                        const QJsonObject track = trackValue.toObject();
                        const QJsonValue trackName = track.value(
                            QStringLiteral("name"));
                        const QJsonValue actions = track.value(
                            QStringLiteral("action_names"));
                        if (!trackName.isString() || trackName.toString().size() > 4096 ||
                            !actions.isArray() || actions.toArray().size() > 1024) {
                            return false;
                        }
                        animationRowCount += 1 + actions.toArray().size();
                        for (const QJsonValue &actionNameValue : actions.toArray()) {
                            if (!actionNameValue.isString() ||
                                actionNameValue.toString().size() > 4096) return false;
                        }
                    }
                    if (animationRowCount > 1024 ||
                        totalAnimationRows + animationRowCount > 20000) return false;
                    totalAnimationRows += animationRowCount;
                }
            }
        }
    }
    return true;
}

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
    restored.textureImageInterpolation = object.value(
        QStringLiteral("textureImageInterpolation")).toString(QStringLiteral("Linear"));
    restored.textureImageExtension = object.value(
        QStringLiteral("textureImageExtension")).toString(QStringLiteral("REPEAT"));
    const QJsonValue imagePathValue = object.value(QStringLiteral("textureImagePath"));
    const QJsonValue packedImageValue = object.value(
        QStringLiteral("textureImagePackedDataBase64"));
    const QJsonValue alphaModeValue = object.value(
        QStringLiteral("textureImageAlphaMode"));
    if ((!imagePathValue.isUndefined() && !imagePathValue.isString()) ||
        (!packedImageValue.isUndefined() && !packedImageValue.isString()) ||
        (!alphaModeValue.isUndefined() && !alphaModeValue.isString())) {
        return false;
    }
    restored.textureImagePath = imagePathValue.toString();
    restored.textureImageAlphaMode = alphaModeValue.toString(
        QStringLiteral("STRAIGHT"));
    restored.textureImagePackedDataBase64 = packedImageValue.toString();
    if (restored.textureImagePackedDataBase64.size() > 44 * 1024 * 1024) {
        return false;
    }
    if (!restored.textureImagePackedDataBase64.isEmpty()) {
        const QByteArray packedBytes = QByteArray::fromBase64(
            restored.textureImagePackedDataBase64.toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        if (packedBytes.isEmpty() || packedBytes.size() > 32 * 1024 * 1024) {
            return false;
        }
    }
    restored.textureImageSrgb = object.value(QStringLiteral("textureImageSrgb")).toBool(true);
    restored.textureImageColorTransformId = object.value(
        QStringLiteral("textureImageColorTransformId")).toString(QStringLiteral("identity"));
    const QJsonValue floatImagePayload = object.value(
        QStringLiteral("textureImageSceneLinearFloatDataBase64"));
    if (!floatImagePayload.isUndefined() && !floatImagePayload.isString())
        return false;
    restored.textureImageSceneLinearFloatDataBase64 = floatImagePayload.toString();
    restored.textureImageSceneLinearFloatWidth = object.value(
        QStringLiteral("textureImageSceneLinearFloatWidth")).toInt();
    restored.textureImageSceneLinearFloatHeight = object.value(
        QStringLiteral("textureImageSceneLinearFloatHeight")).toInt();
    if (!restored.textureImageSceneLinearFloatDataBase64.isEmpty()) {
        QByteArray payload;
        if (!decodeFiniteSceneLinearFloatPayload(
                restored.textureImageSceneLinearFloatDataBase64,
                object.value(QStringLiteral("textureImageSceneLinearFloatWidth")),
                object.value(QStringLiteral("textureImageSceneLinearFloatHeight")),
                &payload)) return false;
    }
    const QJsonValue tilesValue = object.value(QStringLiteral("textureImageTiles"));
    if (!tilesValue.isUndefined()) {
        if (!tilesValue.isArray()) return false;
        qsizetype packedTileBytes = 0;
        for (const QJsonValue &tileValue : tilesValue.toArray()) {
            if (!tileValue.isObject()) return false;
            const QJsonObject tileObject = tileValue.toObject();
            const QJsonValue numberValue = tileObject.value(QStringLiteral("number"));
            const QJsonValue filepathValue = tileObject.value(QStringLiteral("filepath"));
            const QJsonValue existsValue = tileObject.value(QStringLiteral("sourceFileExists"));
            const QJsonValue packedDataValue = tileObject.value(QStringLiteral("packedDataBase64"));
            const double numberNumber = numberValue.toDouble(-1.0);
            const int number = numberValue.isDouble() && std::isfinite(numberNumber) &&
                    std::floor(numberNumber) == numberNumber &&
                    numberNumber >= 1001.0 && numberNumber <= 1999.0
                ? static_cast<int>(numberNumber) : -1;
            if (number < 1001 || number > 1999 ||
                !filepathValue.isString() || !existsValue.isBool() ||
                (!packedDataValue.isUndefined() && !packedDataValue.isString())) return false;
            const QString packedData = packedDataValue.toString();
            if (packedData.size() > 44 * 1024 * 1024)
                return false;
            const QByteArray decoded = packedData.isEmpty() ? QByteArray{} :
                QByteArray::fromBase64(packedData.toLatin1(),
                    QByteArray::AbortOnBase64DecodingErrors);
            if (!packedData.isEmpty() && decoded.isEmpty()) return false;
            packedTileBytes += decoded.size();
            if (packedTileBytes > 32 * 1024 * 1024) return false;
            DocumentImageTextureTile tile{number, filepathValue.toString(),
                                           existsValue.toBool(), packedData,
                                           {}, 0, 0};
            const QJsonValue floatTilePayload = tileObject.value(
                QStringLiteral("sceneLinearFloatDataBase64"));
            if (!floatTilePayload.isUndefined() && !floatTilePayload.isString())
                return false;
            tile.sceneLinearFloatDataBase64 = floatTilePayload.toString();
            tile.sceneLinearFloatWidth = tileObject.value(
                QStringLiteral("sceneLinearFloatWidth")).toInt();
            tile.sceneLinearFloatHeight = tileObject.value(
                QStringLiteral("sceneLinearFloatHeight")).toInt();
            if (!tile.sceneLinearFloatDataBase64.isEmpty()) {
                QByteArray floatPayload;
                if (!decodeFiniteSceneLinearFloatPayload(
                        tile.sceneLinearFloatDataBase64,
                        tileObject.value(QStringLiteral("sceneLinearFloatWidth")),
                        tileObject.value(QStringLiteral("sceneLinearFloatHeight")),
                        &floatPayload)) return false;
            }
            restored.textureImageTiles.append(std::move(tile));
        }
    }
    *material = restored;
    return true;
}

} // namespace

QJsonObject documentToJson(const Document &document)
{
    QJsonObject serialized;
    serialized.insert(QStringLiteral("version"), 46);
    QJsonObject settings;
    settings.insert(QStringLiteral("lengthUnit"),
                    documentLengthUnitKey(document.settings().lengthUnit));
    settings.insert(QStringLiteral("gridSpacing"), document.settings().gridSpacing);
    settings.insert(QStringLiteral("activeViewLayerKey"),
                    document.settings().activeViewLayerKey);
    settings.insert(QStringLiteral("importedBlenderScenes"),
                    document.settings().importedBlenderScenes);
    settings.insert(QStringLiteral("importedBlenderLibraries"),
                    document.settings().importedBlenderLibraries);
    settings.insert(QStringLiteral("importedBlenderOrphans"),
                    document.settings().importedBlenderOrphans);
    settings.insert(QStringLiteral("importedBlenderSequencer"),
                    document.settings().importedBlenderSequencer);
    settings.insert(QStringLiteral("importedBlenderDataApi"),
                    document.settings().importedBlenderDataApi);
    settings.insert(QStringLiteral("importedBlenderLibraryOverrides"),
                    document.settings().importedBlenderLibraryOverrides);
    settings.insert(QStringLiteral("importedBlenderOutlinerOnlyObjects"),
                    document.settings().importedBlenderOutlinerOnlyObjects);
    QJsonObject colorLuts;
    for (auto it = document.settings().importedBlenderColorLuts.cbegin();
         it != document.settings().importedBlenderColorLuts.cend(); ++it) {
        const ImportedBlenderColorLutMetadata metadata =
            document.settings().importedBlenderColorLutMetadata.value(it.key());
        QJsonObject record;
        record.insert(QStringLiteral("data_base64"),
                      QString::fromLatin1(it.value().toBase64()));
        record.insert(QStringLiteral("size"), metadata.size);
        record.insert(QStringLiteral("domain_min"), metadata.domainMin);
        record.insert(QStringLiteral("domain_max"), metadata.domainMax);
        record.insert(QStringLiteral("shaper"), metadata.shaper);
        record.insert(QStringLiteral("shaper_scale"), metadata.shaperScale);
        colorLuts.insert(it.key(), record);
    }
    settings.insert(QStringLiteral("importedBlenderColorLuts"), colorLuts);
    serialized.insert(QStringLiteral("settings"), settings);
    QJsonObject outlinerExpandedPathsByScene;
    for (auto it = document.outlinerExpandedPathsByScene().cbegin();
         it != document.outlinerExpandedPathsByScene().cend(); ++it) {
        QJsonArray paths;
        for (const QString &path : it.value()) paths.append(path);
        outlinerExpandedPathsByScene.insert(it.key(), paths);
    }
    serialized.insert(QStringLiteral("outlinerExpandedPathsByScene"),
                      outlinerExpandedPathsByScene);
    QJsonArray collections;
    for (const DocumentCollection &collection : document.collections()) {
        QJsonObject record;
        record.insert(QStringLiteral("id"), idToJson(collection.id.value()));
        record.insert(QStringLiteral("name"), collection.name);
        record.insert(QStringLiteral("sourceId"), collection.sourceId);
        record.insert(QStringLiteral("sourceSceneName"), collection.sourceSceneName);
        QJsonArray sourceSceneNames;
        for (const QString &sceneName : collection.sourceSceneNames) {
            sourceSceneNames.append(sceneName);
        }
        record.insert(QStringLiteral("sourceSceneNames"), sourceSceneNames);
        record.insert(QStringLiteral("sourceSceneLibraryPath"),
                      collection.sourceSceneLibraryPath);
        record.insert(QStringLiteral("libraryPath"), collection.libraryPath);
        QJsonArray parentIds;
        for (const CollectionId parentId : collection.parentIds) {
            parentIds.append(idToJson(parentId.value()));
        }
        record.insert(QStringLiteral("parentIds"), parentIds);
        record.insert(QStringLiteral("hideViewport"), collection.hideViewport);
        record.insert(QStringLiteral("hideRender"), collection.hideRender);
        record.insert(QStringLiteral("hideSelect"), collection.hideSelect);
        record.insert(QStringLiteral("viewLayerStates"), collection.viewLayerStates);
        collections.append(record);
    }
    serialized.insert(QStringLiteral("collections"), collections);

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
            image.insert(QStringLiteral("nodeType"), reference.nodeType);
            image.insert(QStringLiteral("imageName"), reference.imageName);
            image.insert(QStringLiteral("imageLibraryPath"), reference.imageLibraryPath);
            image.insert(QStringLiteral("filepath"), reference.filepath);
            image.insert(QStringLiteral("colorSpace"), reference.colorSpace);
            image.insert(QStringLiteral("sourceTransformId"), reference.sourceTransformId);
            image.insert(QStringLiteral("sceneLinearFloatDataBase64"),
                         reference.sceneLinearFloatDataBase64);
            image.insert(QStringLiteral("sceneLinearFloatWidth"),
                         reference.sceneLinearFloatWidth);
            image.insert(QStringLiteral("sceneLinearFloatHeight"),
                         reference.sceneLinearFloatHeight);
            image.insert(QStringLiteral("alphaMode"), reference.alphaMode);
            image.insert(QStringLiteral("interpolation"), reference.interpolation);
            image.insert(QStringLiteral("extension"), reference.extension);
            image.insert(QStringLiteral("packedDataBase64"), reference.packedDataBase64);
            image.insert(QStringLiteral("packed"), reference.packed);
            image.insert(QStringLiteral("sourceFileExists"), reference.sourceFileExists);
            QJsonArray tiles;
            for (const DocumentImageTextureTile &tile : reference.tiles) {
                QJsonObject tileObject;
                tileObject.insert(QStringLiteral("number"), tile.number);
                tileObject.insert(QStringLiteral("filepath"), tile.filepath);
                tileObject.insert(QStringLiteral("sourceFileExists"), tile.sourceFileExists);
                tileObject.insert(QStringLiteral("packedDataBase64"), tile.packedDataBase64);
                tileObject.insert(QStringLiteral("sceneLinearFloatDataBase64"),
                                  tile.sceneLinearFloatDataBase64);
                tileObject.insert(QStringLiteral("sceneLinearFloatWidth"),
                                  tile.sceneLinearFloatWidth);
                tileObject.insert(QStringLiteral("sceneLinearFloatHeight"),
                                  tile.sceneLinearFloatHeight);
                tiles.append(tileObject);
            }
            image.insert(QStringLiteral("tiles"), tiles);
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
        QJsonArray collectionIds;
        for (const CollectionId collectionId : sceneObject.collectionIds) {
            collectionIds.append(idToJson(collectionId.value()));
        }
        serializedObject.insert(QStringLiteral("collectionIds"), collectionIds);
        serializedObject.insert(QStringLiteral("hideViewport"), sceneObject.hideViewport);
        serializedObject.insert(QStringLiteral("hideRender"), sceneObject.hideRender);
        serializedObject.insert(QStringLiteral("hiddenInViewport"), sceneObject.hiddenInViewport);
        serializedObject.insert(QStringLiteral("hideSelect"), sceneObject.hideSelect);
        serializedObject.insert(QStringLiteral("hideSurfacePick"),
                                sceneObject.hideSurfacePick);
        serializedObject.insert(QStringLiteral("showInFront"), sceneObject.showInFront);
        serializedObject.insert(QStringLiteral("showWire"), sceneObject.showWire);
        serializedObject.insert(QStringLiteral("showAllEdges"), sceneObject.showAllEdges);
        serializedObject.insert(QStringLiteral("wireDisplay"), sceneObject.wireDisplay);
        serializedObject.insert(QStringLiteral("blenderLooseMeshEdgeCompanion"),
                                sceneObject.blenderLooseMeshEdgeCompanion);
        QJsonArray looseEdgeSourceIndices;
        for (const int edgeIndex :
             sceneObject.blenderLooseMeshEdgeSourceIndices) {
            looseEdgeSourceIndices.append(edgeIndex);
        }
        serializedObject.insert(
            QStringLiteral("blenderLooseMeshEdgeSourceIndices"),
            looseEdgeSourceIndices);
        serializedObject.insert(QStringLiteral("blenderSourceObjectId"),
                                sceneObject.blenderSourceObjectId);
        serializedObject.insert(QStringLiteral("blenderParentObjectId"),
                                sceneObject.blenderParentObjectId);
        serializedObject.insert(QStringLiteral("displayBoundsType"),
                                sceneObject.displayBoundsType);
        serializedObject.insert(QStringLiteral("hasDisplayBoundsData"),
                                sceneObject.hasDisplayBoundsData);
        if (sceneObject.hasDisplayBoundsData) {
            serializedObject.insert(QStringLiteral("displayBoundsMinimumLocal"),
                QJsonArray{sceneObject.displayBoundsMinimumLocal.x,
                           sceneObject.displayBoundsMinimumLocal.y,
                           sceneObject.displayBoundsMinimumLocal.z});
            serializedObject.insert(QStringLiteral("displayBoundsMaximumLocal"),
                QJsonArray{sceneObject.displayBoundsMaximumLocal.x,
                           sceneObject.displayBoundsMaximumLocal.y,
                           sceneObject.displayBoundsMaximumLocal.z});
            QJsonArray boundsTransform;
            for (int row = 0; row < 4; ++row) {
                QJsonArray transformRow;
                for (int column = 0; column < 4; ++column) {
                    transformRow.append(sceneObject.displayBoundsTransformWorld[
                        static_cast<size_t>(row * 4 + column)]);
                }
                boundsTransform.append(transformRow);
            }
            serializedObject.insert(QStringLiteral("displayBoundsTransformWorld"),
                                    boundsTransform);
        }
        serializedObject.insert(QStringLiteral("viewLayerHidden"), sceneObject.viewLayerHidden);
        serializedObject.insert(QStringLiteral("defaultViewLayerKey"), sceneObject.defaultViewLayerKey);
        serializedObject.insert(QStringLiteral("displayName"), sceneObject.displayName);
        serializedObject.insert(QStringLiteral("libraryPath"), sceneObject.libraryPath);
        serializedObject.insert(QStringLiteral("workbenchRandomLibraryPath"),
                                sceneObject.workbenchRandomLibraryPath);
        serializedObject.insert(QStringLiteral("blenderSourceVersion"),
                                sceneObject.blenderSourceVersion);
        serializedObject.insert(QStringLiteral("blenderMeshImportMode"),
                                sceneObject.blenderMeshImportMode);
        serializedObject.insert(QStringLiteral("blenderSourceObjectType"),
                                sceneObject.blenderSourceObjectType);
        serializedObject.insert(QStringLiteral("blenderSourceDataName"),
                                sceneObject.blenderSourceDataName);
        serializedObject.insert(QStringLiteral("blenderSourceShapeKeyDataName"),
                                sceneObject.blenderSourceShapeKeyDataName);
        QJsonArray blenderSourceShapeKeyNames;
        for (const QString &name : sceneObject.blenderSourceShapeKeyNames) {
            blenderSourceShapeKeyNames.append(name);
        }
        serializedObject.insert(QStringLiteral("blenderSourceShapeKeyNames"),
                                blenderSourceShapeKeyNames);
        QJsonArray blenderSourceDataMaterialNames;
        for (const QString &name : sceneObject.blenderSourceDataMaterialNames) {
            blenderSourceDataMaterialNames.append(name);
        }
        serializedObject.insert(QStringLiteral("blenderSourceDataMaterialNames"),
                                blenderSourceDataMaterialNames);
        const auto animationDataToJson = [](
            const BlenderSourceAnimationData &animation) {
            QJsonArray driverNames;
            for (const QString &name : animation.driverTargetNames) {
                driverNames.append(name);
            }
            QJsonArray tracks;
            for (const BlenderSourceNlaTrack &track : animation.nlaTracks) {
                QJsonArray actions;
                for (const QString &name : track.actionNames) {
                    actions.append(name);
                }
                tracks.append(QJsonObject{
                    {QStringLiteral("name"), track.name},
                    {QStringLiteral("actionNames"), actions}});
            }
            return QJsonObject{
                {QStringLiteral("hasAnimationData"), animation.hasAnimationData},
                {QStringLiteral("actionName"), animation.actionName},
                {QStringLiteral("actionSlotName"), animation.actionSlotName},
                {QStringLiteral("hasDrivers"), animation.hasDrivers},
                {QStringLiteral("driverTargetNames"), driverNames},
                {QStringLiteral("nlaTracks"), tracks}};
        };
        QJsonArray dataDriverTargetNames;
        for (const QString &name :
             sceneObject.blenderSourceDataAnimation.driverTargetNames) {
            dataDriverTargetNames.append(name);
        }
        QJsonArray dataNlaTracks;
        for (const BlenderSourceNlaTrack &track :
             sceneObject.blenderSourceDataAnimation.nlaTracks) {
            QJsonArray actionNames;
            for (const QString &name : track.actionNames) {
                actionNames.append(name);
            }
            dataNlaTracks.append(QJsonObject{
                {QStringLiteral("name"), track.name},
                {QStringLiteral("actionNames"), actionNames}});
        }
        serializedObject.insert(QStringLiteral("blenderSourceDataAnimation"),
            QJsonObject{
                {QStringLiteral("hasAnimationData"),
                 sceneObject.blenderSourceDataAnimation.hasAnimationData},
                {QStringLiteral("actionName"),
                 sceneObject.blenderSourceDataAnimation.actionName},
                {QStringLiteral("actionSlotName"),
                 sceneObject.blenderSourceDataAnimation.actionSlotName},
                {QStringLiteral("hasDrivers"),
                 sceneObject.blenderSourceDataAnimation.hasDrivers},
                {QStringLiteral("driverTargetNames"), dataDriverTargetNames},
                {QStringLiteral("nlaTracks"), dataNlaTracks}});
        QJsonArray dataMaterialAnimations;
        for (const BlenderSourceAnimationData &animation :
             sceneObject.blenderSourceDataMaterialAnimations) {
            dataMaterialAnimations.append(animationDataToJson(animation));
        }
        serializedObject.insert(QStringLiteral("blenderSourceDataMaterialAnimations"),
                                dataMaterialAnimations);
        QJsonArray materialAnimations;
        for (const BlenderSourceAnimationData &animation :
             sceneObject.blenderSourceMaterialAnimations) {
            materialAnimations.append(animationDataToJson(animation));
        }
        serializedObject.insert(QStringLiteral("blenderSourceMaterialAnimations"),
                                materialAnimations);
        QJsonArray blenderSourceMaterialNames;
        for (const QString &name : sceneObject.blenderSourceMaterialNames) {
            blenderSourceMaterialNames.append(name);
        }
        serializedObject.insert(QStringLiteral("blenderSourceMaterialNames"),
                                blenderSourceMaterialNames);
        const auto insertBlenderNameList = [&serializedObject](
            const QString &key, const QStringList &names) {
            QJsonArray values;
            for (const QString &name : names) values.append(name);
            serializedObject.insert(key, values);
        };
        insertBlenderNameList(QStringLiteral("blenderSourceModifierNames"),
                              sceneObject.blenderSourceModifierNames);
        QJsonArray blenderSourceModifiers;
        for (const BlenderSourceModifier &modifier :
             sceneObject.blenderSourceModifiers) {
            blenderSourceModifiers.append(QJsonObject{
                {QStringLiteral("name"), modifier.name},
                {QStringLiteral("linkedName"), modifier.linkedName},
                {QStringLiteral("linkedType"), modifier.linkedType}});
        }
        serializedObject.insert(QStringLiteral("blenderSourceModifiers"),
                                blenderSourceModifiers);
        QJsonArray blenderSourceShaderEffects;
        for (const BlenderSourceShaderEffect &effect :
             sceneObject.blenderSourceShaderEffects) {
            blenderSourceShaderEffects.append(QJsonObject{
                {QStringLiteral("name"), effect.name},
                {QStringLiteral("linkedName"), effect.linkedName},
                {QStringLiteral("linkedType"), effect.linkedType}});
        }
        serializedObject.insert(QStringLiteral("blenderSourceShaderEffects"),
                                blenderSourceShaderEffects);
        serializedObject.insert(QStringLiteral("blenderSourceGreasePencilLayerTree"),
                                sceneObject.blenderSourceGreasePencilLayerTree);
        insertBlenderNameList(QStringLiteral("blenderSourceConstraintNames"),
                              sceneObject.blenderSourceConstraintNames);
        insertBlenderNameList(QStringLiteral("blenderSourceVertexGroupNames"),
                              sceneObject.blenderSourceVertexGroupNames);
        serializedObject.insert(QStringLiteral("blenderSourceHasAnimationData"),
                                sceneObject.blenderSourceHasAnimationData);
        serializedObject.insert(QStringLiteral("blenderSourceAnimationActionName"),
                                sceneObject.blenderSourceAnimationActionName);
        serializedObject.insert(
            QStringLiteral("blenderSourceAnimationActionSlotName"),
            sceneObject.blenderSourceAnimationActionSlotName);
        serializedObject.insert(QStringLiteral("blenderSourceHasDrivers"),
                                sceneObject.blenderSourceHasDrivers);
        QJsonArray driverTargetNames;
        for (const QString &name : sceneObject.blenderSourceDriverTargetNames) {
            driverTargetNames.append(name);
        }
        serializedObject.insert(QStringLiteral("blenderSourceDriverTargetNames"),
                                driverTargetNames);
        QJsonArray nlaTracks;
        for (const BlenderSourceNlaTrack &track :
             sceneObject.blenderSourceNlaTracks) {
            QJsonArray actionNames;
            for (const QString &name : track.actionNames) actionNames.append(name);
            nlaTracks.append(QJsonObject{
                {QStringLiteral("name"), track.name},
                {QStringLiteral("actionNames"), actionNames}});
        }
        serializedObject.insert(QStringLiteral("blenderSourceNlaTracks"),
                                nlaTracks);
        serializedObject.insert(QStringLiteral("geometry"), shapeToJson(sceneObject.geometry));
        if (!sceneObject.blenderSurfaceWireVerticesWorld.isEmpty()) {
            QJsonArray wireVertices;
            for (const Point3D &point :
                 sceneObject.blenderSurfaceWireVerticesWorld) {
                wireVertices.append(QJsonArray{point.x, point.y, point.z});
            }
            serializedObject.insert(QStringLiteral("blenderSurfaceWireVerticesWorld"),
                                    wireVertices);
            serializedObject.insert(
                QStringLiteral("blenderSurfaceWireGeometryFingerprint"),
                QString::fromLatin1(
                    sceneObject.blenderSurfaceWireGeometryFingerprint));
        }
        QJsonObject solidFaceMaterials;
        QStringList solidFaceIds = sceneObject.nurbsSolidFaceMaterialIds.keys();
        std::sort(solidFaceIds.begin(), solidFaceIds.end());
        for (const QString &faceId : solidFaceIds) {
            solidFaceMaterials.insert(faceId,
                                      sceneObject.nurbsSolidFaceMaterialIds.value(faceId));
        }
        serializedObject.insert(QStringLiteral("nurbsSolidFaceMaterialIds"),
                                solidFaceMaterials);
        QJsonObject placement;
        placement.insert(QStringLiteral("x"), sceneObject.placementTranslation.x);
        placement.insert(QStringLiteral("y"), sceneObject.placementTranslation.y);
        placement.insert(QStringLiteral("z"), sceneObject.placementTranslation.z);
        serializedObject.insert(QStringLiteral("placementTranslation"), placement);
        serializedObject.insert(
            QStringLiteral("displayColorSceneLinear"),
            vector4ToJson(sceneObject.displayColorSceneLinear));
        serializedObject.insert(QStringLiteral("hasWorkbenchMaterial"),
                                sceneObject.hasWorkbenchMaterial);
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
        workbenchMaterial.insert(QStringLiteral("textureImagePath"),
                                 sceneObject.workbenchMaterial.textureImagePath);
        workbenchMaterial.insert(QStringLiteral("textureImagePackedDataBase64"),
                                 sceneObject.workbenchMaterial.textureImagePackedDataBase64);
        workbenchMaterial.insert(QStringLiteral("textureImageAlphaMode"),
                                 sceneObject.workbenchMaterial.textureImageAlphaMode);
        workbenchMaterial.insert(QStringLiteral("textureImageInterpolation"),
                                 sceneObject.workbenchMaterial.textureImageInterpolation);
        workbenchMaterial.insert(QStringLiteral("textureImageExtension"),
                                 sceneObject.workbenchMaterial.textureImageExtension);
        workbenchMaterial.insert(QStringLiteral("textureImageSrgb"),
                                 sceneObject.workbenchMaterial.textureImageSrgb);
        workbenchMaterial.insert(QStringLiteral("textureImageColorTransformId"),
                                 sceneObject.workbenchMaterial.textureImageColorTransformId);
        workbenchMaterial.insert(QStringLiteral("textureImageSceneLinearFloatDataBase64"),
                                 sceneObject.workbenchMaterial.textureImageSceneLinearFloatDataBase64);
        workbenchMaterial.insert(QStringLiteral("textureImageSceneLinearFloatWidth"),
                                 sceneObject.workbenchMaterial.textureImageSceneLinearFloatWidth);
        workbenchMaterial.insert(QStringLiteral("textureImageSceneLinearFloatHeight"),
                                 sceneObject.workbenchMaterial.textureImageSceneLinearFloatHeight);
        QJsonArray textureImageTiles;
        for (const DocumentImageTextureTile &tile :
             sceneObject.workbenchMaterial.textureImageTiles) {
            QJsonObject tileObject;
            tileObject.insert(QStringLiteral("number"), tile.number);
            tileObject.insert(QStringLiteral("filepath"), tile.filepath);
            tileObject.insert(QStringLiteral("sourceFileExists"), tile.sourceFileExists);
            tileObject.insert(QStringLiteral("packedDataBase64"), tile.packedDataBase64);
            tileObject.insert(QStringLiteral("sceneLinearFloatDataBase64"),
                              tile.sceneLinearFloatDataBase64);
            tileObject.insert(QStringLiteral("sceneLinearFloatWidth"),
                              tile.sceneLinearFloatWidth);
            tileObject.insert(QStringLiteral("sceneLinearFloatHeight"),
                              tile.sceneLinearFloatHeight);
            textureImageTiles.append(tileObject);
        }
        workbenchMaterial.insert(QStringLiteral("textureImageTiles"), textureImageTiles);
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
    if (version < 1 || version > 46) {
        setError(errorMessage, QStringLiteral("unsupported document version"));
        return false;
    }

    QHash<QString, QStringList> outlinerExpandedPathsByScene;
    if (version >= 27) {
        const QJsonValue outlinerStateValue = serialized.value(
            QStringLiteral("outlinerExpandedPathsByScene"));
        if (!outlinerStateValue.isObject()) {
            setError(errorMessage,
                     QStringLiteral("invalid saved Outliner expansion state"));
            return false;
        }
        const QJsonObject outlinerState = outlinerStateValue.toObject();
        for (auto it = outlinerState.begin(); it != outlinerState.end(); ++it) {
            if (!it.value().isArray()) {
                setError(errorMessage,
                         QStringLiteral("invalid saved Outliner expansion paths"));
                return false;
            }
            QStringList paths;
            for (const QJsonValue &pathValue : it.value().toArray()) {
                if (!pathValue.isString()) {
                    setError(errorMessage,
                             QStringLiteral("invalid saved Outliner expansion path"));
                    return false;
                }
                paths.append(pathValue.toString());
            }
            outlinerExpandedPathsByScene.insert(it.key(), paths);
        }
    }
    Document validatedOutlinerState;
    if (!validatedOutlinerState.setOutlinerExpandedPathsByScene(
            outlinerExpandedPathsByScene)) {
        setError(errorMessage,
                 QStringLiteral("saved Outliner expansion state exceeds its limits"));
        return false;
    }

    const QJsonValue objectsValue = serialized.value(QStringLiteral("objects"));
    if (!objectsValue.isArray()) {
        setError(errorMessage, QStringLiteral("document objects must be an array"));
        return false;
    }

    Document::Snapshot snapshot;
    QSet<QString> materialIds;
    QSet<quint64> collectionIds;
    if (version >= 10) {
        const QJsonValue collectionsValue = serialized.value(QStringLiteral("collections"));
        if (!collectionsValue.isArray()) {
            setError(errorMessage, QStringLiteral("document collections must be an array"));
            return false;
        }
        quint64 nextCollectionValue = 1;
        for (const QJsonValue &collectionValue : collectionsValue.toArray()) {
            if (!collectionValue.isObject()) {
                setError(errorMessage, QStringLiteral("invalid document collection"));
                return false;
            }
            const QJsonObject record = collectionValue.toObject();
            quint64 collectionValueId = 0;
            const QString name = record.value(QStringLiteral("name")).toString().trimmed();
            const QJsonValue sourceId = record.value(QStringLiteral("sourceId"));
            const QJsonValue sourceSceneNamesValue =
                record.value(QStringLiteral("sourceSceneNames"));
            const QJsonValue parentIdsValue = record.value(QStringLiteral("parentIds"));
            const QJsonValue sourceSceneName = record.value(QStringLiteral("sourceSceneName"));
            const QJsonValue sourceSceneLibraryPath =
                record.value(QStringLiteral("sourceSceneLibraryPath"));
            const QJsonValue libraryPath = record.value(QStringLiteral("libraryPath"));
            if (!idFromJson(record.value(QStringLiteral("id")), &collectionValueId) ||
                collectionIds.contains(collectionValueId) || name.isEmpty() ||
                !parentIdsValue.isArray() || !sourceId.isString() ||
                !sourceSceneNamesValue.isArray() || !sourceSceneName.isString() ||
                !sourceSceneLibraryPath.isString() || !libraryPath.isString() ||
                !record.value(QStringLiteral("hideViewport")).isBool() ||
                !record.value(QStringLiteral("hideRender")).isBool() ||
                (version >= 24 &&
                 !record.value(QStringLiteral("hideSelect")).isBool())) {
                setError(errorMessage, QStringLiteral("invalid document collection"));
                return false;
            }
            DocumentCollection collection;
            collection.id = CollectionId::fromValue(collectionValueId);
            collection.name = name;
            collection.sourceId = sourceId.toString();
            collection.sourceSceneName = sourceSceneName.toString();
            for (const QJsonValue &sceneNameValue : sourceSceneNamesValue.toArray()) {
                if (!sceneNameValue.isString() || sceneNameValue.toString().isEmpty()) {
                    setError(errorMessage, QStringLiteral("invalid collection scene name"));
                    return false;
                }
                collection.sourceSceneNames.append(sceneNameValue.toString());
            }
            collection.sourceSceneLibraryPath = sourceSceneLibraryPath.toString();
            collection.libraryPath = libraryPath.toString();
            collection.hideViewport = record.value(QStringLiteral("hideViewport")).toBool();
            collection.hideRender = record.value(QStringLiteral("hideRender")).toBool();
            collection.hideSelect = version >= 24 &&
                                    record.value(QStringLiteral("hideSelect")).toBool();
            if (version >= 12) {
                const QJsonValue states = record.value(QStringLiteral("viewLayerStates"));
                if (!states.isArray()) {
                    setError(errorMessage, QStringLiteral("invalid collection view-layer states"));
                    return false;
                }
                collection.viewLayerStates = states.toArray();
            }
            for (const QJsonValue &parentValue : parentIdsValue.toArray()) {
                quint64 parentId = 0;
                if (!idFromJson(parentValue, &parentId)) {
                    setError(errorMessage, QStringLiteral("invalid collection parent id"));
                    return false;
                }
                collection.parentIds.append(CollectionId::fromValue(parentId));
            }
            snapshot.collections.append(std::move(collection));
            collectionIds.insert(collectionValueId);
            if (collectionValueId < std::numeric_limits<quint64>::max()) {
                nextCollectionValue = std::max(nextCollectionValue, collectionValueId + 1);
            }
        }
        // Check references after loading all records so collection order is immaterial.
        for (const DocumentCollection &collection : snapshot.collections) {
            QSet<quint64> parents;
            for (const CollectionId parentId : collection.parentIds) {
                if (!collectionIds.contains(parentId.value()) ||
                    parentId == collection.id || parents.contains(parentId.value())) {
                    setError(errorMessage, QStringLiteral("invalid collection parent reference"));
                    return false;
                }
                parents.insert(parentId.value());
            }
        }
        QSet<quint64> visiting;
        QSet<quint64> visited;
        const auto hasCycle = [&](const auto &self, quint64 id) -> bool {
            if (visiting.contains(id)) return true;
            if (visited.contains(id)) return false;
            visiting.insert(id);
            const auto record = std::find_if(
                snapshot.collections.cbegin(), snapshot.collections.cend(),
                [id](const DocumentCollection &item) { return item.id.value() == id; });
            if (record != snapshot.collections.cend()) {
                for (const CollectionId parentId : record->parentIds) {
                    if (self(self, parentId.value())) return true;
                }
            }
            visiting.remove(id);
            visited.insert(id);
            return false;
        };
        for (const DocumentCollection &collection : snapshot.collections) {
            if (hasCycle(hasCycle, collection.id.value())) {
                setError(errorMessage, QStringLiteral("collection hierarchy contains a cycle"));
                return false;
            }
        }
        snapshot.nextCollectionValue = nextCollectionValue;
    }
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
                    const QStringList stringKeys{QStringLiteral("nodeName"),
                        QStringLiteral("imageName"),
                        QStringLiteral("imageLibraryPath"), QStringLiteral("filepath"),
                        QStringLiteral("colorSpace")};
                    bool validImage = true;
                    for (const QString &key : stringKeys) validImage &= image.value(key).isString();
                    const QJsonValue packedDataValue = image.value(QStringLiteral("packedDataBase64"));
                    if ((!image.value(QStringLiteral("nodeType")).isUndefined() &&
                         !image.value(QStringLiteral("nodeType")).isString()) ||
                        !validImage || !image.value(QStringLiteral("packed")).isBool() ||
                        !image.value(QStringLiteral("sourceFileExists")).isBool() ||
                        (!packedDataValue.isUndefined() && !packedDataValue.isString()) ||
                        image.value(QStringLiteral("imageName")).toString().isEmpty()) {
                        setError(errorMessage, QStringLiteral("invalid document image texture reference"));
                        return false;
                    }
                    DocumentImageTextureReference reference;
                    reference.nodeName = image.value(QStringLiteral("nodeName")).toString();
                    reference.nodeType = image.value(QStringLiteral("nodeType"))
                                             .toString(QStringLiteral("TEX_IMAGE"));
                    reference.imageName = image.value(QStringLiteral("imageName")).toString();
                    reference.imageLibraryPath = image.value(QStringLiteral("imageLibraryPath")).toString();
                    reference.filepath = image.value(QStringLiteral("filepath")).toString();
                    reference.colorSpace = image.value(QStringLiteral("colorSpace")).toString();
                    reference.sourceTransformId = image.value(
                        QStringLiteral("sourceTransformId")).toString(QStringLiteral("identity"));
                    const QJsonValue floatImagePayload = image.value(
                        QStringLiteral("sceneLinearFloatDataBase64"));
                    if (!floatImagePayload.isUndefined() &&
                        !floatImagePayload.isString()) {
                        setError(errorMessage,
                                 QStringLiteral("invalid scene-linear float image payload"));
                        return false;
                    }
                    reference.sceneLinearFloatDataBase64 = floatImagePayload.toString();
                    if (!reference.sceneLinearFloatDataBase64.isEmpty()) {
                        QByteArray floatPayload;
                        if (!decodeFiniteSceneLinearFloatPayload(
                                reference.sceneLinearFloatDataBase64,
                                image.value(QStringLiteral("sceneLinearFloatWidth")),
                                image.value(QStringLiteral("sceneLinearFloatHeight")),
                                &floatPayload,
                                &reference.sceneLinearFloatWidth,
                                &reference.sceneLinearFloatHeight)) {
                            setError(errorMessage, QStringLiteral("invalid scene-linear float image payload"));
                            return false;
                        }
                    }
                    reference.alphaMode = image.value(QStringLiteral("alphaMode"))
                                              .toString(QStringLiteral("STRAIGHT"));
                    reference.interpolation = image.value(QStringLiteral("interpolation"))
                                                  .toString(QStringLiteral("Linear"));
                    reference.extension = image.value(QStringLiteral("extension"))
                                             .toString(QStringLiteral("REPEAT"));
                    reference.packedDataBase64 = image.value(QStringLiteral("packedDataBase64"))
                                                     .toString();
                    if (reference.packedDataBase64.size() > 44 * 1024 * 1024 ||
                        QByteArray::fromBase64(reference.packedDataBase64.toLatin1()).size() >
                            32 * 1024 * 1024) {
                        setError(errorMessage,
                                 QStringLiteral("document packed image exceeds the size limit"));
                        return false;
                    }
                    reference.packed = image.value(QStringLiteral("packed")).toBool();
                    reference.sourceFileExists = image.value(QStringLiteral("sourceFileExists")).toBool();
                    const QJsonValue tilesValue = image.value(QStringLiteral("tiles"));
                    if (!tilesValue.isUndefined() && !tilesValue.isArray()) {
                        setError(errorMessage,
                                 QStringLiteral("invalid document image texture tiles"));
                        return false;
                    }
                    qsizetype packedTileBytes = 0;
                    for (const QJsonValue &tileValue : tilesValue.toArray()) {
                        if (!tileValue.isObject()) {
                            setError(errorMessage,
                                     QStringLiteral("invalid document image texture tile"));
                            return false;
                        }
                        const QJsonObject tileObject = tileValue.toObject();
                        const QJsonValue numberValue = tileObject.value(QStringLiteral("number"));
                        const QJsonValue filepathValue = tileObject.value(QStringLiteral("filepath"));
                        const QJsonValue existsValue = tileObject.value(QStringLiteral("sourceFileExists"));
                        const QJsonValue packedDataValue = tileObject.value(
                            QStringLiteral("packedDataBase64"));
                        const double numberNumber = numberValue.toDouble(-1.0);
                        const int number = numberValue.isDouble() &&
                                std::isfinite(numberNumber) &&
                                std::floor(numberNumber) == numberNumber &&
                                numberNumber >= 1001.0 && numberNumber <= 1999.0
                            ? static_cast<int>(numberNumber) : -1;
                        if (number < 1001 || number > 1999 ||
                            !filepathValue.isString() || !existsValue.isBool() ||
                            (!packedDataValue.isUndefined() && !packedDataValue.isString())) {
                            setError(errorMessage,
                                     QStringLiteral("invalid document image texture tile"));
                            return false;
                        }
                        const QString packedData = packedDataValue.toString();
                        if (packedData.size() > 44 * 1024 * 1024) {
                            setError(errorMessage,
                                     QStringLiteral("document packed UDIM tile exceeds the size limit"));
                            return false;
                        }
                        const QByteArray decoded = packedData.isEmpty() ? QByteArray{} :
                            QByteArray::fromBase64(packedData.toLatin1(),
                                QByteArray::AbortOnBase64DecodingErrors);
                        if (!packedData.isEmpty() && decoded.isEmpty()) {
                            setError(errorMessage,
                                     QStringLiteral("invalid document packed UDIM tile"));
                            return false;
                        }
                        packedTileBytes += decoded.size();
                        if (packedTileBytes > 32 * 1024 * 1024) {
                            setError(errorMessage,
                                     QStringLiteral("document packed UDIM tiles exceed the per-image size limit"));
                            return false;
                        }
                        DocumentImageTextureTile tile{number, filepathValue.toString(),
                                                      existsValue.toBool(), packedData,
                                                      {}, 0, 0};
                        const QJsonValue floatTilePayload = tileObject.value(
                            QStringLiteral("sceneLinearFloatDataBase64"));
                        if (!floatTilePayload.isUndefined() &&
                            !floatTilePayload.isString()) {
                            setError(errorMessage,
                                     QStringLiteral("invalid scene-linear float UDIM payload"));
                            return false;
                        }
                        tile.sceneLinearFloatDataBase64 = floatTilePayload.toString();
                        if (!tile.sceneLinearFloatDataBase64.isEmpty()) {
                            QByteArray floatPayload;
                            if (!decodeFiniteSceneLinearFloatPayload(
                                    tile.sceneLinearFloatDataBase64,
                                    tileObject.value(QStringLiteral("sceneLinearFloatWidth")),
                                    tileObject.value(QStringLiteral("sceneLinearFloatHeight")),
                                    &floatPayload,
                                    &tile.sceneLinearFloatWidth,
                                    &tile.sceneLinearFloatHeight)) {
                                setError(errorMessage, QStringLiteral("invalid scene-linear float UDIM payload"));
                                return false;
                            }
                        }
                        reference.tiles.append(std::move(tile));
                    }
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
        if (version >= 13) {
            const QJsonValue activeViewLayerKey =
                settingsObject.value(QStringLiteral("activeViewLayerKey"));
            if (!activeViewLayerKey.isString()) {
                setError(errorMessage,
                         QStringLiteral("invalid active Blender view-layer setting"));
                return false;
            }
            settings.activeViewLayerKey = activeViewLayerKey.toString();
        }
        if (version >= 14) {
            const QJsonValue importedScenes =
                settingsObject.value(QStringLiteral("importedBlenderScenes"));
            if (!importedScenes.isArray()) {
                setError(errorMessage,
                         QStringLiteral("invalid imported Blender scene metadata"));
                return false;
            }
            settings.importedBlenderScenes = importedScenes.toArray();
        }
        if (version >= 40) {
            const QJsonValue importedLibraries = settingsObject.value(
                QStringLiteral("importedBlenderLibraries"));
            if (!importedLibraries.isArray() ||
                importedLibraries.toArray().size() > 1024 ||
                (version >= 45 &&
                 !isValidImportedBlenderLibraryRows(importedLibraries))) {
                setError(errorMessage,
                         QStringLiteral("invalid imported Blender library metadata"));
                return false;
            }
            settings.importedBlenderLibraries = importedLibraries.toArray();
        }
        if (version >= 41) {
            const QJsonValue importedOrphans = settingsObject.value(
                QStringLiteral("importedBlenderOrphans"));
            if (!importedOrphans.isArray() ||
                importedOrphans.toArray().size() > 1024) {
                setError(errorMessage,
                         QStringLiteral("invalid imported Blender Orphan Data metadata"));
                return false;
            }
            settings.importedBlenderOrphans = importedOrphans.toArray();
        }
        if (version >= 42) {
            const QJsonValue importedSequencer = settingsObject.value(
                QStringLiteral("importedBlenderSequencer"));
            if (!isValidImportedBlenderSequencer(importedSequencer)) {
                setError(errorMessage,
                         QStringLiteral("invalid imported Blender Sequencer metadata"));
                return false;
            }
            settings.importedBlenderSequencer = importedSequencer.toObject();
        }
        if (version >= 43) {
            const QJsonValue importedDataApi = settingsObject.value(
                QStringLiteral("importedBlenderDataApi"));
            if (!isValidImportedBlenderDataApi(importedDataApi)) {
                setError(errorMessage,
                         QStringLiteral("invalid imported Blender Data API metadata"));
                return false;
            }
            settings.importedBlenderDataApi = importedDataApi.toObject();
        }
        if (version >= 44) {
            const QJsonValue importedOverrides = settingsObject.value(
                QStringLiteral("importedBlenderLibraryOverrides"));
            if (!isValidImportedBlenderLibraryOverrides(importedOverrides)) {
                setError(errorMessage,
                         QStringLiteral("invalid imported Blender Library Overrides metadata"));
                return false;
            }
            settings.importedBlenderLibraryOverrides = importedOverrides.toObject();
        }
        if (version >= 37) {
            const QJsonValue outlinerOnlyValue = settingsObject.value(
                QStringLiteral("importedBlenderOutlinerOnlyObjects"));
            if (!outlinerOnlyValue.isArray() ||
                outlinerOnlyValue.toArray().size() > 100000) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender Outliner-only object metadata"));
                return false;
            }
            for (const QJsonValue &recordValue : outlinerOnlyValue.toArray()) {
                if (!recordValue.isObject()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender Outliner-only object record"));
                    return false;
                }
                const QJsonObject record = recordValue.toObject();
                if (!record.value(QStringLiteral("object_id")).isString() ||
                    record.value(QStringLiteral("object_id")).toString().isEmpty() ||
                    !record.value(QStringLiteral("name")).isString() ||
                    !record.value(QStringLiteral("object_type")).isString()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender Outliner-only object identity"));
                    return false;
                }
            }
            settings.importedBlenderOutlinerOnlyObjects =
                outlinerOnlyValue.toArray();
        }
        if (version >= 15) {
            const QJsonValue lutValue = settingsObject.value(
                QStringLiteral("importedBlenderColorLuts"));
            if (!lutValue.isObject()) {
                setError(errorMessage, QStringLiteral("invalid imported Blender color LUT table"));
                return false;
            }
            const QJsonObject lutObject = lutValue.toObject();
            qsizetype totalBytes = 0;
            for (auto it = lutObject.begin(); it != lutObject.end(); ++it) {
                QString encoded;
                ImportedBlenderColorLutMetadata metadata;
                if (version >= 17) {
                    if (!it.value().isObject()) {
                        setError(errorMessage, QStringLiteral("invalid imported Blender color LUT record"));
                        return false;
                    }
                    const QJsonObject record = it.value().toObject();
                    const QJsonValue sizeValue = record.value(QStringLiteral("size"));
                    const QJsonValue minValue = record.value(QStringLiteral("domain_min"));
                    const QJsonValue maxValue = record.value(QStringLiteral("domain_max"));
                    const QJsonValue shaperValue = record.value(QStringLiteral("shaper"));
                    const QJsonValue scaleValue = record.value(QStringLiteral("shaper_scale"));
                    metadata.size = sizeValue.toInt(-1);
                    metadata.domainMin = minValue.toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                    metadata.domainMax = maxValue.toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                    metadata.shaper = shaperValue.toString();
                    metadata.shaperScale = scaleValue.toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                    if (!sizeValue.isDouble() ||
                        std::floor(sizeValue.toDouble()) != metadata.size ||
                        !minValue.isDouble() || !maxValue.isDouble() ||
                        !shaperValue.isString() || !scaleValue.isDouble()) {
                        setError(errorMessage, QStringLiteral("invalid imported Blender color LUT metadata"));
                        return false;
                    }
                    encoded = record.value(QStringLiteral("data_base64")).toString();
                    if (!record.value(QStringLiteral("data_base64")).isString()) {
                        setError(errorMessage, QStringLiteral("invalid imported Blender color LUT data"));
                        return false;
                    }
                } else if (it.value().isString()) {
                    encoded = it.value().toString();
                    metadata = {};
                } else {
                    setError(errorMessage, QStringLiteral("invalid imported Blender color LUT data"));
                    return false;
                }
                const auto decoded = QByteArray::fromBase64Encoding(
                    encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                const QByteArray bytes = decoded.decoded;
                if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok ||
                    metadata.size < 2 || metadata.size > 140 ||
                    !std::isfinite(metadata.domainMin) ||
                    !std::isfinite(metadata.domainMax) ||
                    metadata.domainMax <= metadata.domainMin ||
                    (metadata.shaper != QStringLiteral("linear") &&
                     metadata.shaper != QStringLiteral("log2_1p")) ||
                    !std::isfinite(metadata.shaperScale) ||
                    metadata.shaperScale <= 0.0 ||
                    bytes.size() != qsizetype(metadata.size) * metadata.size * metadata.size * 3 * 4 ||
                    QString::fromLatin1(QCryptographicHash::hash(
                        bytes, QCryptographicHash::Sha256).toHex()) != it.key()) {
                    setError(errorMessage, QStringLiteral("invalid imported Blender color LUT identity or data"));
                    return false;
                }
                totalBytes += bytes.size();
                if (lutObject.size() > 8 || totalBytes > kColorLutTotalByteLimit) {
                    setError(errorMessage, QStringLiteral("imported Blender color LUT table exceeds its size limit"));
                    return false;
                }
                settings.importedBlenderColorLuts.insert(it.key(), bytes);
                settings.importedBlenderColorLutMetadata.insert(it.key(), metadata);
            }
        }
        if (!isValidImportedColorLuts(settings.importedBlenderColorLuts,
                                      settings.importedBlenderColorLutMetadata)) {
            setError(errorMessage, QStringLiteral("invalid imported Blender color LUT table"));
            return false;
        }
        if (!isValidDocumentSettings(settings)) {
            setError(errorMessage, QStringLiteral("invalid document grid settings"));
            return false;
        }
        snapshot.settings = settings;
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
        Shape shape;
        if (!idFromJson(serializedObject.value(QStringLiteral("id")), &objectValueId) ||
            objectIds.contains(objectValueId) ||
            !shapeFromJson(serializedObject.value(QStringLiteral("geometry")), &shape)) {
            setError(errorMessage, QStringLiteral("invalid scene object record"));
            return false;
        }

        QVector<CollectionId> objectCollectionIds;
        bool hideViewport = false;
        bool hideRender = false;
        bool hiddenInViewport = false;
        bool hideSelect = false;
        bool hideSurfacePick = false;
        QJsonObject viewLayerHidden;
        QString defaultViewLayerKey;
        if (version >= 10) {
            const QJsonValue collectionIdsValue =
                serializedObject.value(QStringLiteral("collectionIds"));
            if (!collectionIdsValue.isArray() ||
                !serializedObject.value(QStringLiteral("hideViewport")).isBool() ||
                !serializedObject.value(QStringLiteral("hideRender")).isBool() ||
                !serializedObject.value(QStringLiteral("hiddenInViewport")).isBool()) {
                setError(errorMessage, QStringLiteral("invalid scene object collection visibility"));
                return false;
            }
            QSet<quint64> seenCollectionIds;
            for (const QJsonValue &collectionIdValue : collectionIdsValue.toArray()) {
                quint64 collectionId = 0;
                if (!idFromJson(collectionIdValue, &collectionId) ||
                    !collectionIds.contains(collectionId) ||
                    seenCollectionIds.contains(collectionId)) {
                    setError(errorMessage, QStringLiteral("invalid scene object collection membership"));
                    return false;
                }
                seenCollectionIds.insert(collectionId);
                objectCollectionIds.append(CollectionId::fromValue(collectionId));
            }
            hideViewport = serializedObject.value(QStringLiteral("hideViewport")).toBool();
            hideRender = serializedObject.value(QStringLiteral("hideRender")).toBool();
            hiddenInViewport = serializedObject.value(QStringLiteral("hiddenInViewport")).toBool();
        }
        if (version >= 11) {
            if (!serializedObject.value(QStringLiteral("hideSelect")).isBool()) {
                setError(errorMessage, QStringLiteral("invalid scene object selection visibility"));
                return false;
            }
            hideSelect = serializedObject.value(QStringLiteral("hideSelect")).toBool();
            if (version >= 12) {
                const QJsonValue states = serializedObject.value(QStringLiteral("viewLayerHidden"));
                if (!states.isObject()) {
                    setError(errorMessage, QStringLiteral("invalid object view-layer visibility"));
                    return false;
                }
                viewLayerHidden = states.toObject();
                const QJsonValue key = serializedObject.value(QStringLiteral("defaultViewLayerKey"));
                if (!key.isString()) {
                    setError(errorMessage, QStringLiteral("invalid default view-layer key"));
                    return false;
                }
                defaultViewLayerKey = key.toString();
            }
        }
        if (version >= 25) {
            if (!serializedObject.value(QStringLiteral("hideSurfacePick")).isBool()) {
                setError(errorMessage,
                         QStringLiteral("invalid scene object surface-pick state"));
                return false;
            }
            hideSurfacePick =
                serializedObject.value(QStringLiteral("hideSurfacePick")).toBool();
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
        bool hasWorkbenchMaterial = version < 20;
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
            if (version >= 20) {
                const QJsonValue hasMaterialValue =
                    serializedObject.value(QStringLiteral("hasWorkbenchMaterial"));
                if (!hasMaterialValue.isBool()) {
                    setError(errorMessage,
                             QStringLiteral("invalid scene object material presence"));
                    return false;
                }
                hasWorkbenchMaterial = hasMaterialValue.toBool();
            }
        }

        SceneObject sceneObject;
        sceneObject.id = ObjectId::fromValue(objectValueId);
        sceneObject.collectionIds = std::move(objectCollectionIds);
        sceneObject.hideViewport = hideViewport;
        sceneObject.hideRender = hideRender;
        sceneObject.hiddenInViewport = hiddenInViewport;
        sceneObject.hideSelect = hideSelect;
        sceneObject.hideSurfacePick = hideSurfacePick;
        if (version >= 19) {
            if (!serializedObject.value(QStringLiteral("showInFront")).isBool()) {
                setError(errorMessage, QStringLiteral("invalid object in-front state"));
                return false;
            }
            sceneObject.showInFront = serializedObject.value(QStringLiteral("showInFront")).toBool();
        }
        sceneObject.showWire = serializedObject.value(QStringLiteral("showWire")).toBool(false);
        sceneObject.showAllEdges = serializedObject.value(QStringLiteral("showAllEdges")).toBool(false);
        sceneObject.wireDisplay = serializedObject.value(QStringLiteral("wireDisplay")).toBool(false);
        sceneObject.blenderLooseMeshEdgeCompanion = serializedObject.value(
            QStringLiteral("blenderLooseMeshEdgeCompanion")).toBool(false);
        const QJsonValue looseEdgeSourceIndicesValue = serializedObject.value(
            QStringLiteral("blenderLooseMeshEdgeSourceIndices"));
        if (!looseEdgeSourceIndicesValue.isUndefined()) {
            if (!looseEdgeSourceIndicesValue.isArray()) {
                setError(errorMessage,
                         QStringLiteral("invalid loose-edge source index list"));
                return false;
            }
            for (const QJsonValue &edgeIndexValue :
                 looseEdgeSourceIndicesValue.toArray()) {
                const double edgeIndexNumber = edgeIndexValue.toDouble(-1.0);
                if (!edgeIndexValue.isDouble() ||
                    !std::isfinite(edgeIndexNumber) ||
                    std::floor(edgeIndexNumber) != edgeIndexNumber ||
                    edgeIndexNumber < 0.0 ||
                    edgeIndexNumber > std::numeric_limits<int>::max()) {
                    setError(errorMessage,
                             QStringLiteral("invalid loose-edge source index"));
                    return false;
                }
                sceneObject.blenderLooseMeshEdgeSourceIndices.append(
                    static_cast<int>(edgeIndexNumber));
            }
            const int curveComponentCount =
                shape.geometryType == GeometryType::PolyCurve
                    ? shape.components.size()
                    : shape.geometryType == GeometryType::Nurbs ? 1 : 0;
            if ((!sceneObject.blenderLooseMeshEdgeCompanion &&
                 !sceneObject.blenderLooseMeshEdgeSourceIndices.isEmpty()) ||
                (!sceneObject.blenderLooseMeshEdgeSourceIndices.isEmpty() &&
                 sceneObject.blenderLooseMeshEdgeSourceIndices.size() !=
                     curveComponentCount)) {
                setError(errorMessage,
                         QStringLiteral("loose-edge source indices do not match companion geometry"));
                return false;
            }
        }
        sceneObject.blenderSourceObjectId = serializedObject.value(
            QStringLiteral("blenderSourceObjectId")).toString();
        if (version >= 26) {
            const QJsonValue parentObjectId = serializedObject.value(
                QStringLiteral("blenderParentObjectId"));
            if (!parentObjectId.isString()) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender object parent identity"));
                return false;
            }
            sceneObject.blenderParentObjectId = parentObjectId.toString();
        }
        sceneObject.displayBoundsType = serializedObject.value(
            QStringLiteral("displayBoundsType")).toString();
        const QJsonValue hasDisplayBoundsData = serializedObject.value(
            QStringLiteral("hasDisplayBoundsData"));
        if (!hasDisplayBoundsData.isUndefined()) {
            if (!hasDisplayBoundsData.isBool()) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender bounds display data flag"));
                return false;
            }
            sceneObject.hasDisplayBoundsData = hasDisplayBoundsData.toBool();
        }
        if (sceneObject.hasDisplayBoundsData &&
            (!readDisplayBoundsPoint(serializedObject.value(
                 QStringLiteral("displayBoundsMinimumLocal")),
                 &sceneObject.displayBoundsMinimumLocal) ||
             !readDisplayBoundsPoint(serializedObject.value(
                 QStringLiteral("displayBoundsMaximumLocal")),
                 &sceneObject.displayBoundsMaximumLocal) ||
             !readDisplayBoundsTransform(serializedObject.value(
                 QStringLiteral("displayBoundsTransformWorld")),
                 &sceneObject.displayBoundsTransformWorld) ||
             sceneObject.displayBoundsMinimumLocal.x >
                 sceneObject.displayBoundsMaximumLocal.x ||
             sceneObject.displayBoundsMinimumLocal.y >
                 sceneObject.displayBoundsMaximumLocal.y ||
             sceneObject.displayBoundsMinimumLocal.z >
                 sceneObject.displayBoundsMaximumLocal.z)) {
            setError(errorMessage,
                     QStringLiteral("invalid Blender bounds display metadata"));
            return false;
        }
        sceneObject.viewLayerHidden = viewLayerHidden;
        sceneObject.defaultViewLayerKey = defaultViewLayerKey;
        if (version >= 7) {
            const QJsonValue displayName = serializedObject.value(QStringLiteral("displayName"));
            const QJsonValue libraryPath = serializedObject.value(QStringLiteral("libraryPath"));
            if (!displayName.isString() || !libraryPath.isString()) {
                setError(errorMessage, QStringLiteral("invalid Blender object identity"));
                return false;
            }
            sceneObject.displayName = displayName.toString();
            sceneObject.libraryPath = libraryPath.toString();
            if (version >= 22) {
                const QJsonValue randomLibraryPath =
                    serializedObject.value(
                        QStringLiteral("workbenchRandomLibraryPath"));
                if (!randomLibraryPath.isString()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Workbench Random identity path"));
                    return false;
                }
                sceneObject.workbenchRandomLibraryPath =
                    randomLibraryPath.toString();
            }
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
        sceneObject.blenderSourceObjectType = serializedObject.value(
            QStringLiteral("blenderSourceObjectType")).toString();
        if (version >= 28) {
            const QJsonValue dataName = serializedObject.value(
                QStringLiteral("blenderSourceDataName"));
            if (!dataName.isString()) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender data-block identity"));
                return false;
            }
            sceneObject.blenderSourceDataName = dataName.toString();
        }
        if (version >= 29) {
            const QJsonValue namesValue = serializedObject.value(
                QStringLiteral("blenderSourceMaterialNames"));
            if (!namesValue.isArray() || namesValue.toArray().size() > 1024) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender object material names"));
                return false;
            }
            for (const QJsonValue &nameValue : namesValue.toArray()) {
                if (!nameValue.isString() || nameValue.toString().size() > 4096) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender object material name"));
                    return false;
                }
                sceneObject.blenderSourceMaterialNames.append(
                    nameValue.toString());
            }
        }
        if (version >= 30) {
            const auto readBlenderNameList = [
                &serializedObject, errorMessage](const QString &key,
                                                  const QString &label,
                                                  QStringList *names) {
                const QJsonValue namesValue = serializedObject.value(key);
                if (!namesValue.isArray() || namesValue.toArray().size() > 1024) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender %1 names").arg(label));
                    return false;
                }
                for (const QJsonValue &nameValue : namesValue.toArray()) {
                    if (!nameValue.isString() ||
                        nameValue.toString().size() > 4096) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender %1 name").arg(label));
                        return false;
                    }
                    names->append(nameValue.toString());
                }
                return true;
            };
            if (!readBlenderNameList(
                    QStringLiteral("blenderSourceModifierNames"),
                    QStringLiteral("modifier"),
                    &sceneObject.blenderSourceModifierNames) ||
                !readBlenderNameList(
                    QStringLiteral("blenderSourceConstraintNames"),
                    QStringLiteral("constraint"),
                    &sceneObject.blenderSourceConstraintNames) ||
                !readBlenderNameList(
                    QStringLiteral("blenderSourceVertexGroupNames"),
                    QStringLiteral("vertex-group"),
                    &sceneObject.blenderSourceVertexGroupNames)) {
                return false;
            }
        }
        if (version >= 31) {
            const QJsonValue hasAnimationValue = serializedObject.value(
                QStringLiteral("blenderSourceHasAnimationData"));
            const QJsonValue actionNameValue = serializedObject.value(
                QStringLiteral("blenderSourceAnimationActionName"));
            const QJsonValue hasDriversValue = serializedObject.value(
                QStringLiteral("blenderSourceHasDrivers"));
            const QJsonValue driverNamesValue = serializedObject.value(
                QStringLiteral("blenderSourceDriverTargetNames"));
            const QJsonValue nlaTracksValue = serializedObject.value(
                QStringLiteral("blenderSourceNlaTracks"));
            if (!hasAnimationValue.isBool() || !actionNameValue.isString() ||
                actionNameValue.toString().size() > 4096 ||
                !hasDriversValue.isBool() || !driverNamesValue.isArray() ||
                driverNamesValue.toArray().size() > 1024 ||
                !nlaTracksValue.isArray() || nlaTracksValue.toArray().size() > 1024) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender animation data"));
                return false;
            }
            sceneObject.blenderSourceHasAnimationData = hasAnimationValue.toBool();
            sceneObject.blenderSourceAnimationActionName = actionNameValue.toString();
            if (version >= 35) {
                const QJsonValue actionSlotNameValue = serializedObject.value(
                    QStringLiteral("blenderSourceAnimationActionSlotName"));
                if (!actionSlotNameValue.isString() ||
                    actionSlotNameValue.toString().size() > 4096) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender animation action slot name"));
                    return false;
                }
                sceneObject.blenderSourceAnimationActionSlotName =
                    actionSlotNameValue.toString();
            }
            sceneObject.blenderSourceHasDrivers = hasDriversValue.toBool();
            for (const QJsonValue &nameValue : driverNamesValue.toArray()) {
                if (!nameValue.isString() || nameValue.toString().size() > 4096) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender driver target name"));
                    return false;
                }
                sceneObject.blenderSourceDriverTargetNames.append(nameValue.toString());
            }
            for (const QJsonValue &trackValue : nlaTracksValue.toArray()) {
                if (!trackValue.isObject()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender NLA track"));
                    return false;
                }
                const QJsonObject trackObject = trackValue.toObject();
                const QJsonValue nameValue = trackObject.value(QStringLiteral("name"));
                const QJsonValue actionsValue = trackObject.value(
                    QStringLiteral("actionNames"));
                if (!nameValue.isString() || nameValue.toString().size() > 4096 ||
                    !actionsValue.isArray() || actionsValue.toArray().size() > 1024) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender NLA track contents"));
                    return false;
                }
                BlenderSourceNlaTrack track;
                track.name = nameValue.toString();
                for (const QJsonValue &actionValue : actionsValue.toArray()) {
                    if (!actionValue.isString() || actionValue.toString().size() > 4096) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender NLA action name"));
                        return false;
                    }
                    track.actionNames.append(actionValue.toString());
                }
                sceneObject.blenderSourceNlaTracks.append(std::move(track));
            }
        }
        if (version >= 32) {
            const QJsonValue modifiersValue = serializedObject.value(
                QStringLiteral("blenderSourceModifiers"));
            if (!modifiersValue.isArray() || modifiersValue.toArray().size() > 1024) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender modifier entries"));
                return false;
            }
            for (const QJsonValue &modifierValue : modifiersValue.toArray()) {
                if (!modifierValue.isObject()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender modifier entry"));
                    return false;
                }
                const QJsonObject modifierObject = modifierValue.toObject();
                const QJsonValue nameValue = modifierObject.value(
                    QStringLiteral("name"));
                const QJsonValue linkedNameValue = modifierObject.value(
                    QStringLiteral("linkedName"));
                const QJsonValue linkedTypeValue = modifierObject.value(
                    QStringLiteral("linkedType"));
                const QString linkedType = linkedTypeValue.toString();
                if (!nameValue.isString() || nameValue.toString().size() > 4096 ||
                    !linkedNameValue.isString() ||
                    linkedNameValue.toString().size() > 4096 ||
                    !linkedTypeValue.isString() || linkedType.size() > 32 ||
                    (!linkedType.isEmpty() && linkedType != QLatin1String("object") &&
                     linkedType != QLatin1String("node_tree") &&
                     linkedType != QLatin1String("particle_system"))) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender modifier link"));
                    return false;
                }
                sceneObject.blenderSourceModifiers.append({
                    nameValue.toString(), linkedNameValue.toString(), linkedType});
            }
        }
        if (version >= 38) {
            const QJsonValue effectsValue = serializedObject.value(
                QStringLiteral("blenderSourceShaderEffects"));
            if (!effectsValue.isArray() || effectsValue.toArray().size() > 1024) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender shader effects"));
                return false;
            }
            for (const QJsonValue &effectValue : effectsValue.toArray()) {
                if (!effectValue.isObject()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender shader effect entry"));
                    return false;
                }
                const QJsonObject effectObject = effectValue.toObject();
                const QJsonValue nameValue = effectObject.value(
                    QStringLiteral("name"));
                const QJsonValue linkedNameValue = effectObject.value(
                    QStringLiteral("linkedName"));
                const QJsonValue linkedTypeValue = effectObject.value(
                    QStringLiteral("linkedType"));
                const QString linkedType = linkedTypeValue.toString();
                if (!nameValue.isString() || nameValue.toString().size() > 4096 ||
                    !linkedNameValue.isString() ||
                    linkedNameValue.toString().size() > 4096 ||
                    !linkedTypeValue.isString() || linkedType.size() > 32 ||
                    (!linkedType.isEmpty() && linkedType != QLatin1String("object"))) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender shader effect link"));
                    return false;
                }
                sceneObject.blenderSourceShaderEffects.append({
                    nameValue.toString(), linkedNameValue.toString(), linkedType});
            }
        }
        if (version >= 39) {
            const QJsonValue layerTreeValue = serializedObject.value(
                QStringLiteral("blenderSourceGreasePencilLayerTree"));
            int nodeCount = 0;
            const auto validLayerNodes = [](
                const auto &self, const QJsonArray &nodes, int depth,
                int &count) -> bool {
                if (nodes.isEmpty()) return true;
                if (depth >= 128 || nodes.size() > 1024) return false;
                for (const QJsonValue &nodeValue : nodes) {
                    if (!nodeValue.isObject() || ++count > 1024) return false;
                    const QJsonObject node = nodeValue.toObject();
                    const QJsonValue nameValue = node.value(
                        QStringLiteral("name"));
                    const QJsonValue groupValue = node.value(
                        QStringLiteral("is_group"));
                    const QJsonValue childrenValue = node.value(
                        QStringLiteral("children"));
                    if (!nameValue.isString() || nameValue.toString().size() > 4096 ||
                        !groupValue.isBool() || !childrenValue.isArray() ||
                        (!groupValue.toBool() &&
                         !childrenValue.toArray().isEmpty()) ||
                        !self(self, childrenValue.toArray(), depth + 1, count)) {
                        return false;
                    }
                }
                return true;
            };
            if (!layerTreeValue.isArray() ||
                !validLayerNodes(validLayerNodes, layerTreeValue.toArray(), 0,
                                 nodeCount)) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender Grease Pencil layer tree"));
                return false;
            }
            sceneObject.blenderSourceGreasePencilLayerTree =
                layerTreeValue.toArray();
        }
        if (version >= 33) {
            const QJsonValue shapeKeyDataNameValue = serializedObject.value(
                QStringLiteral("blenderSourceShapeKeyDataName"));
            const QJsonValue shapeKeyNamesValue = serializedObject.value(
                QStringLiteral("blenderSourceShapeKeyNames"));
            const QJsonValue dataMaterialNamesValue = serializedObject.value(
                QStringLiteral("blenderSourceDataMaterialNames"));
            const auto readNames = [errorMessage](
                const QJsonValue &namesValue, const QString &label,
                QStringList *names) {
                if (!namesValue.isArray() || namesValue.toArray().size() > 1024) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender %1 list").arg(label));
                    return false;
                }
                for (const QJsonValue &nameValue : namesValue.toArray()) {
                    if (!nameValue.isString() ||
                        nameValue.toString().size() > 4096) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender %1 name").arg(label));
                        return false;
                    }
                    names->append(nameValue.toString());
                }
                return true;
            };
            if (!shapeKeyDataNameValue.isString() ||
                shapeKeyDataNameValue.toString().size() > 4096 ||
                !readNames(shapeKeyNamesValue, QStringLiteral("shape key"),
                           &sceneObject.blenderSourceShapeKeyNames) ||
                !readNames(dataMaterialNamesValue,
                           QStringLiteral("data material"),
                           &sceneObject.blenderSourceDataMaterialNames)) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender data-block contents"));
                return false;
            }
            sceneObject.blenderSourceShapeKeyDataName =
                shapeKeyDataNameValue.toString();
        }
        if (version >= 34) {
            const QJsonValue dataAnimationValue = serializedObject.value(
                QStringLiteral("blenderSourceDataAnimation"));
            if (!dataAnimationValue.isObject()) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender data-block animation data"));
                return false;
            }
            const QJsonObject dataAnimation = dataAnimationValue.toObject();
            const QJsonValue hasAnimationValue = dataAnimation.value(
                QStringLiteral("hasAnimationData"));
            const QJsonValue actionNameValue = dataAnimation.value(
                QStringLiteral("actionName"));
            const QJsonValue actionSlotNameValue = dataAnimation.value(
                QStringLiteral("actionSlotName"));
            const QJsonValue hasDriversValue = dataAnimation.value(
                QStringLiteral("hasDrivers"));
            const QJsonValue driverNamesValue = dataAnimation.value(
                QStringLiteral("driverTargetNames"));
            const QJsonValue nlaTracksValue = dataAnimation.value(
                QStringLiteral("nlaTracks"));
            if (!hasAnimationValue.isBool() || !actionNameValue.isString() ||
                actionNameValue.toString().size() > 4096 ||
                (version >= 35 && (!actionSlotNameValue.isString() ||
                    actionSlotNameValue.toString().size() > 4096)) ||
                !hasDriversValue.isBool() || !driverNamesValue.isArray() ||
                driverNamesValue.toArray().size() > 1024 ||
                !nlaTracksValue.isArray() || nlaTracksValue.toArray().size() > 1024) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender data-block animation data"));
                return false;
            }
            BlenderSourceAnimationData &animation =
                sceneObject.blenderSourceDataAnimation;
            animation.hasAnimationData = hasAnimationValue.toBool();
            animation.actionName = actionNameValue.toString();
            if (version >= 35) {
                animation.actionSlotName = actionSlotNameValue.toString();
            }
            animation.hasDrivers = hasDriversValue.toBool();
            for (const QJsonValue &nameValue : driverNamesValue.toArray()) {
                if (!nameValue.isString() || nameValue.toString().size() > 4096) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender data-block driver target name"));
                    return false;
                }
                animation.driverTargetNames.append(nameValue.toString());
            }
            for (const QJsonValue &trackValue : nlaTracksValue.toArray()) {
                if (!trackValue.isObject()) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender data-block NLA track"));
                    return false;
                }
                const QJsonObject trackObject = trackValue.toObject();
                const QJsonValue nameValue = trackObject.value(
                    QStringLiteral("name"));
                const QJsonValue actionsValue = trackObject.value(
                    QStringLiteral("actionNames"));
                if (!nameValue.isString() || nameValue.toString().size() > 4096 ||
                    !actionsValue.isArray() || actionsValue.toArray().size() > 1024) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender data-block NLA track contents"));
                    return false;
                }
                BlenderSourceNlaTrack track;
                track.name = nameValue.toString();
                for (const QJsonValue &actionValue : actionsValue.toArray()) {
                    if (!actionValue.isString() ||
                        actionValue.toString().size() > 4096) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender data-block NLA action name"));
                        return false;
                    }
                    track.actionNames.append(actionValue.toString());
                }
                animation.nlaTracks.append(std::move(track));
            }
        }
        if (version >= 36) {
            const auto readAnimationList = [errorMessage](
                const QJsonValue &value, qsizetype expectedCount,
                QVector<BlenderSourceAnimationData> *destination) {
                if (destination == nullptr || !value.isArray() ||
                    value.toArray().size() > 1024 ||
                    value.toArray().size() != expectedCount) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender material animation list"));
                    return false;
                }
                for (const QJsonValue &animationValue : value.toArray()) {
                    if (!animationValue.isObject()) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender material animation data"));
                        return false;
                    }
                    const QJsonObject animationObject = animationValue.toObject();
                    const QJsonValue hasAnimationValue = animationObject.value(
                        QStringLiteral("hasAnimationData"));
                    const QJsonValue actionNameValue = animationObject.value(
                        QStringLiteral("actionName"));
                    const QJsonValue actionSlotNameValue = animationObject.value(
                        QStringLiteral("actionSlotName"));
                    const QJsonValue hasDriversValue = animationObject.value(
                        QStringLiteral("hasDrivers"));
                    const QJsonValue driverNamesValue = animationObject.value(
                        QStringLiteral("driverTargetNames"));
                    const QJsonValue nlaTracksValue = animationObject.value(
                        QStringLiteral("nlaTracks"));
                    if (!hasAnimationValue.isBool() || !actionNameValue.isString() ||
                        actionNameValue.toString().size() > 4096 ||
                        !actionSlotNameValue.isString() ||
                        actionSlotNameValue.toString().size() > 4096 ||
                        !hasDriversValue.isBool() || !driverNamesValue.isArray() ||
                        driverNamesValue.toArray().size() > 1024 ||
                        !nlaTracksValue.isArray() ||
                        nlaTracksValue.toArray().size() > 1024) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender material animation data"));
                        return false;
                    }
                    BlenderSourceAnimationData animation;
                    animation.hasAnimationData = hasAnimationValue.toBool();
                    animation.actionName = actionNameValue.toString();
                    animation.actionSlotName = actionSlotNameValue.toString();
                    animation.hasDrivers = hasDriversValue.toBool();
                    for (const QJsonValue &nameValue : driverNamesValue.toArray()) {
                        if (!nameValue.isString() ||
                            nameValue.toString().size() > 4096) {
                            setError(errorMessage,
                                     QStringLiteral("invalid Blender material driver target name"));
                            return false;
                        }
                        animation.driverTargetNames.append(nameValue.toString());
                    }
                    for (const QJsonValue &trackValue : nlaTracksValue.toArray()) {
                        if (!trackValue.isObject()) {
                            setError(errorMessage,
                                     QStringLiteral("invalid Blender material NLA track"));
                            return false;
                        }
                        const QJsonObject trackObject = trackValue.toObject();
                        const QJsonValue nameValue = trackObject.value(
                            QStringLiteral("name"));
                        const QJsonValue actionsValue = trackObject.value(
                            QStringLiteral("actionNames"));
                        if (!nameValue.isString() || nameValue.toString().size() > 4096 ||
                            !actionsValue.isArray() ||
                            actionsValue.toArray().size() > 1024) {
                            setError(errorMessage,
                                     QStringLiteral("invalid Blender material NLA track contents"));
                            return false;
                        }
                        BlenderSourceNlaTrack track;
                        track.name = nameValue.toString();
                        for (const QJsonValue &actionValue : actionsValue.toArray()) {
                            if (!actionValue.isString() ||
                                actionValue.toString().size() > 4096) {
                                setError(errorMessage,
                                         QStringLiteral("invalid Blender material NLA action name"));
                                return false;
                            }
                            track.actionNames.append(actionValue.toString());
                        }
                        animation.nlaTracks.append(std::move(track));
                    }
                    destination->append(std::move(animation));
                }
                return true;
            };
            if (!readAnimationList(
                    serializedObject.value(
                        QStringLiteral("blenderSourceMaterialAnimations")),
                    sceneObject.blenderSourceMaterialNames.size(),
                    &sceneObject.blenderSourceMaterialAnimations) ||
                !readAnimationList(
                    serializedObject.value(
                        QStringLiteral("blenderSourceDataMaterialAnimations")),
                    sceneObject.blenderSourceDataMaterialNames.size(),
                    &sceneObject.blenderSourceDataMaterialAnimations)) {
                return false;
            }
        }
        const QJsonValue surfaceWireValue = serializedObject.value(
            QStringLiteral("blenderSurfaceWireVerticesWorld"));
        if (!surfaceWireValue.isUndefined()) {
            const QJsonValue surfaceFingerprintValue = serializedObject.value(
                QStringLiteral("blenderSurfaceWireGeometryFingerprint"));
            if (sceneObject.geometry.geometryType != GeometryType::NurbsSurface ||
                !surfaceWireValue.isArray() ||
                !surfaceFingerprintValue.isString()) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender surface wire display data"));
                return false;
            }
            const QByteArray fingerprintHex =
                surfaceFingerprintValue.toString().toLatin1();
            if (fingerprintHex.size() != 64 ||
                QByteArray::fromHex(fingerprintHex).toHex() != fingerprintHex) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender surface geometry fingerprint"));
                return false;
            }
            const QJsonArray wireVertices = surfaceWireValue.toArray();
            if (wireVertices.size() > 400000 ||
                (wireVertices.size() % 2) != 0) {
                setError(errorMessage,
                         QStringLiteral("invalid Blender surface wire vertex count"));
                return false;
            }
            sceneObject.blenderSurfaceWireVerticesWorld.reserve(
                wireVertices.size());
            for (const QJsonValue &wireVertexValue : wireVertices) {
                if (!wireVertexValue.isArray() ||
                    wireVertexValue.toArray().size() != 3) {
                    setError(errorMessage,
                             QStringLiteral("invalid Blender surface wire vertex"));
                    return false;
                }
                const QJsonArray coordinates = wireVertexValue.toArray();
                for (const QJsonValue &coordinate : coordinates) {
                    if (!coordinate.isDouble() ||
                        !std::isfinite(coordinate.toDouble())) {
                        setError(errorMessage,
                                 QStringLiteral("invalid Blender surface wire vertex"));
                        return false;
                    }
                }
                sceneObject.blenderSurfaceWireVerticesWorld.append(
                    {coordinates[0].toDouble(), coordinates[1].toDouble(),
                     coordinates[2].toDouble()});
            }
            sceneObject.blenderSurfaceWireGeometryFingerprint = fingerprintHex;
        }
        if (version >= 21) {
            const QJsonValue faceMaterialsValue =
                serializedObject.value(QStringLiteral("nurbsSolidFaceMaterialIds"));
            if (!faceMaterialsValue.isObject()) {
                setError(errorMessage,
                         QStringLiteral("invalid solid face material references"));
                return false;
            }
            const QJsonObject faceMaterials = faceMaterialsValue.toObject();
            if (!faceMaterials.isEmpty() &&
                (sceneObject.geometry.geometryType != GeometryType::NurbsSolid ||
                 !sceneObject.geometry.nurbsSolid.boundaryFaces.isEmpty())) {
                setError(errorMessage,
                         QStringLiteral("face materials require a NurbsSolid"));
                return false;
            }
            QSet<QString> validFaceIds{
                sceneObject.geometry.nurbsSolid.baseCapFaceId,
                sceneObject.geometry.nurbsSolid.displacedCapFaceId};
            for (const QString &faceId :
                 sceneObject.geometry.nurbsSolid.wallFaceIds) {
                validFaceIds.insert(faceId);
            }
            for (auto it = faceMaterials.begin(); it != faceMaterials.end(); ++it) {
                if (!it.value().isString() || !validFaceIds.contains(it.key()) ||
                    !materialIds.contains(it.value().toString())) {
                    setError(errorMessage,
                             QStringLiteral("invalid solid face material reference"));
                    return false;
                }
                sceneObject.nurbsSolidFaceMaterialIds.insert(
                    it.key(), it.value().toString());
            }
        }
        sceneObject.placementTranslation = placementTranslation;
        sceneObject.displayColorSceneLinear = displayColorSceneLinear;
        sceneObject.workbenchMaterial = workbenchMaterial;
        sceneObject.hasWorkbenchMaterial = hasWorkbenchMaterial;
        snapshot.objects.append(std::move(sceneObject));
        objectIds.insert(objectValueId);
        if (objectValueId < std::numeric_limits<quint64>::max()) {
            nextObjectValue = std::max(nextObjectValue, objectValueId + 1);
        }
    }

    // Keep native documents under the same scene-wide unique float-texture
    // budget as Blender extraction. Material/object records may repeat an
    // identical shared image payload, so count hashes rather than occurrences.
    QSet<QByteArray> floatTextureIds;
    quint64 floatTextureBytes = 0;
    std::function<bool(const QJsonValue &)> validateFloatTextureBudget =
        [&](const QJsonValue &node) -> bool {
            if (node.isObject()) {
                const QJsonObject object = node.toObject();
                for (auto it = object.begin(); it != object.end(); ++it) {
                    if (it.key() == QStringLiteral("sceneLinearFloatDataBase64") ||
                        it.key() == QStringLiteral("textureImageSceneLinearFloatDataBase64")) {
                        if (!it.value().isString()) return false;
                        const QString encoded = it.value().toString();
                        if (!encoded.isEmpty()) {
                            if (encoded.size() > 44 * 1024 * 1024) return false;
                            const QByteArray bytes = QByteArray::fromBase64(
                                encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                            if (bytes.isEmpty() || bytes.size() > 32 * 1024 * 1024)
                                return false;
                            const QByteArray hash = QCryptographicHash::hash(
                                bytes, QCryptographicHash::Sha256);
                            if (!floatTextureIds.contains(hash)) {
                                if (floatTextureBytes + quint64(bytes.size()) >
                                    64ULL * 1024 * 1024) return false;
                                floatTextureIds.insert(hash);
                                floatTextureBytes += quint64(bytes.size());
                            }
                        }
                    }
                    if (!validateFloatTextureBudget(it.value())) return false;
                }
            } else if (node.isArray()) {
                for (const QJsonValue &child : node.toArray())
                    if (!validateFloatTextureBudget(child)) return false;
            }
            return true;
        };
    if (!validateFloatTextureBudget(value)) {
        setError(errorMessage, QStringLiteral("scene-linear float image payloads exceed the document limits"));
        return false;
    }
    snapshot.nextObjectValue = nextObjectValue;
    document->restoreSnapshot(snapshot);
    if (!document->setOutlinerExpandedPathsByScene(
            outlinerExpandedPathsByScene)) {
        setError(errorMessage,
                 QStringLiteral("invalid saved Outliner expansion state"));
        return false;
    }
    return true;
}

} // namespace classiCAD
