/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QString>

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

    bool operator==(const DocumentSettings &) const = default;
};

qreal millimetersPerDocumentUnit(DocumentLengthUnit unit);
QString documentLengthUnitName(DocumentLengthUnit unit);
QString documentLengthUnitKey(DocumentLengthUnit unit);
bool documentLengthUnitFromKey(const QString &key, DocumentLengthUnit *unit);
bool isValidDocumentSettings(const DocumentSettings &settings);
qreal documentGridSpacingInMillimeters(const DocumentSettings &settings);

} // namespace classiCAD
