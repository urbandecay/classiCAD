/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QString>
#include <QJsonArray>

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

struct DocumentSettings {
    DocumentLengthUnit lengthUnit = DocumentLengthUnit::Millimeter;
    qreal gridSpacing = 1.0;
    // Active imported Blender view layer. Empty keeps legacy documents on
    // each object's first imported view layer.
    QString activeViewLayerKey;
    // Imported Blender scene metadata, including each scene's active camera.
    // Kept scene-qualified because Blender files may contain multiple scenes.
    QJsonArray importedBlenderScenes;

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
