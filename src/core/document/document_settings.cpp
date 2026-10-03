/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "document_settings.h"

#include <QStringList>
#include <cmath>

namespace classiCAD {
namespace {

bool parseLengthNumber(const QString &text, qreal *value)
{
    if (value == nullptr) return false;
    const QString token = text.trimmed();
    const qsizetype slash = token.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
        bool numeratorValid = false;
        bool denominatorValid = false;
        const qreal numerator = token.left(slash).trimmed().toDouble(&numeratorValid);
        const qreal denominator = token.mid(slash + 1).trimmed().toDouble(&denominatorValid);
        if (!numeratorValid || !denominatorValid || std::abs(denominator) <= 1.0e-12) {
            return false;
        }
        *value = numerator / denominator;
        return std::isfinite(*value);
    }
    bool valid = false;
    const qreal parsed = token.toDouble(&valid);
    if (!valid || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

bool parseAdditiveLength(const QString &text, qreal *value)
{
    if (value == nullptr) return false;
    const QString normalized = text.trimmed().replace(QLatin1Char('\t'),
                                                       QLatin1Char(' '));
    if (normalized.isEmpty()) return false;
    const QStringList terms = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    qreal sum = 0.0;
    for (const QString &term : terms) {
        qreal parsed = 0.0;
        if (!parseLengthNumber(term, &parsed)) return false;
        sum += parsed;
    }
    *value = sum;
    return std::isfinite(sum);
}

} // namespace

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

bool parseDocumentLengthInput(const QString &input,
                              DocumentLengthUnit defaultUnit,
                              qreal *millimeters)
{
    if (millimeters == nullptr) return false;
    QString text = input.trimmed().toLower();
    if (text.isEmpty()) return false;

    const qsizetype feetMark = text.indexOf(QLatin1Char('\''));
    if (feetMark >= 0) {
        qreal feet = 0.0;
        if (!parseAdditiveLength(text.left(feetMark), &feet)) return false;
        QString inchesText = text.mid(feetMark + 1).trimmed();
        inchesText.remove(QLatin1Char('"'));
        inchesText.remove(QStringLiteral("inches"));
        inchesText.remove(QStringLiteral("inch"));
        inchesText.remove(QStringLiteral("in"));
        qreal inches = 0.0;
        if (!inchesText.isEmpty() && !parseAdditiveLength(inchesText, &inches)) {
            return false;
        }
        *millimeters = feet * 304.8 + inches * 25.4;
        return std::isfinite(*millimeters);
    }

    qreal unitScale = millimetersPerDocumentUnit(defaultUnit);
    const auto stripSuffix = [&](const QStringList &suffixes, qreal scale) {
        for (const QString &suffix : suffixes) {
            if (text.endsWith(suffix)) {
                text.chop(suffix.size());
                text = text.trimmed();
                unitScale = scale;
                return true;
            }
        }
        return false;
    };
    if (text.endsWith(QLatin1Char('"')) || text.endsWith(QStringLiteral("in")) ||
        text.endsWith(QStringLiteral("inch")) || text.endsWith(QStringLiteral("inches"))) {
        text.remove(QLatin1Char('"'));
        stripSuffix({QStringLiteral("inches"), QStringLiteral("inch"),
                     QStringLiteral("in")}, 25.4);
    } else if (!stripSuffix({QStringLiteral("mm"), QStringLiteral("millimeters"),
                             QStringLiteral("millimeter")}, 1.0) &&
               !stripSuffix({QStringLiteral("µm"), QStringLiteral("um"),
                             QStringLiteral("micrometers"),
                             QStringLiteral("micrometer")}, 0.001) &&
               !stripSuffix({QStringLiteral("cm"), QStringLiteral("centimeters"),
                             QStringLiteral("centimeter")}, 10.0) &&
               !stripSuffix({QStringLiteral("km"), QStringLiteral("kilometers"),
                             QStringLiteral("kilometer")}, 1000000.0) &&
               !stripSuffix({QStringLiteral("m"), QStringLiteral("meters"),
                             QStringLiteral("meter")}, 1000.0) &&
               !stripSuffix({QStringLiteral("ft"), QStringLiteral("feet"),
                             QStringLiteral("foot")}, 304.8)) {
        // A bare imperial value uses the active document unit.
    }

    qreal value = 0.0;
    if (!parseAdditiveLength(text, &value)) return false;
    if (defaultUnit == DocumentLengthUnit::Foot &&
        unitScale == millimetersPerDocumentUnit(defaultUnit) &&
        text.contains(QLatin1Char(' '))) {
        const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        qreal feet = 0.0;
        qreal inches = 0.0;
        if (parts.size() >= 2 && parseLengthNumber(parts.first(), &feet) &&
            parseAdditiveLength(parts.mid(1).join(QLatin1Char(' ')), &inches)) {
            value = feet * 12.0 + inches;
            unitScale = 25.4;
        }
    }
    *millimeters = value * unitScale;
    return std::isfinite(*millimeters);
}

} // namespace classiCAD
