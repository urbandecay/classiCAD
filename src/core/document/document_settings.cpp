/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "document_settings.h"

#include <cmath>

namespace classiCAD {

qreal millimetersPerDocumentUnit(DocumentLengthUnit unit)
{
    switch (unit) {
    case DocumentLengthUnit::Millimeter:
        return 1.0;
    case DocumentLengthUnit::Centimeter:
        return 10.0;
    case DocumentLengthUnit::Meter:
        return 1000.0;
    case DocumentLengthUnit::Inch:
        return 25.4;
    case DocumentLengthUnit::Foot:
        return 304.8;
    }
    return 1.0;
}

QString documentLengthUnitName(DocumentLengthUnit unit)
{
    switch (unit) {
    case DocumentLengthUnit::Millimeter:
        return QStringLiteral("Millimeters");
    case DocumentLengthUnit::Centimeter:
        return QStringLiteral("Centimeters");
    case DocumentLengthUnit::Meter:
        return QStringLiteral("Meters");
    case DocumentLengthUnit::Inch:
        return QStringLiteral("Inches");
    case DocumentLengthUnit::Foot:
        return QStringLiteral("Feet");
    }
    return QStringLiteral("Millimeters");
}

QString documentLengthUnitKey(DocumentLengthUnit unit)
{
    switch (unit) {
    case DocumentLengthUnit::Millimeter:
        return QStringLiteral("millimeter");
    case DocumentLengthUnit::Centimeter:
        return QStringLiteral("centimeter");
    case DocumentLengthUnit::Meter:
        return QStringLiteral("meter");
    case DocumentLengthUnit::Inch:
        return QStringLiteral("inch");
    case DocumentLengthUnit::Foot:
        return QStringLiteral("foot");
    }
    return {};
}

bool documentLengthUnitFromKey(const QString &key, DocumentLengthUnit *unit)
{
    if (unit == nullptr) {
        return false;
    }
    if (key == QStringLiteral("millimeter")) {
        *unit = DocumentLengthUnit::Millimeter;
    } else if (key == QStringLiteral("centimeter")) {
        *unit = DocumentLengthUnit::Centimeter;
    } else if (key == QStringLiteral("meter")) {
        *unit = DocumentLengthUnit::Meter;
    } else if (key == QStringLiteral("inch")) {
        *unit = DocumentLengthUnit::Inch;
    } else if (key == QStringLiteral("foot")) {
        *unit = DocumentLengthUnit::Foot;
    } else {
        return false;
    }
    return true;
}

bool isValidDocumentSettings(const DocumentSettings &settings)
{
    const bool validUnit = documentLengthUnitKey(settings.lengthUnit).size() > 0;
    return validUnit && std::isfinite(settings.gridSpacing) &&
           settings.gridSpacing >= 1.0e-9 && settings.gridSpacing <= 1.0e9;
}

qreal documentGridSpacingInMillimeters(const DocumentSettings &settings)
{
    return isValidDocumentSettings(settings)
               ? settings.gridSpacing * millimetersPerDocumentUnit(settings.lengthUnit)
               : 1.0;
}

} // namespace classiCAD
