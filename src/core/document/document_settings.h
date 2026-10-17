/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QByteArray>

namespace classiCAD {

// Geometry coordinates are stored in millimeters. The selected unit changes
// the document grid's displayed spacing, not the underlying geometry values.
enum class DocumentLengthUnit {
    Millimeter,
    Centimeter,
    Meter,
    Inch,
    Foot,
};

struct ImportedBlenderColorLutMetadata {
    int size = 65;
    double domainMin = 0.0;
    double domainMax = 2.0;
    QString shaper = QStringLiteral("linear");
    double shaperScale = 1.0;
    bool operator==(const ImportedBlenderColorLutMetadata &) const = default;
};

struct DocumentSettings {
    DocumentLengthUnit lengthUnit = DocumentLengthUnit::Millimeter;
    qreal gridSpacing = 1.0;
    // Active imported Blender view layer. Empty keeps legacy documents on
    // each object's first imported view layer.
    QString activeViewLayerKey;
    // Blender's active collection is view-layer-specific. Values are source
    // collection ID paths from the scene root to the active collection.
    QJsonObject activeCollectionPathsByViewLayer;
    // Imported Blender scene metadata, including each scene's active camera.
    // Kept scene-qualified because Blender files may contain multiple scenes.
    QJsonArray importedBlenderScenes;
    // Blender Outliner Libraries-mode data, grouped by owning .blend and ID type.
    QJsonArray importedBlenderLibraries;
    // Blender Outliner Orphan Data rows, grouped by Blender ID type.
    QJsonArray importedBlenderOrphans;
    // Current Blender workspace's Sequencer Outliner tree.
    QJsonObject importedBlenderSequencer;
    // Bounded read-only Blender RNA Data API Outliner tree.
    QJsonObject importedBlenderDataApi;
    // Bounded Blender Library Overrides Properties and Hierarchies trees.
    QJsonObject importedBlenderLibraryOverrides;
    // Blender objects with no classiCAD geometry representation still belong
    // in the Outliner. These source records preserve their tree rows only.
    QJsonArray importedBlenderOutlinerOnlyObjects;
    // Exact decoded RGB float LUT bytes, keyed by their lowercase SHA-256.
    QHash<QString, QByteArray> importedBlenderColorLuts;
    // Metadata is keyed by the LUT payload's SHA-256, which remains its identity.
    QHash<QString, ImportedBlenderColorLutMetadata> importedBlenderColorLutMetadata;

    bool operator==(const DocumentSettings &) const = default;
};

qreal millimetersPerDocumentUnit(DocumentLengthUnit unit);
QString documentLengthUnitName(DocumentLengthUnit unit);
QString documentLengthUnitKey(DocumentLengthUnit unit);
bool documentLengthUnitFromKey(const QString &key, DocumentLengthUnit *unit);
bool isValidDocumentSettings(const DocumentSettings &settings);
qreal documentGridSpacingInMillimeters(const DocumentSettings &settings);
bool parseDocumentLengthInput(const QString &input,
                              DocumentLengthUnit defaultUnit,
                              qreal *millimeters);

} // namespace classiCAD
