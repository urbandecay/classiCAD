/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "preferences_dialog.h"

#include "services/dimensions/dimension_font.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>

namespace classiCAD {
namespace {

class PreferencesDialog final : public QDialog {
public:
    explicit PreferencesDialog(Qt::MouseButton panButton,
                               bool snapLabelsVisible,
                               bool smoothCurveDisplay,
                               bool architecturalDimensionFont,
                               const BlenderGridAppearance &gridAppearance,
                               const ViewportCameraPreferences &cameraPreferences,
                               const ViewportNavigationPreferences &navigationPreferences,
                               const RotateToolPreferences &rotateToolPreferences,
                               int viewportAaSamples,
                               bool smoothWiresOverlay,
                               bool smoothWiresEditMode,
                               QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(800, 660);
        setModal(true);

        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(10, 10, 10, 10);

        auto *contentLayout = new QHBoxLayout;
        contentLayout->setSpacing(10);
        rootLayout->addLayout(contentLayout, 1);

        categoryList_ = new QListWidget;
        categoryList_->setObjectName(QStringLiteral("preferencesCategories"));
        categoryList_->setFixedWidth(165);

        const QStringList categoryNames{
            QStringLiteral("Interface"),
            QStringLiteral("Viewport"),
            QStringLiteral("Dimensions"),
            QStringLiteral("Lights"),
            QStringLiteral("Editing"),
            QStringLiteral("Animation"),
            QStringLiteral("Get Extensions"),
            QStringLiteral("Add-ons"),
            QStringLiteral("Themes"),
            QStringLiteral("Asset Libraries"),
            QStringLiteral("Input"),
            QStringLiteral("Navigation"),
            QStringLiteral("Keymap"),
            QStringLiteral("System"),
            QStringLiteral("Save & Load"),
            QStringLiteral("File Paths"),
        };
        categoryList_->addItems(categoryNames);
        contentLayout->addWidget(categoryList_);

        pages_ = new QStackedWidget;
        for (const QString &category : categoryNames) {
            if (category == QStringLiteral("Viewport")) {
                pages_->addWidget(createViewportPage(snapLabelsVisible,
                                                     smoothCurveDisplay,
                                                     gridAppearance,
                                                     cameraPreferences,
                                                     rotateToolPreferences,
                                                     viewportAaSamples,
                                                     smoothWiresOverlay,
                                                     smoothWiresEditMode));
            } else if (category == QStringLiteral("Dimensions")) {
                pages_->addWidget(createDimensionPage(architecturalDimensionFont));
            } else if (category == QStringLiteral("Navigation")) {
                pages_->addWidget(createNavigationPage(navigationPreferences));
            } else if (category == QStringLiteral("Keymap")) {
                pages_->addWidget(createKeymapPage(panButton));
            } else if (category == QStringLiteral("System")) {
                pages_->addWidget(createSystemPage());
            } else {
                pages_->addWidget(createPlaceholderPage(category));
            }
        }
        contentLayout->addWidget(pages_, 1);

        auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        rootLayout->addWidget(buttonBox);

        connect(categoryList_, &QListWidget::currentRowChanged,
                pages_, &QStackedWidget::setCurrentIndex);
        categoryList_->setCurrentRow(categoryNames.indexOf(QStringLiteral("Keymap")));
    }

    Qt::MouseButton panButton() const
    {
        return panButtonCombo_->currentIndex() == 1 ? Qt::RightButton : Qt::MiddleButton;
    }

    bool snapLabelsVisible() const
    {
        return snapLabelsCheckBox_->isChecked();
    }

    bool smoothCurveDisplay() const
    {
        return smoothCurveDisplayCheckBox_->isChecked();
    }

    BlenderGridAppearance gridAppearance() const
    {
        BlenderGridAppearance appearance = gridAppearance_;
        appearance.opacity = gridOpacitySpinBox_->value();
        appearance.lowAlphaStipple = gridStippleCheckBox_->isChecked();
        return appearance;
    }

    bool architecturalDimensionFont() const
    {
        return dimensionFontCombo_->currentData().toBool();
    }

    ViewportCameraPreferences cameraPreferences() const
    {
        return {focalLengthSpinBox_->value(),
                clipStartSpinBox_->value(),
                clipEndSpinBox_->value()};
    }

    ViewportNavigationPreferences navigationPreferences() const
    {
        ViewportNavigationPreferences preferences;
        preferences.autoPerspective = autoPerspectiveCheckBox_->isChecked();
        preferences.zoomToMouse = zoomToMouseCheckBox_->isChecked();
        preferences.orbitAroundActive = orbitAroundActiveCheckBox_->isChecked();
        preferences.useMouseDepthNavigate = mouseDepthNavigateCheckBox_->isChecked();
        preferences.turntableSensitivityRadiansPerPixel =
            qDegreesToRadians(turntableSensitivitySpinBox_->value());
        preferences.orbitMethod = orbitMethodCombo_->currentData().toInt() == 1
                                      ? ViewportOrbitMethod::Trackball
                                      : ViewportOrbitMethod::Turntable;
        preferences.trackballSensitivity = trackballSensitivitySpinBox_->value();
        preferences.invertMouseZoom = invertMouseZoomCheckBox_->isChecked();
        preferences.invertZoomWheel = invertZoomWheelCheckBox_->isChecked();
        preferences.zoomMethod = zoomMethodCombo_->currentData().toInt() == 1
                                     ? ViewportZoomMethod::Scale
                                     : ViewportZoomMethod::Dolly;
        preferences.zoomAxis = zoomAxisCombo_->currentData().toInt() == 1
                                   ? ViewportZoomAxis::Horizontal
                                   : ViewportZoomAxis::Vertical;
        return preferences;
    }

    RotateToolPreferences rotateToolPreferences() const
    {
        RotateToolPreferences preferences = rotateToolPreferences_;
        const qreal selectedIncrement =
            rotateAngleSnapIncrementCombo_->currentData().toDouble();
        if (rotateAngleInputRadiansCheckBox_->isChecked()) {
            preferences.angleSnapIncrementRadiansDegrees = selectedIncrement;
        } else {
            preferences.angleSnapIncrementDegrees = selectedIncrement;
        }
        preferences.angleSnapStrengthDegrees =
            rotateAngleSnapStrengthSpinBox_->value();
        preferences.angleSnapEnabled = rotateAngleSnapEnabledCheckBox_->isChecked();
        preferences.useRadians = rotateAngleInputRadiansCheckBox_->isChecked();
        return preferences;
    }

    int viewportAaSamples() const
    {
        return viewportAaCombo_->currentData().toInt();
    }

    bool smoothWiresOverlay() const
    {
        return smoothWiresOverlayCheckBox_->isChecked();
    }

    bool smoothWiresEditMode() const
    {
        return smoothWiresEditModeCheckBox_->isChecked();
    }

private:
    void populateRotateSnapIncrements(bool useRadians)
    {
        if (rotateAngleSnapIncrementCombo_ == nullptr) {
            return;
        }
        rotateAngleSnapIncrementCombo_->clear();
        const qreal degreeIncrements[]{1.0, 2.0, 3.0, 5.0, 10.0,
                                        15.0, 22.5, 30.0, 45.0, 90.0};
        const qreal radianIncrements[]{1.0, 2.0, 3.0, 5.0, 10.0,
                                        15.0, 22.5, 30.0, 45.0, 60.0, 90.0};
        if (useRadians) {
            const int denominators[]{180, 90, 60, 36, 18, 12, 8, 6, 4, 3, 2};
            for (int index = 0; index < 11; ++index) {
                rotateAngleSnapIncrementCombo_->addItem(
                    QStringLiteral("π/%1 (%2°)")
                        .arg(denominators[index])
                        .arg(radianIncrements[index], 0, 'g', 4),
                    radianIncrements[index]);
            }
        } else {
            for (const qreal increment : degreeIncrements) {
                if (qFuzzyCompare(
                        rotateToolPreferences_.angleSnapIncrementDegrees, 60.0) &&
                    qFuzzyCompare(increment, 90.0)) {
                    rotateAngleSnapIncrementCombo_->addItem(
                        QStringLiteral("60 Degrees"), 60.0);
                }
                rotateAngleSnapIncrementCombo_->addItem(
                    QStringLiteral("%1 %2")
                        .arg(increment, 0, 'g', 4)
                        .arg(qFuzzyCompare(increment, 1.0)
                                 ? QStringLiteral("Degree")
                                 : QStringLiteral("Degrees")),
                    increment);
            }
        }
        const qreal selectedIncrement = useRadians
                                            ? rotateToolPreferences_.angleSnapIncrementRadiansDegrees
                                            : rotateToolPreferences_.angleSnapIncrementDegrees;
        int index = rotateAngleSnapIncrementCombo_->findData(selectedIncrement);
        if (index < 0) {
            index = rotateAngleSnapIncrementCombo_->findData(15.0);
        }
        rotateAngleSnapIncrementCombo_->setCurrentIndex(index);
    }

    QWidget *createPlaceholderPage(const QString &category)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);

        auto *title = new QLabel(category);
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *description = new QLabel(QStringLiteral("The %1 preferences will be added here.")
                                           .arg(category));
        description->setObjectName(QStringLiteral("preferencesHint"));
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addStretch(1);
        return page;
    }

    QWidget *createKeymapPage(Qt::MouseButton panButton)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("Keymap"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *navigationBox = new QGroupBox(QStringLiteral("Viewport Navigation"));
        auto *navigationLayout = new QFormLayout(navigationBox);
        panButtonCombo_ = new QComboBox;
        panButtonCombo_->addItem(QStringLiteral("None (MMB only)"));
        panButtonCombo_->addItem(QStringLiteral("Right Mouse Button"));
        panButtonCombo_->setCurrentIndex(panButton == Qt::RightButton ? 1 : 0);
        navigationLayout->addRow(QStringLiteral("Additional pan button"), panButtonCombo_);
        layout->addWidget(navigationBox);

        auto *modelingBox = new QGroupBox(QStringLiteral("Modeling Shortcuts"));
        auto *modelingLayout = new QFormLayout(modelingBox);
        modelingLayout->addRow(QStringLiteral("Scale 1D"),
                               new QLabel(QStringLiteral("S, then 1"), modelingBox));
        modelingLayout->addRow(QStringLiteral("Scale 2D"),
                               new QLabel(QStringLiteral("S, then 2"), modelingBox));
        modelingLayout->addRow(QStringLiteral("Control Points"),
                               new QLabel(QStringLiteral("C, then P (toggle)"), modelingBox));
        layout->addWidget(modelingBox);

        auto *hint = new QLabel(QStringLiteral(
            "Choose which mouse button pans the viewport. The mouse wheel continues to zoom, "
            "and Alt + Left Mouse Button remains available as an alternate pan shortcut."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addStretch(1);
        return page;
    }

    QPushButton *addGridColorButton(QFormLayout *layout,
                                    const QString &label,
                                    QColor BlenderGridAppearance::*colorMember,
                                    const QString &objectName)
    {
        auto *button = new QPushButton;
        button->setObjectName(objectName);
        const auto updateButton = [button](const QColor &color) {
            button->setText(color.name(QColor::HexArgb).toUpper());
            button->setStyleSheet(
                QStringLiteral("background-color: rgba(%1,%2,%3,%4);")
                    .arg(color.red())
                    .arg(color.green())
                    .arg(color.blue())
                    .arg(color.alpha()));
        };
        updateButton(gridAppearance_.*colorMember);
        connect(button, &QPushButton::clicked, this,
                [this, button, colorMember, label, updateButton]() {
                    const QColor selected = QColorDialog::getColor(
                        gridAppearance_.*colorMember,
                        this,
                        label,
                        QColorDialog::ShowAlphaChannel);
                    if (selected.isValid()) {
                        gridAppearance_.*colorMember = selected;
                        updateButton(selected);
                    }
                });
        layout->addRow(label, button);
        return button;
    }

    QWidget *createViewportPage(bool snapLabelsVisible,
                                bool smoothCurveDisplay,
                                const BlenderGridAppearance &gridAppearance,
                                const ViewportCameraPreferences &cameraPreferences,
                                const RotateToolPreferences &rotateToolPreferences,
                                int viewportAaSamples,
                                bool smoothWiresOverlay,
                                bool smoothWiresEditMode)
    {
        gridAppearance_ = gridAppearance;
        rotateToolPreferences_ = rotateToolPreferences;
        auto *scrollArea = new QScrollArea;
        scrollArea->setWidgetResizable(true);
        auto *content = new QWidget;
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("Viewport"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *qualityBox = new QGroupBox(QStringLiteral("Quality"));
        auto *qualityLayout = new QFormLayout(qualityBox);
        viewportAaCombo_ = new QComboBox;
        viewportAaCombo_->addItem(QStringLiteral("Off"), 0);
        viewportAaCombo_->addItem(QStringLiteral("2 Samples"), 2);
        viewportAaCombo_->addItem(QStringLiteral("4 Samples"), 4);
        viewportAaCombo_->addItem(QStringLiteral("8 Samples"), 8);
        viewportAaCombo_->setCurrentIndex(viewportAaCombo_->findData(viewportAaSamples));
        viewportAaCombo_->setObjectName(QStringLiteral("viewportAntiAliasingPreference"));
        qualityLayout->addRow(QStringLiteral("Viewport Anti-Aliasing"), viewportAaCombo_);
        smoothWiresOverlayCheckBox_ = new QCheckBox(QStringLiteral("Overlay"));
        smoothWiresOverlayCheckBox_->setChecked(smoothWiresOverlay);
        smoothWiresOverlayCheckBox_->setObjectName(
            QStringLiteral("smoothWiresOverlayPreference"));
        qualityLayout->addRow(QStringLiteral("Smooth Wires"),
                              smoothWiresOverlayCheckBox_);
        smoothWiresEditModeCheckBox_ = new QCheckBox(QStringLiteral("Edit Mode"));
        smoothWiresEditModeCheckBox_->setChecked(smoothWiresEditMode);
        smoothWiresEditModeCheckBox_->setObjectName(
            QStringLiteral("smoothWiresEditModePreference"));
        qualityLayout->addRow(QString(), smoothWiresEditModeCheckBox_);
        layout->addWidget(qualityBox);

        auto *feedbackBox = new QGroupBox(QStringLiteral("Snap Feedback"));
        auto *feedbackLayout = new QVBoxLayout(feedbackBox);
        snapLabelsCheckBox_ = new QCheckBox(QStringLiteral("Show snap type labels"));
        snapLabelsCheckBox_->setObjectName(QStringLiteral("snapLabelsPreference"));
        snapLabelsCheckBox_->setChecked(snapLabelsVisible);
        feedbackLayout->addWidget(snapLabelsCheckBox_);

        auto *hint = new QLabel(QStringLiteral(
            "Display the snap type, such as Endpoint, Midpoint, or Tangent, beside its marker."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        feedbackLayout->addWidget(hint);
        layout->addWidget(feedbackBox);

        auto *curveBox = new QGroupBox(QStringLiteral("Curve Display"));
        auto *curveLayout = new QVBoxLayout(curveBox);
        smoothCurveDisplayCheckBox_ = new QCheckBox(
            QStringLiteral("Smooth curves when zoomed in"));
        smoothCurveDisplayCheckBox_->setObjectName(
            QStringLiteral("smoothCurveDisplayPreference"));
        smoothCurveDisplayCheckBox_->setChecked(smoothCurveDisplay);
        curveLayout->addWidget(smoothCurveDisplayCheckBox_);

        auto *curveHint = new QLabel(QStringLiteral(
            "Add drawing detail as you zoom in. Turn this off to use a faster, fixed-detail display."));
        curveHint->setObjectName(QStringLiteral("preferencesHint"));
        curveHint->setWordWrap(true);
        curveLayout->addWidget(curveHint);
        layout->addWidget(curveBox);

        auto *rotateBox = new QGroupBox(QStringLiteral("Rotate Tool"));
        auto *rotateLayout = new QFormLayout(rotateBox);
        rotateAngleSnapEnabledCheckBox_ = new QCheckBox(
            QStringLiteral("Enable soft angle snap"));
        rotateAngleSnapEnabledCheckBox_->setObjectName(
            QStringLiteral("rotateAngleSnapEnabledPreference"));
        rotateAngleSnapEnabledCheckBox_->setChecked(
            rotateToolPreferences.angleSnapEnabled);
        rotateLayout->addRow(QString(), rotateAngleSnapEnabledCheckBox_);

        rotateAngleSnapIncrementCombo_ = new QComboBox;
        rotateAngleSnapIncrementCombo_->setObjectName(
            QStringLiteral("rotateAngleSnapIncrementPreference"));
        populateRotateSnapIncrements(rotateToolPreferences.useRadians);
        rotateLayout->addRow(QStringLiteral("Snap increment"),
                             rotateAngleSnapIncrementCombo_);

        rotateAngleSnapStrengthSpinBox_ = new QDoubleSpinBox;
        rotateAngleSnapStrengthSpinBox_->setObjectName(
            QStringLiteral("rotateAngleSnapStrengthPreference"));
        rotateAngleSnapStrengthSpinBox_->setRange(0.1, 45.0);
        rotateAngleSnapStrengthSpinBox_->setSingleStep(0.5);
        rotateAngleSnapStrengthSpinBox_->setDecimals(1);
        rotateAngleSnapStrengthSpinBox_->setSuffix(QStringLiteral("°"));
        rotateAngleSnapStrengthSpinBox_->setValue(
            rotateToolPreferences.angleSnapStrengthDegrees);
        rotateLayout->addRow(QStringLiteral("Snap tolerance"),
                             rotateAngleSnapStrengthSpinBox_);

        rotateAngleInputRadiansCheckBox_ = new QCheckBox(
            QStringLiteral("Use radians for angle readout"));
        rotateAngleInputRadiansCheckBox_->setObjectName(
            QStringLiteral("rotateAngleInputRadiansPreference"));
        rotateAngleInputRadiansCheckBox_->setChecked(
            rotateToolPreferences.useRadians);
        rotateLayout->addRow(QString(), rotateAngleInputRadiansCheckBox_);
        connect(rotateAngleInputRadiansCheckBox_, &QCheckBox::toggled,
                this, [this](bool useRadians) {
                    const qreal selectedIncrement =
                        rotateAngleSnapIncrementCombo_->currentData().toDouble();
                    if (rotateToolPreferences_.useRadians) {
                        rotateToolPreferences_.angleSnapIncrementRadiansDegrees =
                            selectedIncrement;
                    } else {
                        rotateToolPreferences_.angleSnapIncrementDegrees =
                            selectedIncrement;
                    }
                    rotateToolPreferences_.useRadians = useRadians;
                    populateRotateSnapIncrements(useRadians);
                });
        auto *rotateHint = new QLabel(QStringLiteral(
            "C toggles angle snapping while rotating. Typed angles use degrees; positive values follow the current turn direction."));
        rotateHint->setObjectName(QStringLiteral("preferencesHint"));
        rotateHint->setWordWrap(true);
        rotateLayout->addRow(rotateHint);
        layout->addWidget(rotateBox);

        auto *gridBox = new QGroupBox(QStringLiteral("Grid and Axes"));
        auto *gridLayout = new QFormLayout(gridBox);
        addGridColorButton(gridLayout,
                           QStringLiteral("Grid lines"),
                           &BlenderGridAppearance::gridColor,
                           QStringLiteral("gridColorPreference"));
        addGridColorButton(gridLayout,
                           QStringLiteral("Emphasis lines"),
                           &BlenderGridAppearance::emphasisColor,
                           QStringLiteral("gridEmphasisColorPreference"));
        addGridColorButton(gridLayout,
                           QStringLiteral("X axis"),
                           &BlenderGridAppearance::axisXColor,
                           QStringLiteral("gridAxisXColorPreference"));
        addGridColorButton(gridLayout,
                           QStringLiteral("Y axis"),
                           &BlenderGridAppearance::axisYColor,
                           QStringLiteral("gridAxisYColorPreference"));
        addGridColorButton(gridLayout,
                           QStringLiteral("Z axis"),
                           &BlenderGridAppearance::axisZColor,
                           QStringLiteral("gridAxisZColorPreference"));
        gridOpacitySpinBox_ = new QDoubleSpinBox;
        gridOpacitySpinBox_->setRange(0.0, 2.0);
        gridOpacitySpinBox_->setSingleStep(0.05);
        gridOpacitySpinBox_->setDecimals(2);
        gridOpacitySpinBox_->setValue(gridAppearance.opacity);
        gridOpacitySpinBox_->setObjectName(QStringLiteral("gridOpacityPreference"));
        gridLayout->addRow(QStringLiteral("Opacity"), gridOpacitySpinBox_);
        gridStippleCheckBox_ = new QCheckBox(QStringLiteral("Stipple low-opacity lines"));
        gridStippleCheckBox_->setChecked(gridAppearance.lowAlphaStipple);
        gridStippleCheckBox_->setObjectName(QStringLiteral("gridStipplePreference"));
        gridLayout->addRow(QString(), gridStippleCheckBox_);
        layout->addWidget(gridBox);

        auto *cameraBox = new QGroupBox(QStringLiteral("View Camera"));
        auto *cameraLayout = new QFormLayout(cameraBox);
        focalLengthSpinBox_ = new QDoubleSpinBox;
        focalLengthSpinBox_->setRange(1.0, 2000.0);
        focalLengthSpinBox_->setDecimals(1);
        focalLengthSpinBox_->setSingleStep(1.0);
        focalLengthSpinBox_->setValue(cameraPreferences.focalLengthMillimeters);
        focalLengthSpinBox_->setSuffix(QStringLiteral(" mm"));
        focalLengthSpinBox_->setObjectName(QStringLiteral("viewportFocalLengthPreference"));
        cameraLayout->addRow(QStringLiteral("Focal length"), focalLengthSpinBox_);

        clipStartSpinBox_ = new QDoubleSpinBox;
        clipStartSpinBox_->setRange(0.000001, 1.0e8);
        clipStartSpinBox_->setDecimals(6);
        clipStartSpinBox_->setSingleStep(0.01);
        clipStartSpinBox_->setValue(cameraPreferences.clipStart);
        clipStartSpinBox_->setObjectName(QStringLiteral("viewportClipStartPreference"));
        cameraLayout->addRow(QStringLiteral("Clip start"), clipStartSpinBox_);

        clipEndSpinBox_ = new QDoubleSpinBox;
        clipEndSpinBox_->setRange(0.000002, 1.0e9);
        clipEndSpinBox_->setDecimals(6);
        clipEndSpinBox_->setSingleStep(1.0);
        clipEndSpinBox_->setValue(cameraPreferences.clipEnd);
        clipEndSpinBox_->setObjectName(QStringLiteral("viewportClipEndPreference"));
        cameraLayout->addRow(QStringLiteral("Clip end"), clipEndSpinBox_);
        connect(clipStartSpinBox_, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this](double start) {
                    const double minimumEnd = start + std::max(0.000001, start * 1.0e-6);
                    clipEndSpinBox_->setMinimum(minimumEnd);
                });
        layout->addWidget(cameraBox);

        auto *cameraHint = new QLabel(QStringLiteral(
            "These camera and clipping values drive perspective, picking, scene depth, and the grid. "
            "Distances use the document's model units."));
        cameraHint->setObjectName(QStringLiteral("preferencesHint"));
        cameraHint->setWordWrap(true);
        layout->addWidget(cameraHint);

        auto *gridHint = new QLabel(QStringLiteral(
            "Grid and axis colors are theme values. Low-opacity stipple and camera fades "
            "use Blender's viewport behavior."));
        gridHint->setObjectName(QStringLiteral("preferencesHint"));
        gridHint->setWordWrap(true);
        layout->addWidget(gridHint);
        layout->addStretch(1);
        scrollArea->setWidget(content);
        return scrollArea;
    }

    QWidget *createDimensionPage(bool architecturalDimensionFont)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("Dimensions"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *textBox = new QGroupBox(QStringLiteral("Annotation Text"));
        auto *textLayout = new QFormLayout(textBox);
        dimensionFontCombo_ = new QComboBox;
        dimensionFontCombo_->addItem(QStringLiteral("Standard"), false);
        dimensionFontCombo_->addItem(QStringLiteral("Architectural"), true);
        dimensionFontCombo_->setCurrentIndex(architecturalDimensionFont ? 1 : 0);
        textLayout->addRow(QStringLiteral("Font style"), dimensionFontCombo_);

        auto *sample = new QLabel(QStringLiteral("123.45 mm   90°"));
        sample->setAlignment(Qt::AlignCenter);
        sample->setMinimumHeight(36);
        textLayout->addRow(QStringLiteral("Preview"), sample);
        const auto updateSampleFont = [this, sample]() {
            const DimensionFontStyle style = dimensionFontCombo_->currentData().toBool()
                                                 ? DimensionFontStyle::Architectural
                                                 : DimensionFontStyle::Standard;
            sample->setFont(dimensionAnnotationFont(style));
        };
        connect(dimensionFontCombo_, &QComboBox::currentTextChanged,
                this, [updateSampleFont](const QString &) { updateSampleFont(); });
        updateSampleFont();
        layout->addWidget(textBox);

        auto *hint = new QLabel(QStringLiteral(
            "Architectural lettering uses a hand-drafted font for dimension labels. "
            "It changes annotations only; the rest of the interface stays the same."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addStretch(1);
        return page;
    }

    QWidget *createNavigationPage(
        const ViewportNavigationPreferences &preferences)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("Navigation"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *orbitBox = new QGroupBox(QStringLiteral("Orbit"));
        auto *orbitLayout = new QVBoxLayout(orbitBox);
        autoPerspectiveCheckBox_ = new QCheckBox(
            QStringLiteral("Auto perspective when orbiting"));
        autoPerspectiveCheckBox_->setChecked(preferences.autoPerspective);
        autoPerspectiveCheckBox_->setObjectName(
            QStringLiteral("viewportAutoPerspectivePreference"));
        orbitLayout->addWidget(autoPerspectiveCheckBox_);

        orbitAroundActiveCheckBox_ = new QCheckBox(
            QStringLiteral("Orbit around the active object"));
        orbitAroundActiveCheckBox_->setChecked(preferences.orbitAroundActive);
        orbitAroundActiveCheckBox_->setObjectName(
            QStringLiteral("viewportOrbitAroundActivePreference"));
        orbitLayout->addWidget(orbitAroundActiveCheckBox_);

        mouseDepthNavigateCheckBox_ = new QCheckBox(
            QStringLiteral("Use mouse depth when orbiting over geometry"));
        mouseDepthNavigateCheckBox_->setChecked(preferences.useMouseDepthNavigate);
        mouseDepthNavigateCheckBox_->setObjectName(
            QStringLiteral("viewportMouseDepthNavigatePreference"));
        orbitLayout->addWidget(mouseDepthNavigateCheckBox_);

        auto *sensitivityLayout = new QFormLayout;
        orbitMethodCombo_ = new QComboBox;
        orbitMethodCombo_->addItem(QStringLiteral("Turntable"), 0);
        orbitMethodCombo_->addItem(QStringLiteral("Trackball"), 1);
        orbitMethodCombo_->setCurrentIndex(
            orbitMethodCombo_->findData(
                preferences.orbitMethod == ViewportOrbitMethod::Trackball ? 1 : 0));
        orbitMethodCombo_->setObjectName(QStringLiteral("viewportOrbitMethodPreference"));
        sensitivityLayout->addRow(QStringLiteral("Rotation method"), orbitMethodCombo_);

        turntableSensitivitySpinBox_ = new QDoubleSpinBox;
        turntableSensitivitySpinBox_->setRange(0.01, 5.0);
        turntableSensitivitySpinBox_->setDecimals(2);
        turntableSensitivitySpinBox_->setSingleStep(0.05);
        turntableSensitivitySpinBox_->setValue(
            qRadiansToDegrees(preferences.turntableSensitivityRadiansPerPixel));
        turntableSensitivitySpinBox_->setSuffix(QStringLiteral("° per pixel"));
        turntableSensitivitySpinBox_->setObjectName(
            QStringLiteral("viewportTurntableSensitivityPreference"));
        sensitivityLayout->addRow(QStringLiteral("Turntable sensitivity"),
                                  turntableSensitivitySpinBox_);

        trackballSensitivitySpinBox_ = new QDoubleSpinBox;
        trackballSensitivitySpinBox_->setRange(0.01, 10.0);
        trackballSensitivitySpinBox_->setDecimals(2);
        trackballSensitivitySpinBox_->setSingleStep(0.1);
        trackballSensitivitySpinBox_->setValue(preferences.trackballSensitivity);
        trackballSensitivitySpinBox_->setSuffix(QStringLiteral("×"));
        trackballSensitivitySpinBox_->setObjectName(
            QStringLiteral("viewportTrackballSensitivityPreference"));
        sensitivityLayout->addRow(QStringLiteral("Trackball sensitivity"),
                                  trackballSensitivitySpinBox_);

        QWidget *turntableSensitivityLabel =
            sensitivityLayout->labelForField(turntableSensitivitySpinBox_);
        QWidget *trackballSensitivityLabel =
            sensitivityLayout->labelForField(trackballSensitivitySpinBox_);
        const auto updateSensitivityFields = [this,
                                              turntableSensitivityLabel,
                                              trackballSensitivityLabel]() {
            const bool trackball = orbitMethodCombo_->currentData().toInt() == 1;
            turntableSensitivitySpinBox_->setVisible(!trackball);
            if (turntableSensitivityLabel != nullptr) {
                turntableSensitivityLabel->setVisible(!trackball);
            }
            trackballSensitivitySpinBox_->setVisible(trackball);
            if (trackballSensitivityLabel != nullptr) {
                trackballSensitivityLabel->setVisible(trackball);
            }
        };
        connect(orbitMethodCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [updateSensitivityFields](int) { updateSensitivityFields(); });
        updateSensitivityFields();
        orbitLayout->addLayout(sensitivityLayout);
        layout->addWidget(orbitBox);

        auto *zoomBox = new QGroupBox(QStringLiteral("Zoom"));
        auto *zoomLayout = new QFormLayout(zoomBox);
        zoomToMouseCheckBox_ = new QCheckBox(
            QStringLiteral("Zoom toward mouse position"));
        zoomToMouseCheckBox_->setChecked(preferences.zoomToMouse);
        zoomToMouseCheckBox_->setObjectName(
            QStringLiteral("viewportZoomToMousePreference"));
        zoomLayout->addRow(QString(), zoomToMouseCheckBox_);

        zoomMethodCombo_ = new QComboBox;
        zoomMethodCombo_->addItem(QStringLiteral("Dolly"), 0);
        zoomMethodCombo_->addItem(QStringLiteral("Scale"), 1);
        zoomMethodCombo_->setCurrentIndex(
            zoomMethodCombo_->findData(preferences.zoomMethod == ViewportZoomMethod::Scale
                                           ? 1
                                           : 0));
        zoomMethodCombo_->setObjectName(QStringLiteral("viewportZoomMethodPreference"));
        zoomLayout->addRow(QStringLiteral("Zoom method"), zoomMethodCombo_);

        zoomAxisCombo_ = new QComboBox;
        zoomAxisCombo_->addItem(QStringLiteral("Vertical"), 0);
        zoomAxisCombo_->addItem(QStringLiteral("Horizontal"), 1);
        zoomAxisCombo_->setCurrentIndex(
            zoomAxisCombo_->findData(preferences.zoomAxis == ViewportZoomAxis::Horizontal
                                         ? 1
                                         : 0));
        zoomAxisCombo_->setObjectName(QStringLiteral("viewportZoomAxisPreference"));
        zoomLayout->addRow(QStringLiteral("Drag zoom axis"), zoomAxisCombo_);

        invertMouseZoomCheckBox_ = new QCheckBox(
            QStringLiteral("Invert drag zoom direction"));
        invertMouseZoomCheckBox_->setChecked(preferences.invertMouseZoom);
        invertMouseZoomCheckBox_->setObjectName(
            QStringLiteral("viewportInvertMouseZoomPreference"));
        zoomLayout->addRow(QString(), invertMouseZoomCheckBox_);

        invertZoomWheelCheckBox_ = new QCheckBox(
            QStringLiteral("Invert mouse wheel zoom"));
        invertZoomWheelCheckBox_->setChecked(preferences.invertZoomWheel);
        invertZoomWheelCheckBox_->setObjectName(
            QStringLiteral("viewportInvertZoomWheelPreference"));
        zoomLayout->addRow(QString(), invertZoomWheelCheckBox_);
        layout->addWidget(zoomBox);

        auto *hint = new QLabel(QStringLiteral(
            "Choose Blender-style turntable or trackball rotation and adjust its sensitivity. "
            "Mouse-depth orbit uses the geometry under the cursor."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addStretch(1);
        return page;
    }

    QWidget *createSystemPage()
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("System"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *viewportBox = new QGroupBox(QStringLiteral("Graphics Backend"));
        auto *viewportLayout = new QFormLayout(viewportBox);

        auto *backend = new QLabel(QStringLiteral(
            "OpenGL shader viewport; Qt rendering is used automatically if OpenGL is unavailable."));
        backend->setWordWrap(true);
        viewportLayout->addRow(QStringLiteral("Graphics backend"), backend);
        layout->addWidget(viewportBox);

        layout->addStretch(1);
        return page;
    }

    QListWidget *categoryList_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QComboBox *panButtonCombo_ = nullptr;
    QComboBox *dimensionFontCombo_ = nullptr;
    QCheckBox *snapLabelsCheckBox_ = nullptr;
    QCheckBox *smoothCurveDisplayCheckBox_ = nullptr;
    QCheckBox *rotateAngleSnapEnabledCheckBox_ = nullptr;
    QComboBox *rotateAngleSnapIncrementCombo_ = nullptr;
    QDoubleSpinBox *rotateAngleSnapStrengthSpinBox_ = nullptr;
    QCheckBox *rotateAngleInputRadiansCheckBox_ = nullptr;
    RotateToolPreferences rotateToolPreferences_;
    BlenderGridAppearance gridAppearance_;
    QDoubleSpinBox *gridOpacitySpinBox_ = nullptr;
    QCheckBox *gridStippleCheckBox_ = nullptr;
    QDoubleSpinBox *focalLengthSpinBox_ = nullptr;
    QDoubleSpinBox *clipStartSpinBox_ = nullptr;
    QDoubleSpinBox *clipEndSpinBox_ = nullptr;
    QCheckBox *autoPerspectiveCheckBox_ = nullptr;
    QCheckBox *zoomToMouseCheckBox_ = nullptr;
    QCheckBox *orbitAroundActiveCheckBox_ = nullptr;
    QCheckBox *mouseDepthNavigateCheckBox_ = nullptr;
    QComboBox *orbitMethodCombo_ = nullptr;
    QDoubleSpinBox *turntableSensitivitySpinBox_ = nullptr;
    QDoubleSpinBox *trackballSensitivitySpinBox_ = nullptr;
    QCheckBox *invertMouseZoomCheckBox_ = nullptr;
    QCheckBox *invertZoomWheelCheckBox_ = nullptr;
    QComboBox *zoomMethodCombo_ = nullptr;
    QComboBox *zoomAxisCombo_ = nullptr;
    QComboBox *viewportAaCombo_ = nullptr;
    QCheckBox *smoothWiresOverlayCheckBox_ = nullptr;
    QCheckBox *smoothWiresEditModeCheckBox_ = nullptr;
};

} // namespace

bool showPreferencesDialog(QWidget *parent,
                           const PreferencesDialogValues &initialValues,
                           PreferencesDialogValues *selectedValues)
{
    if (selectedValues == nullptr) {
        return false;
    }

    PreferencesDialog dialog(initialValues.panButton,
                             initialValues.snapLabelsVisible,
                             initialValues.smoothCurveDisplay,
                             initialValues.architecturalDimensionFont,
                             initialValues.gridAppearance,
                             initialValues.cameraPreferences,
                             initialValues.navigationPreferences,
                             initialValues.rotateToolPreferences,
                             initialValues.viewportAaSamples,
                             initialValues.smoothWiresOverlay,
                             initialValues.smoothWiresEditMode,
                             parent);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    *selectedValues = initialValues;
    selectedValues->panButton = dialog.panButton();
    selectedValues->snapLabelsVisible = dialog.snapLabelsVisible();
    selectedValues->smoothCurveDisplay = dialog.smoothCurveDisplay();
    selectedValues->architecturalDimensionFont = dialog.architecturalDimensionFont();
    selectedValues->gridAppearance = dialog.gridAppearance();
    selectedValues->cameraPreferences = dialog.cameraPreferences();
    selectedValues->navigationPreferences = dialog.navigationPreferences();
    selectedValues->rotateToolPreferences = dialog.rotateToolPreferences();
    selectedValues->viewportAaSamples = dialog.viewportAaSamples();
    selectedValues->smoothWiresOverlay = dialog.smoothWiresOverlay();
    selectedValues->smoothWiresEditMode = dialog.smoothWiresEditMode();
    return true;
}

} // namespace classiCAD
