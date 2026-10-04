/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "document_grid_dialog.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>

namespace classiCAD {
namespace {

class DocumentGridDialog final : public QDialog {
public:
    explicit DocumentGridDialog(const DocumentSettings &settings,
                                QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Document Grid Settings"));
        setModal(true);
        auto *root = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        unitCombo_ = new QComboBox;
        for (const DocumentLengthUnit unit : {
                 DocumentLengthUnit::Millimeter,
                 DocumentLengthUnit::Centimeter,
                 DocumentLengthUnit::Meter,
                 DocumentLengthUnit::Inch,
                 DocumentLengthUnit::Foot}) {
            unitCombo_->addItem(documentLengthUnitName(unit), static_cast<int>(unit));
        }
        const int unitIndex = unitCombo_->findData(static_cast<int>(settings.lengthUnit));
        unitCombo_->setCurrentIndex(std::max(unitIndex, 0));
        form->addRow(QStringLiteral("Document length unit"), unitCombo_);

        spacingSpinBox_ = new QDoubleSpinBox;
        spacingSpinBox_->setDecimals(6);
        spacingSpinBox_->setRange(0.000001, 1.0e9);
        spacingSpinBox_->setValue(settings.gridSpacing);
        spacingSpinBox_->setObjectName(QStringLiteral("documentGridSpacing"));
        form->addRow(QStringLiteral("Base grid spacing"), spacingSpinBox_);
        root->addLayout(form);

        auto *hint = new QLabel(QStringLiteral(
            "Geometry remains stored in millimeters; these settings control the "
            "document grid and its displayed unit scale."));
        hint->setWordWrap(true);
        root->addWidget(hint);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);
    }

    DocumentSettings settings() const
    {
        DocumentSettings result;
        result.lengthUnit = static_cast<DocumentLengthUnit>(unitCombo_->currentData().toInt());
        result.gridSpacing = spacingSpinBox_->value();
        return result;
    }

private:
    QComboBox *unitCombo_ = nullptr;
    QDoubleSpinBox *spacingSpinBox_ = nullptr;
};

} // namespace

bool showDocumentGridDialog(QWidget *parent,
                            const DocumentSettings &initialSettings,
                            DocumentSettings *selectedSettings)
{
    if (selectedSettings == nullptr) {
        return false;
    }
    DocumentGridDialog dialog(initialSettings, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    *selectedSettings = dialog.settings();
    return true;
}

} // namespace classiCAD
