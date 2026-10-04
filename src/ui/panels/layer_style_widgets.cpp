/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "layer_style_widgets.h"

#include "ui/viewport/line_type_style.h"

#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStylePainter>

namespace classiCAD {

void LayerLineTypeComboBox::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.currentText.clear();
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

QIcon makeLayerLineTypeIcon(const QString &lineType)
{
    QPixmap pixmap(64, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(layerLineTypePen(QColor(220, 220, 220), 1.4, lineType));
    painter.drawLine(QPointF(1, 7), QPointF(63, 7));
    return QIcon(pixmap);
}

QString layerLineTypeDescription(const QString &lineType)
{
    const QString canonical = canonicalLayerLineTypeName(lineType);
    if (canonical == QStringLiteral("Continuous")) {
        return QStringLiteral("Solid line");
    }

    QString baseName = canonical;
    QString scaleDescription;
    if (baseName.endsWith(QStringLiteral("X2"))) {
        baseName.chop(2);
        scaleDescription = QStringLiteral(" (2x)");
    } else if (baseName.endsWith(QLatin1Char('2'))) {
        baseName.chop(1);
        scaleDescription = QStringLiteral(" (0.5x)");
    }
    return QStringLiteral("%1 pattern%2")
        .arg(baseName.toLower(), scaleDescription);
}

void populateLayerLineTypeCombo(QComboBox *combo, bool includeByLayer)
{
    if (combo == nullptr) {
        return;
    }
    combo->setIconSize(QSize(includeByLayer ? 48 : 40, 14));
    if (includeByLayer) {
        combo->addItem(makeLayerLineTypeIcon(QStringLiteral("Continuous")),
                       QStringLiteral("ByLayer"),
                       QStringLiteral("Continuous"));
        combo->setItemData(0, QStringLiteral("Use the layer's line style"),
                           Qt::ToolTipRole);
    }

    for (const QString &lineType : standardLayerLineTypes()) {
        const int index = combo->count();
        combo->addItem(makeLayerLineTypeIcon(lineType), lineType, lineType);
        combo->setItemData(index, layerLineTypeDescription(lineType),
                           Qt::ToolTipRole);
    }
}

} // namespace classiCAD
