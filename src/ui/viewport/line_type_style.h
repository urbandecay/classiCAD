#pragma once

#include <QColor>
#include <QPen>
#include <QString>
#include <QStringList>
#include <QVector>

namespace classiCAD {

inline QStringList standardLayerLineTypes()
{
    return {
        QStringLiteral("Continuous"),
        QStringLiteral("CENTER"), QStringLiteral("CENTER2"), QStringLiteral("CENTERX2"),
        QStringLiteral("DASHDOT"), QStringLiteral("DASHDOT2"), QStringLiteral("DASHDOTX2"),
        QStringLiteral("DASHED"), QStringLiteral("DASHED2"), QStringLiteral("DASHEDX2"),
        QStringLiteral("DIVIDE"), QStringLiteral("DIVIDE2"), QStringLiteral("DIVIDEX2"),
        QStringLiteral("DOT"), QStringLiteral("DOT2"), QStringLiteral("DOTX2"),
        QStringLiteral("HIDDEN"), QStringLiteral("HIDDEN2"), QStringLiteral("HIDDENX2"),
        QStringLiteral("BORDER"), QStringLiteral("BORDER2"), QStringLiteral("BORDERX2"),
        QStringLiteral("PHANTOM"), QStringLiteral("PHANTOM2"), QStringLiteral("PHANTOMX2"),
    };
}

inline QString canonicalLayerLineTypeName(const QString &lineType)
{
    QString normalized = lineType.trimmed().toUpper();
    normalized.remove(QLatin1Char('-'));
    normalized.remove(QLatin1Char('_'));
    normalized.remove(QLatin1Char(' '));

    if (normalized == QStringLiteral("CONTINUOUS")) {
        return QStringLiteral("Continuous");
    }
    if (normalized == QStringLiteral("DOTTED")) {
        return QStringLiteral("DOT");
    }
    if (normalized == QStringLiteral("DASHDOT")) {
        return QStringLiteral("DASHDOT");
    }

    for (const QString &standardName : standardLayerLineTypes()) {
        if (standardName.compare(normalized, Qt::CaseInsensitive) == 0) {
            return standardName;
        }
    }
    return lineType.trimmed();
}

inline QVector<qreal> layerLineTypePattern(const QString &lineType)
{
    QString name = canonicalLayerLineTypeName(lineType).toUpper();
    qreal scale = 1.0;
    if (name.endsWith(QStringLiteral("X2"))) {
        name.chop(2);
        scale = 2.0;
    } else if (name.endsWith(QLatin1Char('2'))) {
        name.chop(1);
        scale = 0.5;
    }

    QVector<qreal> pattern;
    if (name == QStringLiteral("CENTER")) {
        pattern = {10.0, 2.0, 1.0, 2.0, 1.0, 2.0};
    } else if (name == QStringLiteral("DASHDOT")) {
        pattern = {8.0, 2.0, 1.0, 2.0};
    } else if (name == QStringLiteral("DASHED") || name == QStringLiteral("HIDDEN")) {
        pattern = {7.0, 3.0};
    } else if (name == QStringLiteral("DIVIDE")) {
        pattern = {8.0, 2.0, 1.0, 2.0, 1.0, 2.0};
    } else if (name == QStringLiteral("DOT")) {
        pattern = {1.0, 2.0};
    } else if (name == QStringLiteral("BORDER")) {
        pattern = {8.0, 2.0, 2.0, 2.0, 2.0, 2.0};
    } else if (name == QStringLiteral("PHANTOM")) {
        pattern = {12.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
    }

    for (qreal &segment : pattern) {
        segment *= scale;
    }
    return pattern;
}

inline QPen layerLineTypePen(const QColor &color,
                             qreal width,
                             const QString &lineType,
                             Qt::PenCapStyle capStyle = Qt::FlatCap)
{
    const QVector<qreal> pattern = layerLineTypePattern(lineType);
    if (pattern.isEmpty()) {
        QPen pen(color, width, Qt::SolidLine, capStyle, Qt::RoundJoin);
        return pen;
    }

    const qreal safeWidth = qMax<qreal>(width, 0.25);
    QVector<qreal> penPattern;
    penPattern.reserve(pattern.size());
    for (qreal segment : pattern) {
        penPattern.append(segment / safeWidth);
    }

    QPen pen(color, width, Qt::CustomDashLine, capStyle, Qt::RoundJoin);
    pen.setDashPattern(penPattern);
    return pen;
}

} // namespace classiCAD
