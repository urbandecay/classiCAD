/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QComboBox>
#include <QIcon>
#include <QString>

namespace classiCAD {

class LayerLineTypeComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent *event) override;
};

QIcon makeLayerLineTypeIcon(const QString &lineType);
QString layerLineTypeDescription(const QString &lineType);
void populateLayerLineTypeCombo(QComboBox *combo, bool includeByLayer);

} // namespace classiCAD
