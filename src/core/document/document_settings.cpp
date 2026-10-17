/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "document_settings.h"

#include <QCryptographicHash>
#include <QSet>
#include <QtEndian>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace classiCAD {
namespace {

constexpr qsizetype kColorLutTotalByteLimit = 32 * 1024 * 1024;

bool areImportedColorLutsValid(
    const QHash<QString, QByteArray> &colorLuts,
    const QHash<QString, ImportedBlenderColorLutMetadata> &metadata)
{
    if (colorLuts.size() > 8 || metadata.size() != colorLuts.size()) return false;
    qsizetype totalBytes = 0;
    for (auto it = colorLuts.cbegin(); it != colorLuts.cend(); ++it) {
        const QString &id = it.key();
        const QByteArray &bytes = it.value();
        if (id.size() != 64 || !std::all_of(
                id.cbegin(), id.cend(), [](QChar c) {
                    return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                           (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
            }) || !metadata.contains(id) ||
            QString::fromLatin1(QCryptographicHash::hash(
                bytes, QCryptographicHash::Sha256).toHex()) != id) {
            return false;
        }
        const ImportedBlenderColorLutMetadata &lut = metadata[id];
        if (lut.size < 2 || lut.size > 140 ||
            !std::isfinite(lut.domainMin) || !std::isfinite(lut.domainMax) ||
            lut.domainMax <= lut.domainMin ||
            (lut.shaper != QStringLiteral("linear") &&
             lut.shaper != QStringLiteral("log2_1p")) ||
            !std::isfinite(lut.shaperScale) || lut.shaperScale <= 0.0 ||
            bytes.size() != qsizetype(lut.size) * lut.size * lut.size * 3 * 4) {
            return false;
        }
        const auto *source = reinterpret_cast<const uchar *>(bytes.constData());
        for (qsizetype offset = 0; offset < bytes.size();
             offset += static_cast<qsizetype>(sizeof(quint32))) {
            const quint32 bits = qFromLittleEndian<quint32>(source + offset);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) return false;
        }
        totalBytes += bytes.size();
        if (totalBytes > kColorLutTotalByteLimit) return false;
    }
    return true;
}

bool areActiveCollectionPathsValid(const QJsonObject &pathsByViewLayer)
{
    if (pathsByViewLayer.size() > 2048) return false;
    qsizetype totalCharacters = 0;
    for (auto it = pathsByViewLayer.begin(); it != pathsByViewLayer.end(); ++it) {
        if (it.key().size() > 16384 || !it.value().isArray()) return false;
        const QJsonArray path = it.value().toArray();
        if (path.isEmpty() || path.size() > 256) return false;
        totalCharacters += it.key().size();
        QSet<QString> uniqueSourceIds;
        for (const QJsonValue &sourceIdValue : path) {
            if (!sourceIdValue.isString()) return false;
            const QString sourceId = sourceIdValue.toString();
            if (sourceId.isEmpty() || sourceId.size() > 16384 ||
                uniqueSourceIds.contains(sourceId)) {
                return false;
            }
            uniqueSourceIds.insert(sourceId);
            totalCharacters += sourceId.size();
            if (totalCharacters > 2 * 1024 * 1024) return false;
        }
    }
    return true;
}

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
           settings.gridSpacing >= 1.0e-9 && settings.gridSpacing <= 1.0e9 &&
           areActiveCollectionPathsValid(
               settings.activeCollectionPathsByViewLayer) &&
           settings.importedBlenderLibraries.size() <= 1024 &&
           settings.importedBlenderOrphans.size() <= 1024 &&
           settings.importedBlenderSequencer.value(
               QStringLiteral("strips")).toArray().size() <= 10000 &&
           (settings.importedBlenderDataApi.isEmpty() ||
            (settings.importedBlenderDataApi.value(
                 QStringLiteral("root")).isObject() &&
             settings.importedBlenderDataApi.value(
                 QStringLiteral("truncated")).isBool())) &&
           (settings.importedBlenderLibraryOverrides.isEmpty() ||
            (settings.importedBlenderLibraryOverrides.value(
                 QStringLiteral("properties")).isObject() &&
             settings.importedBlenderLibraryOverrides.value(
                 QStringLiteral("hierarchies")).isArray() &&
             settings.importedBlenderLibraryOverrides.value(
                 QStringLiteral("truncated")).isBool() &&
             settings.importedBlenderLibraryOverrides.value(
                 QStringLiteral("has_libraries")).isBool() &&
             settings.importedBlenderLibraryOverrides.value(
                 QStringLiteral("hierarchies")).toArray().size() <= 1024)) &&
           settings.importedBlenderOutlinerOnlyObjects.size() <= 100000 &&
           areImportedColorLutsValid(settings.importedBlenderColorLuts,
                                     settings.importedBlenderColorLutMetadata);
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
