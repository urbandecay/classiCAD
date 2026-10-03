#include "main_window.h"
#include "viewport_widget_api.h"
#include "viewport/line_type_style.h"
#include "input_helpers.h"
#include "../core/debug_log.h"
#include "../core/serialization/blender_project_file.h"
#include "../core/serialization/rhino3dm_interchange.h"
#include "services/dimensions/dimension_font.h"

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProcess>
#include <QPushButton>
#include <QPixmap>
#include <QShortcut>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>

namespace classiCAD {

enum class LayerHeaderIcon {
    Visibility,
    Freeze,
    Lock,
    Color,
    Plot,
};

QIcon makeLayerHeaderIcon(LayerHeaderIcon kind)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(185, 190, 198), 1.0,
                        Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
    case LayerHeaderIcon::Visibility:
        painter.drawEllipse(QRectF(1.5, 4.0, 13.0, 8.0));
        painter.setBrush(QColor(185, 190, 198));
        painter.drawEllipse(QRectF(6.0, 6.0, 4.0, 4.0));
        break;
    case LayerHeaderIcon::Freeze:
        painter.drawLine(QPointF(8, 1.5), QPointF(8, 14.5));
        painter.drawLine(QPointF(2.4, 4.8), QPointF(13.6, 11.2));
        painter.drawLine(QPointF(2.4, 11.2), QPointF(13.6, 4.8));
        painter.drawLine(QPointF(8, 1.5), QPointF(6.4, 3.2));
        painter.drawLine(QPointF(8, 1.5), QPointF(9.6, 3.2));
        painter.drawLine(QPointF(8, 14.5), QPointF(6.4, 12.8));
        painter.drawLine(QPointF(8, 14.5), QPointF(9.6, 12.8));
        break;
    case LayerHeaderIcon::Lock: {
        QPainterPath shackle;
        shackle.moveTo(5, 7.5);
        shackle.lineTo(5, 5.5);
        shackle.cubicTo(5, 1.7, 11, 1.7, 11, 5.5);
        shackle.lineTo(11, 7.5);
        painter.drawPath(shackle);
        painter.drawRoundedRect(QRectF(3.5, 7, 9, 7), 1, 1);
        painter.drawLine(QPointF(8, 9.2), QPointF(8, 11.4));
        break;
    }
    case LayerHeaderIcon::Color:
        painter.setBrush(QColor(235, 235, 235));
        painter.drawRect(QRectF(3, 3, 10, 10));
        break;
    case LayerHeaderIcon::Plot:
        painter.drawRect(QRectF(4, 1.8, 8, 5.2));
        painter.drawRoundedRect(QRectF(2, 5.5, 12, 6.3), 1, 1);
        painter.drawRect(QRectF(4, 9.5, 8, 4.5));
        painter.drawLine(QPointF(5.5, 11.5), QPointF(10.5, 11.5));
        painter.drawPoint(QPointF(11.5, 7.8));
        break;
    }

    return QIcon(pixmap);
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
    combo->setIconSize(QSize(includeByLayer ? 48 : 40, 14));
    if (includeByLayer) {
        combo->addItem(makeLayerLineTypeIcon(QStringLiteral("Continuous")),
                       QStringLiteral("ByLayer"),
                       QStringLiteral("Continuous"));
        combo->setItemData(0, QStringLiteral("Use the layer's line style"), Qt::ToolTipRole);
    }

    for (const QString &lineType : standardLayerLineTypes()) {
        const int index = combo->count();
        combo->addItem(makeLayerLineTypeIcon(lineType), lineType, lineType);
        combo->setItemData(index, layerLineTypeDescription(lineType), Qt::ToolTipRole);
    }
}

class LayerLineTypeComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.currentText.clear();
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

class ToolShelfScrollArea final : public QScrollArea {
public:
    explicit ToolShelfScrollArea(QWidget *parent = nullptr)
        : QScrollArea(parent)
    {
        setObjectName(QStringLiteral("toolShelfScrollArea"));
        setFrameShape(QFrame::NoFrame);
        setFixedWidth(98);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }

    void setShelfWidget(QWidget *shelf)
    {
        setWidget(shelf);
        shelf->installEventFilter(this);
        const auto descendants = shelf->findChildren<QWidget *>();
        for (QWidget *widget : descendants) {
            bool belongsToPopup = false;
            for (QWidget *ancestor = widget;
                 ancestor != nullptr && ancestor != shelf;
                 ancestor = ancestor->parentWidget()) {
                if (ancestor->isWindow()) {
                    belongsToPopup = true;
                    break;
                }
            }
            if (!belongsToPopup) {
                widget->installEventFilter(this);
            }
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Wheel) {
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            int scrollDelta = wheelEvent->pixelDelta().y();
            if (scrollDelta == 0) {
                scrollDelta = wheelEvent->angleDelta().y() * 48 / 120;
            }
            if (scrollDelta != 0) {
                QScrollBar *bar = verticalScrollBar();
                bar->setValue(bar->value() - scrollDelta);
            }
            wheelEvent->accept();
            return true;
        }
        return QScrollArea::eventFilter(watched, event);
    }
};

class PreferencesDialog final : public QDialog {
public:
    explicit PreferencesDialog(Qt::MouseButton panButton,
                               bool snapLabelsVisible,
                               bool smoothCurveDisplay,
                               bool architecturalDimensionFont,
                               const BlenderGridAppearance &gridAppearance,
                               const ViewportCameraPreferences &cameraPreferences,
                               const ViewportNavigationPreferences &navigationPreferences,
                               int viewportAaSamples,
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
                                                     cameraPreferences));
            } else if (category == QStringLiteral("Dimensions")) {
                pages_->addWidget(createDimensionPage(architecturalDimensionFont));
            } else if (category == QStringLiteral("Navigation")) {
                pages_->addWidget(createNavigationPage(navigationPreferences));
            } else if (category == QStringLiteral("Keymap")) {
                pages_->addWidget(createKeymapPage(panButton));
            } else if (category == QStringLiteral("System")) {
                pages_->addWidget(createSystemPage(viewportAaSamples));
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

    int viewportAaSamples() const
    {
        return viewportAaCombo_->currentData().toInt();
    }

private:
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
                                const ViewportCameraPreferences &cameraPreferences)
    {
        gridAppearance_ = gridAppearance;
        auto *scrollArea = new QScrollArea;
        scrollArea->setWidgetResizable(true);
        auto *content = new QWidget;
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("Viewport"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

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

    QWidget *createSystemPage(int viewportAaSamples)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("System"));
        title->setObjectName(QStringLiteral("preferencesTitle"));
        layout->addWidget(title);

        auto *viewportBox = new QGroupBox(QStringLiteral("Viewport"));
        auto *viewportLayout = new QFormLayout(viewportBox);
        viewportAaCombo_ = new QComboBox;
        viewportAaCombo_->addItem(QStringLiteral("Off"), 0);
        viewportAaCombo_->addItem(QStringLiteral("2×"), 2);
        viewportAaCombo_->addItem(QStringLiteral("4×"), 4);
        viewportAaCombo_->addItem(QStringLiteral("8×"), 8);
        viewportAaCombo_->setCurrentIndex(viewportAaCombo_->findData(viewportAaSamples));
        viewportAaCombo_->setObjectName(QStringLiteral("viewportAntiAliasingPreference"));
        viewportLayout->addRow(QStringLiteral("Anti-aliasing"), viewportAaCombo_);

        auto *backend = new QLabel(QStringLiteral(
            "OpenGL shader viewport; Qt rendering is used automatically if OpenGL is unavailable."));
        backend->setWordWrap(true);
        viewportLayout->addRow(QStringLiteral("Graphics backend"), backend);
        layout->addWidget(viewportBox);

        auto *hint = new QLabel(QStringLiteral(
            "8× is the saved viewport anti-aliasing setting from your Blender preferences. "
            "If the GPU cannot provide that sample count, the viewport falls back to single-sample rendering."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addStretch(1);
        return page;
    }

    QListWidget *categoryList_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QComboBox *panButtonCombo_ = nullptr;
    QComboBox *dimensionFontCombo_ = nullptr;
    QCheckBox *snapLabelsCheckBox_ = nullptr;
    QCheckBox *smoothCurveDisplayCheckBox_ = nullptr;
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
};

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

class MainWindow final : public QMainWindow {
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("Untitled — Vignola"));
        resize(1440, 900);
        setMinimumSize(980, 620);

        createMenus();
        createWorkspaceBar();
        createLayerPropertiesBar();
        createMainLayout();
        createModelingShortcuts();
        loadPreferences();
        applyTheme();
    }

    bool restoreUpdateSession(const QString &path)
    {
        if (viewport_ == nullptr || !viewport_->restoreUpdateSession(path)) {
            statusBar()->showMessage(QStringLiteral("Update session could not be restored"), 8000);
            return false;
        }

        QFile::remove(path);
        statusBar()->showMessage(QStringLiteral("Update complete — scene restored"), 5000);
        return true;
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (updateRestartInProgress_ || maybeSaveDocument()) {
            event->accept();
        } else {
            event->ignore();
        }
    }

private:
    bool maybeSaveDocument()
    {
        if (!documentModified_) {
            return true;
        }

        const QString documentName = currentProjectPath_.isEmpty()
                                         ? QStringLiteral("Untitled")
                                         : QFileInfo(currentProjectPath_).fileName();
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            QStringLiteral("Unsaved Changes"),
            QStringLiteral("Save changes to %1 before continuing?").arg(documentName),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Save) {
            return saveDocument(false);
        }
        return answer == QMessageBox::Discard;
    }

    QString projectSavePath()
    {
        QSettings settings;
        QString initialPath = currentProjectPath_;
        if (initialPath.isEmpty()) {
            const QString lastDirectory = settings.value(
                QStringLiteral("files/lastProjectDirectory"),
                QDir::homePath()).toString();
            initialPath = QDir(lastDirectory).filePath(QStringLiteral("Untitled.vignola"));
        }

        QString selectedFilter = QFileInfo(initialPath).suffix().compare(
                                     QStringLiteral("blend"), Qt::CaseInsensitive) == 0
                                     ? QStringLiteral("Blender Project (*.blend)")
                                     : QStringLiteral("Vignola Project (*.vignola)");
        QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Save Project"),
            initialPath,
            QStringLiteral("Vignola Project (*.vignola);;Blender Project (*.blend)"),
            &selectedFilter);
        if (path.isEmpty()) {
            return {};
        }

        const QString selectedSuffix = selectedFilter.contains(QStringLiteral("*.blend"),
                                                               Qt::CaseInsensitive)
                                           ? QStringLiteral("blend")
                                           : QStringLiteral("vignola");
        const QFileInfo selectedPath(path);
        if (selectedPath.suffix().isEmpty()) {
            path += QStringLiteral(".") + selectedSuffix;
        } else if (selectedPath.suffix().compare(selectedSuffix, Qt::CaseInsensitive) != 0) {
            path = QDir(selectedPath.absolutePath()).filePath(
                selectedPath.completeBaseName() + QStringLiteral(".") + selectedSuffix);
        }
        return path;
    }

    bool saveDocument(bool saveAs)
    {
        if (viewport_ == nullptr) {
            return false;
        }

        QString path = currentProjectPath_;
        if (saveAs || path.isEmpty()) {
            path = projectSavePath();
            if (path.isEmpty()) {
                return false;
            }
        }
        const QString suffix = QFileInfo(path).suffix();
        if (suffix.compare(QStringLiteral("vignola"), Qt::CaseInsensitive) != 0 &&
            suffix.compare(QStringLiteral("blend"), Qt::CaseInsensitive) != 0) {
            path += QStringLiteral(".vignola");
        }

        QString errorMessage;
        if (!viewport_->saveVignolaDocument(path, &errorMessage)) {
            QMessageBox::critical(this,
                                  QStringLiteral("Could Not Save Project"),
                                  errorMessage.isEmpty()
                                      ? QStringLiteral("The project could not be saved.")
                                      : errorMessage);
            return false;
        }

        currentProjectPath_ = QFileInfo(path).absoluteFilePath();
        documentModified_ = false;
        QSettings settings;
        settings.setValue(QStringLiteral("files/lastProjectDirectory"),
                          QFileInfo(currentProjectPath_).absolutePath());
        updateWindowTitle();
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(currentProjectPath_), 5000);
        return true;
    }

    void openDocument()
    {
        QString initialPath = currentProjectPath_;
        if (initialPath.isEmpty()) {
            QSettings settings;
            initialPath = settings.value(QStringLiteral("files/lastProjectDirectory"),
                                         QDir::homePath()).toString();
        }
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Open Project"),
            initialPath,
            QStringLiteral("Vignola and Blender Projects (*.vignola *.blend)"));
        if (path.isEmpty() || !maybeSaveDocument()) {
            return;
        }

        QString errorMessage;
        suppressDirtyTracking_ = true;
        const bool loaded = viewport_ != nullptr &&
                            viewport_->loadVignolaDocument(path, &errorMessage);
        suppressDirtyTracking_ = false;
        if (!loaded) {
            QMessageBox::critical(this,
                                  QStringLiteral("Could Not Open Project"),
                                  errorMessage.isEmpty()
                                      ? QStringLiteral("The project could not be opened.")
                                      : errorMessage);
            return;
        }

        currentProjectPath_ = QFileInfo(path).absoluteFilePath();
        documentModified_ = false;
        QSettings settings;
        settings.setValue(QStringLiteral("files/lastProjectDirectory"),
                          QFileInfo(currentProjectPath_).absolutePath());
        updateWindowTitle();
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(currentProjectPath_), 5000);
    }

    void importRhino3dm()
    {
        QString initialDirectory;
        if (!currentProjectPath_.isEmpty()) {
            initialDirectory = QFileInfo(currentProjectPath_).absolutePath();
        } else {
            QSettings settings;
            initialDirectory = settings.value(QStringLiteral("files/lastProjectDirectory"),
                                              QDir::homePath()).toString();
        }

        QFileDialog dialog(this,
                           QStringLiteral("Import Rhino 3DM"),
                           initialDirectory,
                           QStringLiteral("Rhino 3D Model (*.3dm)"));
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setFileMode(QFileDialog::ExistingFile);
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setLabelText(QFileDialog::FileName,
                            QStringLiteral("File name or path:"));
        if (dialog.exec() != QDialog::Accepted || viewport_ == nullptr) {
            return;
        }
        const QStringList selectedFiles = dialog.selectedFiles();
        if (selectedFiles.isEmpty() || selectedFiles.first().isEmpty()) {
            return;
        }
        const QString path = selectedFiles.first();

        Rhino3dmImportReport report;
        QString errorMessage;
        if (!viewport_->importRhino3dmDocument(path, &report, &errorMessage)) {
            QMessageBox::critical(this,
                                  QStringLiteral("Could Not Import Rhino Model"),
                                  errorMessage.isEmpty()
                                      ? QStringLiteral("The Rhino model could not be imported.")
                                      : errorMessage);
            return;
        }

        QSettings settings;
        settings.setValue(QStringLiteral("files/lastProjectDirectory"),
                          QFileInfo(path).absolutePath());
        const QString status = QStringLiteral("Imported %1 object(s) from %2")
                                   .arg(report.importedObjectCount)
                                   .arg(QFileInfo(path).fileName());
        statusBar()->showMessage(status, 6000);
        if (!report.warningMessage.isEmpty()) {
            QMessageBox::warning(
                this,
                QStringLiteral("Rhino Import Completed With Skipped Objects"),
                QStringLiteral("Imported %1 object(s). %2")
                    .arg(report.importedObjectCount)
                    .arg(report.warningMessage));
        }
    }

    void newDocument()
    {
        if (viewport_ == nullptr || !maybeSaveDocument()) {
            return;
        }
        suppressDirtyTracking_ = true;
        viewport_->createNewDocument();
        suppressDirtyTracking_ = false;
        currentProjectPath_.clear();
        documentModified_ = false;
        updateWindowTitle();
        statusBar()->showMessage(QStringLiteral("New Vignola project"), 3000);
    }

    void markDocumentModified()
    {
        if (suppressDirtyTracking_ || documentModified_) {
            return;
        }
        documentModified_ = true;
        updateWindowTitle();
    }

    void updateWindowTitle()
    {
        const QString documentName = currentProjectPath_.isEmpty()
                                         ? QStringLiteral("Untitled")
                                         : QFileInfo(currentProjectPath_).completeBaseName();
        setWindowTitle(QStringLiteral("%1%2 — Vignola")
                           .arg(documentName, documentModified_ ? QStringLiteral("*")
                                                               : QString()));
    }

    void updateApplication()
    {
        if (updateProcess_ != nullptr) {
            statusBar()->showMessage(QStringLiteral("Update already in progress"), 3000);
            return;
        }

        const QString executablePath = QCoreApplication::applicationFilePath();
        const QString buildDirectory = QCoreApplication::applicationDirPath();
        const QString sessionPath = QDir(QDir::tempPath()).filePath(
            QStringLiteral("classiCAD-update-%1-%2.json")
                .arg(QCoreApplication::applicationPid())
                .arg(QDateTime::currentMSecsSinceEpoch()));

        if (viewport_ == nullptr || !viewport_->saveUpdateSession(sessionPath)) {
            statusBar()->showMessage(QStringLiteral("Update cancelled — could not save the current scene"),
                                     8000);
            return;
        }

        updateAction_->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Updating classiCAD — rebuilding…"));

        auto *process = new QProcess(this);
        updateProcess_ = process;
        process->setWorkingDirectory(buildDirectory);

        const auto failUpdate = [this, process, sessionPath](const QString &message) {
            if (updateProcess_ != process) {
                return;
            }

            updateProcess_ = nullptr;
            updateAction_->setEnabled(true);
            QFile::remove(sessionPath);
            statusBar()->showMessage(message, 8000);
            process->deleteLater();
        };

        connect(process,
                &QProcess::errorOccurred,
                this,
                [process, failUpdate](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart) {
                        failUpdate(QStringLiteral("Update failed — could not start cmake: %1")
                                       .arg(process->errorString()));
                    }
                });

        connect(process,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this,
                [this,
                 process,
                 sessionPath,
                 executablePath,
                 buildDirectory,
                 failUpdate](int exitCode, QProcess::ExitStatus exitStatus) {
                    if (updateProcess_ != process) {
                        return;
                    }

                    const QString buildOutput =
                        QString::fromLocal8Bit(process->readAllStandardOutput() +
                                                process->readAllStandardError())
                            .trimmed();
                    if (!buildOutput.isEmpty()) {
                        DebugLog::instance().write(QStringLiteral("update build output: %1")
                                                       .arg(buildOutput));
                    }

                    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                        failUpdate(QStringLiteral("Update failed — build exited with code %1")
                                       .arg(exitCode));
                        return;
                    }

                    const QStringList arguments{
                        QStringLiteral("--update-session"),
                        sessionPath};
                    if (!QProcess::startDetached(executablePath,
                                                 arguments,
                                                 buildDirectory)) {
                        failUpdate(QStringLiteral("Update failed — could not restart classiCAD"));
                        return;
                    }

                    updateProcess_ = nullptr;
                    process->deleteLater();
                    statusBar()->showMessage(QStringLiteral("Update complete — restarting classiCAD"));
                    // The scene is already preserved in the temporary JSON
                    // update session. Close this old window without invoking
                    // the normal unsaved-document prompt; the restarted
                    // process will restore that session.
                    updateRestartInProgress_ = true;
                    close();
                });

        process->start(QStringLiteral("cmake"),
                       QStringList{QStringLiteral("--build"),
                                   buildDirectory,
                                   QStringLiteral("--target"),
                                   QStringLiteral("classiCAD")});
    }

    void startSubdivisionWheelMode()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginSubdivision).accepted) {
            statusBar()->showMessage(QStringLiteral("Select a line or curve first"), 4000);
            return;
        }

        statusBar()->showMessage(viewport_->subdivisionStatusText());
    }

    void startJoinMode()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginJoin).accepted) {
            statusBar()->showMessage(QStringLiteral("Could not start Join"), 4000);
            return;
        }

        statusBar()->showMessage(viewport_->joinStatusText());
    }

    void startRotate()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginRotate).accepted) {
            if (rotateToolButton_ != nullptr) {
                rotateToolButton_->setChecked(false);
            }
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            statusBar()->showMessage(QStringLiteral("Select something to rotate first"), 4000);
            return;
        }

        if (rotateToolButton_ != nullptr) {
            rotateToolButton_->setChecked(true);
        }
        statusBar()->showMessage(QStringLiteral("Rotate: click center, start direction, then end direction"));
    }

    void startScale(ScaleMode mode)
    {
        scaleMode_ = mode;
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginScale,
                                       static_cast<int>(mode)).accepted) {
            if (scaleToolButton_ != nullptr) {
                scaleToolButton_->setChecked(false);
            }
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            statusBar()->showMessage(QStringLiteral("Select something to scale first"), 4000);
            return;
        }

        if (scaleToolButton_ != nullptr) {
            scaleToolButton_->setChecked(true);
        }
        statusBar()->showMessage(
            QStringLiteral("%1: click a base point; press Enter to use the selection center")
                .arg(scaleModeName(mode)));
    }

    void createModelingShortcuts()
    {
        if (viewport_ == nullptr) {
            return;
        }

        auto *scaleOneDimensionalShortcut =
            new QShortcut(QKeySequence(QStringLiteral("S,1")), viewport_);
        scaleOneDimensionalShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(scaleOneDimensionalShortcut, &QShortcut::activated, this, [this]() {
            startScale(ScaleMode::OneD);
        });

        auto *scaleTwoDimensionalShortcut =
            new QShortcut(QKeySequence(QStringLiteral("S,2")), viewport_);
        scaleTwoDimensionalShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(scaleTwoDimensionalShortcut, &QShortcut::activated, this, [this]() {
            startScale(ScaleMode::TwoD);
        });

        auto *controlPointsShortcut =
            new QShortcut(QKeySequence(QStringLiteral("C,P")), viewport_);
        controlPointsShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(controlPointsShortcut, &QShortcut::activated, this, [this]() {
            if (controlPointsButton_ != nullptr) {
                controlPointsButton_->toggle();
            }
        });
    }

    void createScaleToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *oneDimensionalAction = menu->addAction(QStringLiteral("Scale 1D"));
        QAction *twoDimensionalAction = menu->addAction(QStringLiteral("Scale 2D"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);
        button->setToolTip(QStringLiteral("Scale — S, then 1 for 1D or 2 for 2D; hold for menu"));

        connect(oneDimensionalAction, &QAction::triggered, this, [this]() {
            startScale(ScaleMode::OneD);
        });
        connect(twoDimensionalAction, &QAction::triggered, this, [this]() {
            startScale(ScaleMode::TwoD);
        });
    }

    void startMirror()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginMirror).accepted) {
            if (mirrorToolButton_ != nullptr) {
                mirrorToolButton_->setChecked(false);
            }
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            statusBar()->showMessage(QStringLiteral("Select something to mirror first"), 4000);
            return;
        }

        if (mirrorToolButton_ != nullptr) {
            mirrorToolButton_->setChecked(true);
        }
        statusBar()->showMessage(QStringLiteral("Mirror: click the first and second points of the axis"));
    }

    void startDuplicate()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginDuplicate).accepted) {
            statusBar()->showMessage(QStringLiteral("Select something to duplicate first"), 4000);
            return;
        }

        statusBar()->showMessage(
            QStringLiteral("Duplicate: click a base point, then click where to place the copy"));
    }

    void duplicateInPlace()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::DuplicateInPlace).accepted) {
            statusBar()->showMessage(QStringLiteral("Select something to duplicate first"), 4000);
            return;
        }

        statusBar()->showMessage(
            QStringLiteral("Copy created in place — press G to move it; X/Y locks the axis, B picks a base point"));
    }

    void explodeSelectedShapes()
    {
        if (viewport_ == nullptr) {
            return;
        }

        viewport_->setTool(Tool::Select);
        const ViewportCommandResult result =
            viewport_->executeCommand(ViewportCommand::Explode);
        if (!result.accepted) {
            statusBar()->showMessage(QStringLiteral("Select a joined spline first"), 4000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Exploded into %1 separate splines")
                                     .arg(result.count),
                                 5000);
    }

    void activateEraseTool()
    {
        if (viewport_ != nullptr) {
            viewport_->setTool(Tool::Erase);
        }
        if (eraseToolButton_ != nullptr) {
            eraseToolButton_->setChecked(true);
        }
        statusBar()->showMessage(QStringLiteral("Erase: drag over geometry, then release"));
    }

    void activateTrimTool()
    {
        if (viewport_ != nullptr) {
            viewport_->setTool(Tool::Trim);
        }
        if (trimToolButton_ != nullptr) {
            trimToolButton_->setChecked(true);
        }
        statusBar()->showMessage(QStringLiteral("Trim: click a selected curve segment"));
    }

    void subdivideWithNumberOfPoints()
    {
        if (viewport_ == nullptr ||
            !viewport_->executeCommand(ViewportCommand::BeginSubdivision).accepted) {
            statusBar()->showMessage(QStringLiteral("Select a line or curve first"), 4000);
            return;
        }

        const QString title = QStringLiteral("Subdivide Curve");
        const QString label = QStringLiteral("Number of sections:");
        bool accepted = false;
        const int sections = QInputDialog::getInt(this,
                                                  title,
                                                  label,
                                                  2,
                                                  2,
                                                  10000,
                                                  1,
                                                  &accepted);
        if (!accepted) {
            viewport_->executeCommand(ViewportCommand::CancelSubdivision);
            return;
        }

        if (!viewport_->executeCommand(ViewportCommand::ApplySubdivision, sections).accepted) {
            statusBar()->showMessage(QStringLiteral("Could not subdivide the selected curve"), 5000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Subdivided into %1 sections (endpoints marked)")
                                     .arg(sections),
                                 5000);
    }

    void createMenus()
    {
        QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
        QAction *newDocumentAction = fileMenu->addAction(QStringLiteral("New Document"));
        newDocumentAction->setShortcut(QKeySequence::New);
        connect(newDocumentAction, &QAction::triggered, this, [this]() {
            newDocument();
        });
        QAction *openDocumentAction = fileMenu->addAction(QStringLiteral("Open…"));
        openDocumentAction->setShortcut(QKeySequence::Open);
        connect(openDocumentAction, &QAction::triggered, this, [this]() {
            openDocument();
        });
        QMenu *importMenu = fileMenu->addMenu(QStringLiteral("Import"));
        QAction *importRhinoAction = importMenu->addAction(
            QStringLiteral("Rhino 3DM…"));
        connect(importRhinoAction, &QAction::triggered, this, [this]() {
            importRhino3dm();
        });
        fileMenu->addSeparator();
        QAction *saveDocumentAction = fileMenu->addAction(QStringLiteral("Save"));
        saveDocumentAction->setShortcut(QKeySequence::Save);
        connect(saveDocumentAction, &QAction::triggered, this, [this]() {
            saveDocument(false);
        });
        QAction *saveAsDocumentAction = fileMenu->addAction(QStringLiteral("Save As…"));
        saveAsDocumentAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
        connect(saveAsDocumentAction, &QAction::triggered, this, [this]() {
            saveDocument(true);
        });
        fileMenu->addSeparator();
        fileMenu->addAction(QStringLiteral("Quit"), this, &QWidget::close);

        QMenu *editMenu = menuBar()->addMenu(QStringLiteral("Edit"));
        undoAction_ = editMenu->addAction(QStringLiteral("Undo"));
        undoAction_->setShortcut(QKeySequence::Undo);
        undoAction_->setEnabled(false);
        redoAction_ = editMenu->addAction(QStringLiteral("Redo"));
        redoAction_->setShortcut(QKeySequence::Redo);
        redoAction_->setEnabled(false);
        connect(undoAction_, &QAction::triggered, this, [this]() {
            viewport_->executeCommand(ViewportCommand::Undo);
            statusBar()->showMessage(QStringLiteral("Undo"));
        });
        connect(redoAction_, &QAction::triggered, this, [this]() {
            viewport_->executeCommand(ViewportCommand::Redo);
            statusBar()->showMessage(QStringLiteral("Redo"));
        });

        editMenu->addSeparator();
        subdivideAction_ = editMenu->addAction(QStringLiteral("Subdivide Selected (Wheel)"));
        subdivideAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
        subdivideAction_->setShortcutContext(Qt::WindowShortcut);
        connect(subdivideAction_, &QAction::triggered, this, [this]() {
            startSubdivisionWheelMode();
        });

        joinAction_ = editMenu->addAction(QStringLiteral("Join Splines"));
        joinAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
        joinAction_->setShortcutContext(Qt::WindowShortcut);
        connect(joinAction_, &QAction::triggered, this, [this]() {
            startJoinMode();
        });

        QAction *explodeAction = editMenu->addAction(QStringLiteral("Explode Splines"));
        explodeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+J")));
        explodeAction->setShortcutContext(Qt::WindowShortcut);
        connect(explodeAction, &QAction::triggered, this, [this]() {
            explodeSelectedShapes();
        });

        QAction *rotateAction = editMenu->addAction(QStringLiteral("Rotate"));
        rotateAction->setShortcut(QKeySequence(Qt::Key_R));
        rotateAction->setShortcutContext(Qt::WindowShortcut);
        connect(rotateAction, &QAction::triggered, this, [this]() {
            startRotate();
        });

        QAction *mirrorAction = editMenu->addAction(QStringLiteral("Mirror"));
        mirrorAction->setShortcut(QKeySequence(Qt::Key_M));
        mirrorAction->setShortcutContext(Qt::WindowShortcut);
        connect(mirrorAction, &QAction::triggered, this, [this]() {
            startMirror();
        });

        QAction *duplicateAction = editMenu->addAction(QStringLiteral("Duplicate in Place"));
        duplicateAction->setShortcut(QKeySequence(QStringLiteral("Shift+D")));
        duplicateAction->setShortcutContext(Qt::WindowShortcut);
        connect(duplicateAction, &QAction::triggered, this, [this]() {
            duplicateInPlace();
        });

        eraseAction_ = editMenu->addAction(QStringLiteral("Erase"));
        eraseAction_->setShortcut(QKeySequence(Qt::Key_E));
        eraseAction_->setShortcutContext(Qt::WindowShortcut);
        connect(eraseAction_, &QAction::triggered, this, [this]() {
            activateEraseTool();
        });

        trimAction_ = editMenu->addAction(QStringLiteral("Trim"));
        trimAction_->setShortcut(QKeySequence(Qt::Key_T));
        trimAction_->setShortcutContext(Qt::WindowShortcut);
        connect(trimAction_, &QAction::triggered, this, [this]() {
            activateTrimTool();
        });

        editMenu->addSeparator();
        QAction *preferencesAction = editMenu->addAction(QStringLiteral("Preferences…"));
        connect(preferencesAction, &QAction::triggered, this, [this]() {
            openPreferences();
        });

        QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("View"));
        viewMenu->addAction(QStringLiteral("Frame All"));
        viewMenu->addAction(QStringLiteral("Toggle Grid"));
        QAction *documentGridSettingsAction =
            viewMenu->addAction(QStringLiteral("Grid Units and Spacing…"));
        connect(documentGridSettingsAction, &QAction::triggered,
                this, [this]() { openDocumentGridSettings(); });
        QMenu *drawingPlaneMenu = viewMenu->addMenu(QStringLiteral("Drawing Plane"));
        workPlaneActionGroup_ = new QActionGroup(this);
        workPlaneActionGroup_->setExclusive(true);
        const auto addWorkPlaneAction = [this, drawingPlaneMenu](
                                            const QString &label,
                                            WorkPlane plane) {
            QAction *action = drawingPlaneMenu->addAction(label);
            action->setCheckable(true);
            action->setData(static_cast<int>(plane));
            workPlaneActionGroup_->addAction(action);
            connect(action, &QAction::triggered, this, [this, plane]() {
                if (viewport_ != nullptr) {
                    viewport_->setWorkPlane(plane, viewport_->workPlaneOffset());
                }
            });
        };
        addWorkPlaneAction(QStringLiteral("XY"), WorkPlane::XY);
        addWorkPlaneAction(QStringLiteral("XZ"), WorkPlane::XZ);
        addWorkPlaneAction(QStringLiteral("YZ"), WorkPlane::YZ);

        menuBar()->addMenu(QStringLiteral("Help"));
    }

    void updateHistoryActions()
    {
        if (undoAction_ != nullptr && viewport_ != nullptr) {
            undoAction_->setEnabled(viewport_->canUndo());
        }
        if (redoAction_ != nullptr && viewport_ != nullptr) {
            redoAction_->setEnabled(viewport_->canRedo());
        }
    }

    void createWorkspaceBar()
    {
        auto *bar = new QToolBar(QStringLiteral("Workspace"), this);
        bar->setObjectName(QStringLiteral("workspaceBar"));
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setToolButtonStyle(Qt::ToolButtonTextOnly);

        QLabel *brand = new QLabel(QStringLiteral("classiCAD"));
        brand->setObjectName(QStringLiteral("brand"));
        bar->addWidget(brand);
        bar->addSeparator();

        for (const QString &name : {QStringLiteral("Modeling"),
                                     QStringLiteral("Sketching"),
                                     QStringLiteral("Layout")}) {
            QToolButton *workspaceButton = new QToolButton;
            workspaceButton->setText(name);
            workspaceButton->setCheckable(true);
            workspaceButton->setAutoExclusive(true);
            workspaceButton->setObjectName(QStringLiteral("workspaceButton"));
            bar->addWidget(workspaceButton);
            if (name == QStringLiteral("Modeling")) {
                workspaceButton->setChecked(true);
            }
        }

        bar->addSeparator();
        updateAction_ = new QAction(QStringLiteral("Update"), this);
        updateAction_->setToolTip(QStringLiteral("Rebuild and restart classiCAD, preserving the current scene"));
        connect(updateAction_, &QAction::triggered, this, [this]() {
            updateApplication();
        });
        bar->addAction(updateAction_);

        addToolBar(Qt::TopToolBarArea, bar);
    }

    void createLayerPropertiesBar()
    {
        addToolBarBreak(Qt::TopToolBarArea);
        auto *bar = new QToolBar(QStringLiteral("Layer Properties"), this);
        bar->setObjectName(QStringLiteral("layerPropertiesBar"));
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setToolButtonStyle(Qt::ToolButtonTextOnly);

        currentLayerCombo_ = new QComboBox;
        currentLayerCombo_->setObjectName(QStringLiteral("currentLayerCombo"));
        currentLayerCombo_->setMinimumWidth(150);
        currentLayerCombo_->setMaximumWidth(190);
        currentLayerCombo_->setToolTip(QStringLiteral("Current drawing layer"));
        bar->addWidget(currentLayerCombo_);
        bar->addSeparator();

        layerColorCombo_ = new QComboBox;
        layerColorCombo_->setObjectName(QStringLiteral("layerColorCombo"));
        layerColorCombo_->setMinimumWidth(110);
        layerColorCombo_->setToolTip(
            QStringLiteral("Active layer color used by ByLayer geometry"));
        const auto addColorOption = [this](const QString &name, const QColor &color) {
            QPixmap swatch(12, 12);
            swatch.fill(color);
            layerColorCombo_->addItem(QIcon(swatch), name, color);
        };
        addColorOption(QStringLiteral("ByLayer"), QColor(QStringLiteral("#d28b45")));
        addColorOption(QStringLiteral("Red"), QColor(QStringLiteral("#ff3030")));
        addColorOption(QStringLiteral("Yellow"), QColor(QStringLiteral("#f0d030")));
        addColorOption(QStringLiteral("Green"), QColor(QStringLiteral("#40c060")));
        addColorOption(QStringLiteral("Cyan"), QColor(QStringLiteral("#40c8d8")));
        addColorOption(QStringLiteral("Blue"), QColor(QStringLiteral("#4d83e6")));
        addColorOption(QStringLiteral("Magenta"), QColor(QStringLiteral("#d45adc")));
        addColorOption(QStringLiteral("White"), QColor(QStringLiteral("#ffffff")));
        layerColorCombo_->addItem(QStringLiteral("Custom…"), QColor());
        layerColorCombo_->setItemData(layerColorCombo_->count() - 1,
                                      true,
                                      Qt::UserRole + 1);
        bar->addWidget(layerColorCombo_);

        layerLineTypeCombo_ = new QComboBox;
        layerLineTypeCombo_->setObjectName(QStringLiteral("layerLineTypeCombo"));
        layerLineTypeCombo_->setMinimumWidth(125);
        layerLineTypeCombo_->setToolTip(
            QStringLiteral("Active layer linetype used by ByLayer geometry"));
        populateLayerLineTypeCombo(layerLineTypeCombo_, true);
        bar->addWidget(layerLineTypeCombo_);

        layerLineWeightCombo_ = new QComboBox;
        layerLineWeightCombo_->setObjectName(QStringLiteral("layerLineWeightCombo"));
        layerLineWeightCombo_->setMinimumWidth(125);
        layerLineWeightCombo_->setToolTip(
            QStringLiteral("Active layer lineweight used by ByLayer geometry"));
        layerLineWeightCombo_->addItem(QStringLiteral("ByLayer"), 0.0);
        for (const auto &lineWeight : QList<QPair<QString, qreal>>{
                 {QStringLiteral("0.13 mm"), 0.13},
                 {QStringLiteral("0.18 mm"), 0.18},
                 {QStringLiteral("0.25 mm"), 0.25},
                 {QStringLiteral("0.35 mm"), 0.35},
                 {QStringLiteral("0.50 mm"), 0.50},
                 {QStringLiteral("0.70 mm"), 0.70},
                 {QStringLiteral("1.00 mm"), 1.00},
                 {QStringLiteral("1.40 mm"), 1.40},
                 {QStringLiteral("2.00 mm"), 2.00},
                 {QStringLiteral("2.11 mm"), 2.11}}) {
            layerLineWeightCombo_->addItem(lineWeight.first, lineWeight.second);
        }
        bar->addWidget(layerLineWeightCombo_);

        connect(currentLayerCombo_, qOverload<int>(&QComboBox::activated),
                this, [this](int index) {
                    if (index < 0 || viewport_ == nullptr) {
                        return;
                    }
                    const LayerId layerId = LayerId::fromValue(
                        currentLayerCombo_->itemData(index).toULongLong());
                    QTimer::singleShot(0, this, [this, layerId]() {
                        ViewportLayerCommandRequest request;
                        request.command = ViewportLayerCommand::Activate;
                        request.layerId = layerId;
                        executeLayerCommand(request, QString(),
                                            QStringLiteral("Could not change current layer"));
                    });
                });
        connect(layerColorCombo_, qOverload<int>(&QComboBox::activated),
                this, [this](int index) {
                    if (index < 0 || viewport_ == nullptr) {
                        return;
                    }
                    const LayerId layerId = currentLayerId();
                    if (!layerId.isValid()) {
                        return;
                    }
                    QColor color = layerColorCombo_->itemData(index).value<QColor>();
                    const bool chooseCustom =
                        layerColorCombo_->itemData(index, Qt::UserRole + 1).toBool();
                    QTimer::singleShot(0, this, [this, layerId, color, chooseCustom]() {
                        QColor selectedColor = color;
                        if (chooseCustom) {
                            QColor currentColor;
                            for (const ViewportLayerInfo &info : viewport_->layerInfos()) {
                                if (info.id == layerId) {
                                    currentColor = info.color;
                                    break;
                                }
                            }
                            selectedColor = QColorDialog::getColor(
                                currentColor, this, QStringLiteral("Layer Color"));
                        }
                        if (selectedColor.isValid()) {
                            setLayerColor(layerId, selectedColor);
                        }
                    });
                });
        connect(layerLineTypeCombo_, qOverload<int>(&QComboBox::activated),
                this, [this](int index) {
                    if (index >= 0) {
                        const LayerId layerId = currentLayerId();
                        const QString lineType =
                            layerLineTypeCombo_->itemData(index).toString();
                        QTimer::singleShot(0, this, [this, layerId, lineType]() {
                            setLayerLineType(layerId, lineType);
                        });
                    }
                });
        connect(layerLineWeightCombo_, qOverload<int>(&QComboBox::activated),
                this, [this](int index) {
                    if (index >= 0) {
                        const LayerId layerId = currentLayerId();
                        const qreal lineWeightMm =
                            layerLineWeightCombo_->itemData(index).toDouble();
                        QTimer::singleShot(0, this, [this, layerId, lineWeightMm]() {
                            setLayerLineWeight(layerId, lineWeightMm);
                        });
                    }
                });

        addToolBar(Qt::TopToolBarArea, bar);
    }

    void createMainLayout()
    {
        auto *root = new QWidget;
        auto *rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        rootLayout->addWidget(createToolShelf());

        auto *workspaceSplitter = new QSplitter(Qt::Horizontal, root);
        workspaceSplitter->setObjectName(QStringLiteral("workspaceSplitter"));
        workspaceSplitter->setHandleWidth(6);
        workspaceSplitter->setChildrenCollapsible(false);

        viewport_ = createViewportWidget(workspaceSplitter);
        ViewportUiCallbacks viewportCallbacks;
        viewportCallbacks.commandFinished = [this](ToolId tool) {
            if (tool == Tool::Select && selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
                statusBar()->showMessage(QStringLiteral("Select mode"));
            }
        };
        viewportCallbacks.toolRepeated = [this](ToolId tool) {
            if (tool == Tool::Scale && scaleToolButton_ != nullptr) {
                scaleToolButton_->setChecked(true);
            }
            if (isDimensionTool(tool) && dimensionToolButton_ != nullptr) {
                dimensionToolButton_->setChecked(true);
            }
            if ((tool == Tool::TangentFromCurve ||
                 tool == Tool::PerpendicularFromCurve) &&
                lineToolButton_ != nullptr) {
                lineToolButton_->setChecked(true);
            }
            for (QToolButton *button : toolButtons_) {
                if (button->toolTip() == toolName(tool)) {
                    button->setChecked(true);
                    break;
                }
            }
            statusBar()->showMessage(QStringLiteral("Repeated tool: %1").arg(toolName(tool)));
        };
        workspaceSplitter->addWidget(viewport_);
        workspaceSplitter->addWidget(createRightPanel());
        workspaceSplitter->setStretchFactor(0, 1);
        workspaceSplitter->setStretchFactor(1, 0);
        rootLayout->addWidget(workspaceSplitter, 1);
        setCentralWidget(root);
        workspaceSplitter->setSizes({800, 540});
        createOsnapLane();

        coordinateLabel_ = new QLabel(QStringLiteral("X 0.00   Y 0.00   Zoom 100%"));
        statusBar()->addWidget(coordinateLabel_);

        orthoAction_ = new QAction(QStringLiteral("Ortho"), this);
        orthoAction_->setCheckable(true);
        orthoAction_->setShortcut(QKeySequence(Qt::Key_F8));
        orthoAction_->setShortcutContext(Qt::WindowShortcut);
        addAction(orthoAction_);

        auto *orthoButton = new QToolButton;
        orthoButton->setObjectName(QStringLiteral("statusToggle"));
        orthoButton->setDefaultAction(orthoAction_);
        statusBar()->addPermanentWidget(orthoButton);

        osnapAction_ = new QAction(QStringLiteral("OSnap"), this);
        osnapAction_->setCheckable(true);
        osnapAction_->setShortcut(QKeySequence(Qt::Key_F3));
        osnapAction_->setShortcutContext(Qt::WindowShortcut);
        addAction(osnapAction_);

        auto *osnapButton = new QToolButton;
        osnapButton->setObjectName(QStringLiteral("statusToggle"));
        osnapButton->setDefaultAction(osnapAction_);
        statusBar()->addPermanentWidget(osnapButton);
        statusBar()->addPermanentWidget(new QLabel(QStringLiteral("Ready")));

        connect(orthoAction_, &QAction::toggled, this, [this](bool enabled) {
            viewport_->setOrthoEnabled(enabled);
            QSettings settings;
            settings.setValue(QStringLiteral("modeling/orthoEnabled"), enabled);
            settings.sync();
            statusBar()->showMessage(enabled ? QStringLiteral("Ortho: On")
                                             : QStringLiteral("Ortho: Off"));
        });

        connect(osnapAction_, &QAction::toggled, this, [this](bool enabled) {
            osnapLane_->setVisible(enabled);
            viewport_->setOsnapEnabled(enabled);
            QSettings settings;
            settings.setValue(QStringLiteral("osnap/enabled"), enabled);
            settings.sync();
            statusBar()->showMessage(enabled ? QStringLiteral("OSnap: On")
                                             : QStringLiteral("OSnap: Off"));
        });

        viewportCallbacks.coordinateUpdate = [this](const QString &text) {
            coordinateLabel_->setText(text);
        };
        viewportCallbacks.toolStatusUpdate = [this](const QString &message) {
            statusBar()->showMessage(message);
        };
        viewportCallbacks.historyChanged = [this]() {
            updateHistoryActions();
            markDocumentModified();
        };
        viewportCallbacks.layersChanged = [this]() {
            refreshLayers();
            markDocumentModified();
        };
        viewportCallbacks.subdivisionStatusUpdate = [this](const QString &message) {
            if (message.isEmpty()) {
                statusBar()->clearMessage();
            } else {
                statusBar()->showMessage(message);
            }
        };
        viewportCallbacks.joinStatusUpdate = [this](const QString &message) {
            if (message.isEmpty()) {
                statusBar()->clearMessage();
            } else {
                statusBar()->showMessage(message);
            }
        };
        viewportCallbacks.viewStateUpdate = [this](WorkPlane plane,
                                                   qreal,
                                                   ViewportViewPreset) {
            if (workPlaneActionGroup_ != nullptr) {
                for (QAction *action : workPlaneActionGroup_->actions()) {
                    action->setChecked(action->data().toInt() ==
                                       static_cast<int>(plane));
                }
            }
        };
        viewport_->setUiCallbacks(viewportCallbacks);
        viewportCallbacks.viewStateUpdate(viewport_->workPlane(),
                                          viewport_->workPlaneOffset(),
                                          viewport_->viewPreset());
        refreshLayers();
        updateHistoryActions();
    }

    void createOsnapLane()
    {
        osnapLane_ = new QToolBar(QStringLiteral("Object Snaps"), this);
        osnapLane_->setObjectName(QStringLiteral("osnapLane"));
        osnapLane_->setMovable(false);
        osnapLane_->setFloatable(false);
        osnapLane_->setToolButtonStyle(Qt::ToolButtonTextOnly);

        QLabel *label = new QLabel(QStringLiteral("OSNAP"));
        label->setObjectName(QStringLiteral("osnapLaneLabel"));
        osnapLane_->addWidget(label);
        osnapLane_->addSeparator();

        endpointSnapCheckBox_ = new QCheckBox(QStringLiteral("Endpoint"));
        midpointSnapCheckBox_ = new QCheckBox(QStringLiteral("Midpoint"));
        intersectionSnapCheckBox_ = new QCheckBox(QStringLiteral("Intersection"));
        centerSnapCheckBox_ = new QCheckBox(QStringLiteral("Center"));
        perpendicularSnapCheckBox_ = new QCheckBox(QStringLiteral("Perpendicular"));
        tangentSnapCheckBox_ = new QCheckBox(QStringLiteral("Tangent"));
        nearSnapCheckBox_ = new QCheckBox(QStringLiteral("Near"));
        controlPointSnapCheckBox_ = new QCheckBox(QStringLiteral("Control Points"));

        for (QCheckBox *checkBox : {endpointSnapCheckBox_,
                                    midpointSnapCheckBox_,
                                    intersectionSnapCheckBox_,
                                    centerSnapCheckBox_,
                                    perpendicularSnapCheckBox_,
                                    tangentSnapCheckBox_,
                                    nearSnapCheckBox_,
                                    controlPointSnapCheckBox_}) {
            checkBox->setObjectName(QStringLiteral("osnapCheckBox"));
            checkBox->setChecked(true);
            osnapLane_->addWidget(checkBox);
        }
        perpendicularSnapCheckBox_->setChecked(false);
        tangentSnapCheckBox_->setChecked(false);
        nearSnapCheckBox_->setChecked(false);
        controlPointSnapCheckBox_->setChecked(false);

        const auto syncSnapModes = [this]() {
            viewport_->setSnapModes(endpointSnapCheckBox_->isChecked(),
                                    midpointSnapCheckBox_->isChecked(),
                                    intersectionSnapCheckBox_->isChecked(),
                                    centerSnapCheckBox_->isChecked(),
                                    perpendicularSnapCheckBox_->isChecked(),
                                    tangentSnapCheckBox_->isChecked(),
                                    nearSnapCheckBox_->isChecked(),
                                    controlPointSnapCheckBox_->isChecked());
            QSettings settings;
            settings.setValue(QStringLiteral("osnap/endpoint"), endpointSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/midpoint"), midpointSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/intersection"),
                              intersectionSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/center"), centerSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/perpendicular"),
                              perpendicularSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/tangent"), tangentSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/near"), nearSnapCheckBox_->isChecked());
            settings.setValue(QStringLiteral("osnap/controlPoint"),
                              controlPointSnapCheckBox_->isChecked());
            settings.sync();
        };

        connect(endpointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(midpointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(intersectionSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(centerSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(perpendicularSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(tangentSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(nearSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);
        connect(controlPointSnapCheckBox_, &QCheckBox::toggled, this, syncSnapModes);

        addToolBar(Qt::BottomToolBarArea, osnapLane_);
        osnapLane_->setVisible(false);
    }

    QWidget *createToolShelf()
    {
        auto *scrollArea = new ToolShelfScrollArea;
        auto *shelf = new QFrame;
        shelf->setObjectName(QStringLiteral("toolShelf"));
        shelf->setMinimumWidth(82);

        auto *layout = new QVBoxLayout(shelf);
        layout->setSizeConstraint(QLayout::SetMinimumSize);
        layout->setContentsMargins(7, 10, 7, 10);
        layout->setSpacing(6);

        QLabel *label = new QLabel(QStringLiteral("TOOLS"));
        label->setObjectName(QStringLiteral("shelfLabel"));
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);

        auto *group = new QButtonGroup(shelf);
        group->setExclusive(true);

        selectToolButton_ = addToolButton(layout, group, QStringLiteral("↖\nSelect"), Tool::Select, true);
        pointToolButton_ = addToolButton(layout, group, QStringLiteral("•\nPoint"), Tool::Point);
        createPointToolMenu(pointToolButton_);
        lineToolButton_ = addToolButton(layout, group, QStringLiteral("╱\nLine"), Tool::Line);
        createLineToolMenu(lineToolButton_);
        arcToolButton_ = addToolButton(layout, group, QStringLiteral("⌒\nArc"), Tool::Arc);
        createArcToolMenu(arcToolButton_);
        bezierToolButton_ = addToolButton(layout, group, QStringLiteral("∿\nBezier"), Tool::Bezier);
        createCurveToolMenu(bezierToolButton_);
        addToolButton(layout, group, QStringLiteral("N\nNURBS"), Tool::Nurbs);
        rectangleToolButton_ = addToolButton(layout,
                                             group,
                                             QStringLiteral("□\nRect"),
                                             Tool::Rectangle);
        createRectangleToolMenu(rectangleToolButton_);
        polygonToolButton_ = addToolButton(layout,
                                           group,
                                           QStringLiteral("⬡\nPolygon"),
                                           Tool::PolygonCenterCorner);
        createPolygonToolMenu(polygonToolButton_);
        circleToolButton_ = addToolButton(layout,
                                         group,
                                         QStringLiteral("○\nCircle"),
                                         Tool::Circle);
        createCircleToolMenu(circleToolButton_);
        ellipseToolButton_ = addToolButton(layout,
                                           group,
                                           QStringLiteral("⬭\nEllipse"),
                                           Tool::Ellipse);
        createEllipseToolMenu(ellipseToolButton_);
        addToolButton(layout,
                      group,
                      QStringLiteral("▧\nPicture"),
                      Tool::Picture);
        dimensionToolButton_ = addToolButton(layout,
                                             group,
                                             QStringLiteral("↔\nDim"),
                                             Tool::LinearDimension);
        dimensionToolButton_->setToolTip(QStringLiteral("Dimensions"));
        createDimensionToolMenu(dimensionToolButton_);
        eraseToolButton_ = addToolButton(layout, group, QStringLiteral("Erase"), Tool::Erase);
        eraseToolButton_->setIcon(makeEraserIcon());
        eraseToolButton_->setIconSize(QSize(24, 24));
        eraseToolButton_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        trimToolButton_ = addToolButton(layout, group, QStringLiteral("Trim"), Tool::Trim);
        trimToolButton_->setIcon(makeTrimIcon());
        trimToolButton_->setIconSize(QSize(24, 24));
        trimToolButton_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        rotateToolButton_ = addToolButton(layout, group, QStringLiteral("↻\nRotate"), Tool::Rotate);
        mirrorToolButton_ = addToolButton(layout, group, QStringLiteral("⇄\nMirror"), Tool::Mirror);
        scaleToolButton_ = addToolButton(layout, group, QStringLiteral("⤢\nScale"), Tool::Scale);
        createScaleToolMenu(scaleToolButton_);

        auto *duplicateToolButton = new QToolButton;
        duplicateToolButton->setObjectName(QStringLiteral("toolButton"));
        duplicateToolButton->setText(QStringLiteral("⧉\nDuplicate"));
        duplicateToolButton->setToolTip(
            QStringLiteral("Interactive Duplicate: choose a base point and placement"));
        duplicateToolButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(duplicateToolButton);
        connect(duplicateToolButton, &QToolButton::clicked, this, [this]() {
            startDuplicate();
        });

        layout->addSpacing(8);
        controlPointsButton_ = new QToolButton;
        controlPointsButton_->setObjectName(QStringLiteral("toolButton"));
        controlPointsButton_->setText(QStringLiteral("CP\nPoints"));
        controlPointsButton_->setToolTip(QStringLiteral("Control Points — C, then P to toggle"));
        controlPointsButton_->setCheckable(true);
        controlPointsButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(controlPointsButton_);
        connect(controlPointsButton_, &QToolButton::toggled, this, [this](bool visible) {
            viewport_->setControlPointsVisible(visible);
            QSettings settings;
            settings.setValue(QStringLiteral("view/controlPoints"), visible);
            settings.sync();
            statusBar()->showMessage(visible ? QStringLiteral("Control points: On")
                                             : QStringLiteral("Control points: Off"));
        });

        subdivideButton_ = new QToolButton;
        subdivideButton_->setObjectName(QStringLiteral("toolButton"));
        subdivideButton_->setText(QStringLiteral("Subdiv\nPoints"));
        subdivideButton_->setToolTip(QStringLiteral("Add evenly spaced subdivision points to the selected line or curve"));
        subdivideButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(subdivideButton_);
        connect(subdivideButton_, &QToolButton::clicked, this, [this]() {
            subdivideWithNumberOfPoints();
        });

        joinButton_ = new QToolButton;
        joinButton_->setObjectName(QStringLiteral("toolButton"));
        joinButton_->setText(QStringLiteral("Join\nSplines"));
        joinButton_->setToolTip(QStringLiteral("Join connected lines and curves into a component-preserving PolyCurve"));
        joinButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(joinButton_);
        connect(joinButton_, &QToolButton::clicked, this, [this]() {
            startJoinMode();
        });

        explodeButton_ = new QToolButton;
        explodeButton_->setObjectName(QStringLiteral("toolButton"));
        explodeButton_->setText(QStringLiteral("Explode\nSplines"));
        explodeButton_->setToolTip(QStringLiteral("Separate selected joined splines into individual curves"));
        explodeButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(explodeButton_);
        connect(explodeButton_, &QToolButton::clicked, this, [this]() {
            explodeSelectedShapes();
        });

        layout->addStretch(1);

        toolHelp_ = new QLabel;
        toolHelp_->setObjectName(QStringLiteral("toolHelp"));
        toolHelp_->setAlignment(Qt::AlignCenter);
        layout->addWidget(toolHelp_);
        updateToolHelp();

        layout->activate();
        shelf->adjustSize();
        scrollArea->setShelfWidget(shelf);
        return scrollArea;
    }

    QToolButton *addToolButton(QVBoxLayout *layout,
                               QButtonGroup *group,
                               const QString &text,
                               ToolId tool,
                               bool checked = false)
    {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("toolButton"));
        button->setText(text);
        button->setCheckable(true);
        button->setChecked(checked);
        button->setToolTip(toolName(tool));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        group->addButton(button);
        layout->addWidget(button);
        toolButtons_.append(button);

        connect(button, &QToolButton::clicked, this, [this, tool]() {
            if (tool == Tool::Picture) {
                startPicturePlacement();
                return;
            }
            if (tool == Tool::Rotate) {
                startRotate();
                return;
            }
            if (tool == Tool::Mirror) {
                startMirror();
                return;
            }
            if (tool == Tool::Scale) {
                startScale(scaleMode_);
                return;
            }
            if (tool == Tool::Arc) {
                // A normal click always returns to the default arc command.
                viewport_->setArcMode(ArcMode::OnePoint);
            }
            viewport_->setTool(tool);
            statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
        });

        return button;
    }

    void startPicturePlacement()
    {
        if (viewport_ == nullptr) {
            return;
        }

        QSettings settings;
        QString initialDirectory = settings.value(
            QStringLiteral("files/lastPictureDirectory"),
            QDir::homePath()).toString();
        if (!currentProjectPath_.isEmpty()) {
            initialDirectory = QFileInfo(currentProjectPath_).absolutePath();
        }

        QFileDialog dialog(this,
                           QStringLiteral("Place Picture"),
                           initialDirectory,
                           QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp);;All files (*)"));
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setFileMode(QFileDialog::ExistingFile);
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setLabelText(QFileDialog::FileName,
                            QStringLiteral("Image file or path:"));
        if (dialog.exec() != QDialog::Accepted) {
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            return;
        }

        const QStringList selectedFiles = dialog.selectedFiles();
        if (selectedFiles.isEmpty() || selectedFiles.first().isEmpty()) {
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            return;
        }

        const QString imagePath = selectedFiles.first();
        QString errorMessage;
        if (!viewport_->beginPicturePlacement(imagePath, &errorMessage)) {
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            QMessageBox::critical(this,
                                  QStringLiteral("Could Not Open Picture"),
                                  errorMessage.isEmpty()
                                      ? QStringLiteral("The selected image could not be opened.")
                                      : errorMessage);
            return;
        }

        settings.setValue(QStringLiteral("files/lastPictureDirectory"),
                          QFileInfo(imagePath).absolutePath());
        statusBar()->showMessage(
            QStringLiteral("Picture: click the first corner, then the opposite corner"));
    }

    void activateArcMode(ArcMode mode)
    {
        viewport_->setArcMode(mode);
        viewport_->setTool(Tool::Arc);
        if (arcToolButton_ != nullptr) {
            arcToolButton_->setChecked(true);
        }
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(arcModeName(mode)));
    }

    void activateLineTool(ToolId tool)
    {
        if (lineToolButton_ != nullptr) {
            lineToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
    }

    void activatePointTool(ToolId tool)
    {
        if (pointToolButton_ != nullptr) {
            pointToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
    }

    void activateCurveTool(ToolId tool)
    {
        if (bezierToolButton_ != nullptr) {
            bezierToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
    }

    void createPointToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }
        auto *menu = new QMenu(button);
        QAction *pointAction = menu->addAction(QStringLiteral("Point"));
        QAction *pointByLineAction = menu->addAction(QStringLiteral("Point by Line"));
        QAction *pointByArcsAction = menu->addAction(QStringLiteral("Point by Arcs"));
        QAction *pointCenterAction = menu->addAction(QStringLiteral("Point Center"));
        QAction *edgeCenterAction = menu->addAction(QStringLiteral("Edge Center"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);
        connect(pointAction, &QAction::triggered, this, [this]() {
            activatePointTool(Tool::Point);
        });
        connect(pointByLineAction, &QAction::triggered, this, [this]() {
            activatePointTool(Tool::PointByLine);
        });
        connect(pointByArcsAction, &QAction::triggered, this, [this]() {
            activatePointTool(Tool::PointByArcs);
        });
        connect(pointCenterAction, &QAction::triggered, this, [this]() {
            activatePointTool(Tool::PointCenter);
        });
        connect(edgeCenterAction, &QAction::triggered, this, [this]() {
            activatePointTool(Tool::PointEdgeCenter);
        });
    }

    void createCurveToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }
        auto *menu = new QMenu(button);
        QAction *bezierAction = menu->addAction(QStringLiteral("Bezier"));
        QAction *nurbsAction = menu->addAction(QStringLiteral("NURBS"));
        QAction *interpolateAction = menu->addAction(QStringLiteral("Interpolate Curve"));
        QAction *freehandAction = menu->addAction(QStringLiteral("Freehand Curve"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);
        connect(bezierAction, &QAction::triggered, this, [this]() {
            activateCurveTool(Tool::Bezier);
        });
        connect(nurbsAction, &QAction::triggered, this, [this]() {
            activateCurveTool(Tool::Nurbs);
        });
        connect(interpolateAction, &QAction::triggered, this, [this]() {
            activateCurveTool(Tool::CurveInterpolate);
        });
        connect(freehandAction, &QAction::triggered, this, [this]() {
            activateCurveTool(Tool::CurveFreehand);
        });
    }

    void activateDimensionTool(ToolId tool)
    {
        if (dimensionToolButton_ != nullptr) {
            dimensionToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
    }

    void createDimensionToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *linearAction = menu->addAction(QStringLiteral("Linear Dimension"));
        QAction *angularAction = menu->addAction(QStringLiteral("Angular Dimension"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(linearAction, &QAction::triggered, this, [this]() {
            activateDimensionTool(Tool::LinearDimension);
        });
        connect(angularAction, &QAction::triggered, this, [this]() {
            activateDimensionTool(Tool::AngularDimension);
        });
    }

    void createLineToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *lineAction = menu->addAction(QStringLiteral("Line"));
        QAction *tangentAction = menu->addAction(QStringLiteral("Tangent from Curve"));
        QAction *perpendicularAction =
            menu->addAction(QStringLiteral("Perpendicular from Curve"));
        QAction *perpendicularEdgeAction =
            menu->addAction(QStringLiteral("Perpendicular from Edge"));
        QAction *tangentTwoAction =
            menu->addAction(QStringLiteral("Tangent to Two Curves"));
        QAction *perpendicularTwoAction =
            menu->addAction(QStringLiteral("Perpendicular to Two Curves"));
        button->setMenu(menu);
        // A quick click runs Line; holding the button exposes the line variants.
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(lineAction, &QAction::triggered, this, [this]() {
            activateLineTool(Tool::Line);
        });
        connect(tangentAction, &QAction::triggered, this, [this]() {
            activateLineTool(Tool::TangentFromCurve);
        });
        connect(perpendicularAction, &QAction::triggered, this, [this]() {
            activateLineTool(Tool::PerpendicularFromCurve);
        });
        connect(perpendicularEdgeAction, &QAction::triggered, this, [this]() {
            activateLineTool(Tool::PerpendicularFromEdge);
        });
        connect(tangentTwoAction, &QAction::triggered, this, [this]() {
            activateLineTool(Tool::TangentToTwoCurves);
        });
        connect(perpendicularTwoAction, &QAction::triggered, this, [this]() {
            activateLineTool(Tool::PerpendicularToTwoCurves);
        });
    }

    void createArcToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *onePointAction = menu->addAction(QStringLiteral("1 Point Arc"));
        QAction *twoPointAction = menu->addAction(QStringLiteral("2 Point Arc"));
        QAction *threePointAction = menu->addAction(QStringLiteral("3 Point Arc"));
        button->setMenu(menu);
        // A quick click runs the default 1 Point Arc. Holding the button
        // opens this menu, matching the tool-flyout behavior requested here.
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(onePointAction, &QAction::triggered, this, [this]() {
            activateArcMode(ArcMode::OnePoint);
        });
        connect(twoPointAction, &QAction::triggered, this, [this]() {
            activateArcMode(ArcMode::TwoPoint);
        });
        connect(threePointAction, &QAction::triggered, this, [this]() {
            activateArcMode(ArcMode::ThreePoint);
        });
    }

    void activateEllipseTool(ToolId tool)
    {
        if (ellipseToolButton_ != nullptr) {
            ellipseToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
    }

    void activateCircleTool(ToolId tool)
    {
        if (circleToolButton_ != nullptr) {
            circleToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        QString message = QStringLiteral("Active tool: %1").arg(toolName(tool));
        if (tool == Tool::CircleTangentTwo) {
            message += QStringLiteral("  •  Click 2 curves, then move and click to place the circle");
        } else if (tool == Tool::CircleTangentThree) {
            message += QStringLiteral("  •  Click 3 curves, move to preview, Tab cycles tangent solutions, then click to place");
        }
        statusBar()->showMessage(message);
    }

    void createCircleToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *centerRadiusAction = menu->addAction(QStringLiteral("Center, Radius"));
        QAction *diameterAction = menu->addAction(QStringLiteral("2 Points (Diameter)"));
        QAction *threePointAction = menu->addAction(QStringLiteral("3 Points"));
        menu->addSeparator();
        QAction *tangentTwoAction = menu->addAction(QStringLiteral("Tangent to 2 Curves"));
        QAction *tangentThreeAction = menu->addAction(QStringLiteral("Tangent to 3 Curves"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(centerRadiusAction, &QAction::triggered, this, [this]() {
            activateCircleTool(Tool::Circle);
        });
        connect(diameterAction, &QAction::triggered, this, [this]() {
            activateCircleTool(Tool::CircleDiameter);
        });
        connect(threePointAction, &QAction::triggered, this, [this]() {
            activateCircleTool(Tool::CircleThreePoint);
        });
        connect(tangentTwoAction, &QAction::triggered, this, [this]() {
            activateCircleTool(Tool::CircleTangentTwo);
        });
        connect(tangentThreeAction, &QAction::triggered, this, [this]() {
            activateCircleTool(Tool::CircleTangentThree);
        });
    }

    void createEllipseToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *centerAction = menu->addAction(QStringLiteral("Center, Axis, Radius"));
        QAction *endpointsAction = menu->addAction(QStringLiteral("2 Axis Endpoints"));
        QAction *cornersAction = menu->addAction(QStringLiteral("Bounding Corners"));
        QAction *fociAction = menu->addAction(QStringLiteral("Foci + Point"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(centerAction, &QAction::triggered, this, [this]() {
            activateEllipseTool(Tool::Ellipse);
        });
        connect(endpointsAction, &QAction::triggered, this, [this]() {
            activateEllipseTool(Tool::EllipseFromEndpoints);
        });
        connect(cornersAction, &QAction::triggered, this, [this]() {
            activateEllipseTool(Tool::EllipseFromCorners);
        });
        connect(fociAction, &QAction::triggered, this, [this]() {
            activateEllipseTool(Tool::EllipseFromFoci);
        });
    }

    void activateRectangleTool(ToolId tool)
    {
        if (rectangleToolButton_ != nullptr) {
            rectangleToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
    }

    void createRectangleToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *cornerAction = menu->addAction(QStringLiteral("Corner, Corner"));
        QAction *centerAction = menu->addAction(QStringLiteral("Center, Corner"));
        QAction *threePointAction = menu->addAction(QStringLiteral("3 Points"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(cornerAction, &QAction::triggered, this, [this]() {
            activateRectangleTool(Tool::Rectangle);
        });
        connect(centerAction, &QAction::triggered, this, [this]() {
            activateRectangleTool(Tool::RectangleFromCenter);
        });
        connect(threePointAction, &QAction::triggered, this, [this]() {
            activateRectangleTool(Tool::RectangleThreePoint);
        });
    }

    void activatePolygonTool(ToolId tool)
    {
        if (polygonToolButton_ != nullptr) {
            polygonToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(
            QStringLiteral("Active tool: %1  •  Scroll to change the side count")
                .arg(toolName(tool)));
    }

    void createPolygonToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *centerCornerAction = menu->addAction(QStringLiteral("Center, Corner"));
        QAction *centerTangentAction = menu->addAction(QStringLiteral("Center, Tangent"));
        QAction *cornerCornerAction = menu->addAction(QStringLiteral("Corner, Corner"));
        QAction *edgeAction = menu->addAction(QStringLiteral("Edge / Side Size"));
        button->setMenu(menu);
        button->setPopupMode(QToolButton::DelayedPopup);

        connect(centerCornerAction, &QAction::triggered, this, [this]() {
            activatePolygonTool(Tool::PolygonCenterCorner);
        });
        connect(centerTangentAction, &QAction::triggered, this, [this]() {
            activatePolygonTool(Tool::PolygonCenterTangent);
        });
        connect(cornerCornerAction, &QAction::triggered, this, [this]() {
            activatePolygonTool(Tool::PolygonCornerCorner);
        });
        connect(edgeAction, &QAction::triggered, this, [this]() {
            activatePolygonTool(Tool::PolygonEdge);
        });
    }

    void loadPreferences()
    {
        QSettings settings;
        const QString savedPanButton = settings.value(QStringLiteral("keymap/panButton"),
                                                      QStringLiteral("middle"))
                                           .toString();
        applyPanButton(savedPanButton == QStringLiteral("right") ? Qt::RightButton
                                                                   : Qt::MiddleButton,
                       false);
        viewport_->setSnapLabelsVisible(
            settings.value(QStringLiteral("viewport/snapLabelsVisible"), true).toBool());
        viewport_->setSmoothCurveDisplay(
            settings.value(QStringLiteral("viewport/smoothCurveDisplay"), true).toBool());
        BlenderGridAppearance gridAppearance;
        const auto readGridColor = [&settings](const QString &key,
                                               const QColor &defaultColor) {
            const QColor stored(settings.value(key,
                                               defaultColor.name(QColor::HexArgb))
                                    .toString());
            return stored.isValid() ? stored : defaultColor;
        };
        gridAppearance.gridColor = readGridColor(QStringLiteral("viewport/gridColor"),
                                                 gridAppearance.gridColor);
        gridAppearance.emphasisColor = readGridColor(
            QStringLiteral("viewport/gridEmphasisColor"), gridAppearance.emphasisColor);
        gridAppearance.axisXColor = readGridColor(QStringLiteral("viewport/gridAxisXColor"),
                                                  gridAppearance.axisXColor);
        gridAppearance.axisYColor = readGridColor(QStringLiteral("viewport/gridAxisYColor"),
                                                  gridAppearance.axisYColor);
        gridAppearance.axisZColor = readGridColor(QStringLiteral("viewport/gridAxisZColor"),
                                                  gridAppearance.axisZColor);
        gridAppearance.opacity = settings.value(QStringLiteral("viewport/gridOpacity"),
                                                 gridAppearance.opacity).toDouble();
        gridAppearance.lowAlphaStipple = settings.value(
            QStringLiteral("viewport/gridStipple"), gridAppearance.lowAlphaStipple).toBool();
        if (!isValidBlenderGridAppearance(gridAppearance)) {
            gridAppearance = BlenderGridAppearance{};
        } else if ((gridAppearance.gridColor == QColor(QStringLiteral("#38858585")) &&
                    gridAppearance.emphasisColor == QColor(QStringLiteral("#4da1a1a1")) &&
                    gridAppearance.axisXColor == QColor(QStringLiteral("#b8c7332e")) &&
                    gridAppearance.axisYColor == QColor(QStringLiteral("#b842ad38")) &&
                    gridAppearance.axisZColor == QColor(QStringLiteral("#b83378d1"))) ||
                   (gridAppearance.gridColor == QColor(QStringLiteral("#805e5e5e")) &&
                    gridAppearance.emphasisColor == QColor(QStringLiteral("#ff686868")) &&
                    gridAppearance.axisXColor == QColor(QStringLiteral("#ebd1233e")) &&
                    gridAppearance.axisYColor == QColor(QStringLiteral("#eb6eb300")) &&
                    gridAppearance.axisZColor == QColor(QStringLiteral("#eb1a73d1"))) ||
                   (gridAppearance.gridColor == QColor(QStringLiteral("#80545454")) &&
                    gridAppearance.emphasisColor == QColor(QStringLiteral("#ff545454")) &&
                    gridAppearance.axisXColor == QColor(QStringLiteral("#ebd1233e")) &&
                    gridAppearance.axisYColor == QColor(QStringLiteral("#eb70b612")) &&
                    gridAppearance.axisZColor == QColor(QStringLiteral("#eb1a73d1"))) ||
                   (gridAppearance.gridColor == QColor(QStringLiteral("#80545454")) &&
                    gridAppearance.emphasisColor == QColor(QStringLiteral("#ff545454")) &&
                    gridAppearance.axisXColor == QColor(QStringLiteral("#ebd1233e")) &&
                    gridAppearance.axisYColor == QColor(QStringLiteral("#eb73be0e")) &&
                    gridAppearance.axisZColor == QColor(QStringLiteral("#eb1a73d1")))) {
            // Replace known shipped palettes only when every stored color
            // still matches; custom user colors survive this migration.
            gridAppearance.gridColor = BlenderGridAppearance{}.gridColor;
            gridAppearance.emphasisColor = BlenderGridAppearance{}.emphasisColor;
            gridAppearance.axisXColor = BlenderGridAppearance{}.axisXColor;
            gridAppearance.axisYColor = BlenderGridAppearance{}.axisYColor;
            gridAppearance.axisZColor = BlenderGridAppearance{}.axisZColor;
        }
        viewport_->setGridAppearance(gridAppearance);
        ViewportCameraPreferences cameraPreferences;
        cameraPreferences.focalLengthMillimeters = settings.value(
            QStringLiteral("viewport/focalLengthMillimeters"),
            cameraPreferences.focalLengthMillimeters).toDouble();
        cameraPreferences.clipStart = settings.value(
            QStringLiteral("viewport/clipStart"), cameraPreferences.clipStart).toDouble();
        cameraPreferences.clipEnd = settings.value(
            QStringLiteral("viewport/clipEnd"), cameraPreferences.clipEnd).toDouble();
        if (!viewport_->setCameraPreferences(cameraPreferences)) {
            viewport_->setCameraPreferences(ViewportCameraPreferences{});
        }

        ViewportNavigationPreferences navigationPreferences;
        navigationPreferences.autoPerspective = settings.value(
            QStringLiteral("navigation/autoPerspective"),
            navigationPreferences.autoPerspective).toBool();
        navigationPreferences.zoomToMouse = settings.value(
            QStringLiteral("navigation/zoomToMouse"),
            navigationPreferences.zoomToMouse).toBool();
        navigationPreferences.orbitAroundActive = settings.value(
            QStringLiteral("navigation/orbitAroundActive"),
            navigationPreferences.orbitAroundActive).toBool();
        navigationPreferences.useMouseDepthNavigate = settings.value(
            QStringLiteral("navigation/useMouseDepthNavigate"),
            navigationPreferences.useMouseDepthNavigate).toBool();
        navigationPreferences.turntableSensitivityRadiansPerPixel = settings.value(
            QStringLiteral("navigation/turntableSensitivityRadiansPerPixel"),
            navigationPreferences.turntableSensitivityRadiansPerPixel).toDouble();
        navigationPreferences.orbitMethod = settings.value(
            QStringLiteral("navigation/orbitMethod"), 0).toInt() == 1
                                                  ? ViewportOrbitMethod::Trackball
                                                  : ViewportOrbitMethod::Turntable;
        navigationPreferences.trackballSensitivity = settings.value(
            QStringLiteral("navigation/trackballSensitivity"),
            navigationPreferences.trackballSensitivity).toDouble();
        navigationPreferences.invertMouseZoom = settings.value(
            QStringLiteral("navigation/invertMouseZoom"),
            navigationPreferences.invertMouseZoom).toBool();
        navigationPreferences.invertZoomWheel = settings.value(
            QStringLiteral("navigation/invertZoomWheel"),
            navigationPreferences.invertZoomWheel).toBool();
        navigationPreferences.zoomMethod = settings.value(
            QStringLiteral("navigation/zoomMethod"), 0).toInt() == 1
                                                  ? ViewportZoomMethod::Scale
                                                  : ViewportZoomMethod::Dolly;
        navigationPreferences.zoomAxis = settings.value(
            QStringLiteral("navigation/zoomAxis"), 0).toInt() == 1
                                                ? ViewportZoomAxis::Horizontal
                                                : ViewportZoomAxis::Vertical;
        viewport_->setNavigationPreferences(navigationPreferences);

        const int requestedAaSamples = settings.value(
            QStringLiteral("system/viewportAaSamples"), 8).toInt();
        const int viewportAaSamples = requestedAaSamples == 2 || requestedAaSamples == 4 ||
                                              requestedAaSamples == 8
                                          ? requestedAaSamples
                                          : 0;
        viewport_->setViewportAntiAliasingSamples(viewportAaSamples);
        viewport_->setArchitecturalDimensionFont(
            settings.value(QStringLiteral("dimensions/architecturalFont"), false).toBool());

        if (orthoAction_ != nullptr) {
            orthoAction_->setChecked(settings.value(QStringLiteral("modeling/orthoEnabled"), false)
                                         .toBool());
        }

        if (endpointSnapCheckBox_ != nullptr) {
            endpointSnapCheckBox_->setChecked(settings.value(QStringLiteral("osnap/endpoint"), true)
                                                  .toBool());
            midpointSnapCheckBox_->setChecked(settings.value(QStringLiteral("osnap/midpoint"), true)
                                                  .toBool());
            intersectionSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/intersection"), true).toBool());
            centerSnapCheckBox_->setChecked(settings.value(QStringLiteral("osnap/center"), true)
                                                .toBool());
            perpendicularSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/perpendicular"), false).toBool());
            tangentSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/tangent"), false).toBool());
            nearSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/near"), false).toBool());
            controlPointSnapCheckBox_->setChecked(
                settings.value(QStringLiteral("osnap/controlPoint"), false).toBool());
        }

        if (osnapAction_ != nullptr) {
            osnapAction_->setChecked(settings.value(QStringLiteral("osnap/enabled"), false)
                                         .toBool());
        }

        if (controlPointsButton_ != nullptr) {
            controlPointsButton_->setChecked(
                settings.value(QStringLiteral("view/controlPoints"), false).toBool());
        }
    }

    void openPreferences()
    {
        QSettings settings;
        PreferencesDialog dialog(
            viewport_->panButton(),
            settings.value(QStringLiteral("viewport/snapLabelsVisible"), true).toBool(),
            settings.value(QStringLiteral("viewport/smoothCurveDisplay"), true).toBool(),
            settings.value(QStringLiteral("dimensions/architecturalFont"), false).toBool(),
            viewport_->gridAppearance(),
            viewport_->cameraPreferences(),
            viewport_->navigationPreferences(),
            viewport_->viewportAntiAliasingSamples(),
            this);
        if (dialog.exec() == QDialog::Accepted) {
            applyPanButton(dialog.panButton(), true);
            applySnapLabelsVisible(dialog.snapLabelsVisible(), true);
            applySmoothCurveDisplay(dialog.smoothCurveDisplay(), true);
            applyArchitecturalDimensionFont(dialog.architecturalDimensionFont(), true);
            applyGridAppearance(dialog.gridAppearance(), true);
            applyCameraPreferences(dialog.cameraPreferences(), true);
            applyNavigationPreferences(dialog.navigationPreferences(), true);
            applyViewportAntiAliasingSamples(dialog.viewportAaSamples(), true);
        }
    }

    void openDocumentGridSettings()
    {
        if (viewport_ == nullptr) {
            return;
        }
        DocumentGridDialog dialog(viewport_->documentSettings(), this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        if (!viewport_->setDocumentSettings(dialog.settings())) {
            QMessageBox::warning(this,
                                 QStringLiteral("Invalid Grid Settings"),
                                 QStringLiteral("Choose a positive, finite grid spacing and a supported unit."));
            return;
        }
        statusBar()->showMessage(QStringLiteral("Document grid settings updated"), 3000);
    }

    void applyArchitecturalDimensionFont(bool enabled, bool save)
    {
        viewport_->setArchitecturalDimensionFont(enabled);
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("dimensions/architecturalFont"), enabled);
            settings.sync();
            statusBar()->showMessage(
                QStringLiteral("Dimension lettering: %1")
                    .arg(enabled ? QStringLiteral("Architectural")
                                 : QStringLiteral("Standard")));
        }
    }

    void applySmoothCurveDisplay(bool enabled, bool save)
    {
        viewport_->setSmoothCurveDisplay(enabled);
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("viewport/smoothCurveDisplay"), enabled);
            settings.sync();
            statusBar()->showMessage(enabled ? QStringLiteral("Smooth curve display: On")
                                             : QStringLiteral("Smooth curve display: Off"));
        }
    }

    void applyGridAppearance(const BlenderGridAppearance &appearance, bool save)
    {
        if (viewport_ == nullptr || !isValidBlenderGridAppearance(appearance)) {
            return;
        }
        viewport_->setGridAppearance(appearance);
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("viewport/gridColor"),
                              appearance.gridColor.name(QColor::HexArgb));
            settings.setValue(QStringLiteral("viewport/gridEmphasisColor"),
                              appearance.emphasisColor.name(QColor::HexArgb));
            settings.setValue(QStringLiteral("viewport/gridAxisXColor"),
                              appearance.axisXColor.name(QColor::HexArgb));
            settings.setValue(QStringLiteral("viewport/gridAxisYColor"),
                              appearance.axisYColor.name(QColor::HexArgb));
            settings.setValue(QStringLiteral("viewport/gridAxisZColor"),
                              appearance.axisZColor.name(QColor::HexArgb));
            settings.setValue(QStringLiteral("viewport/gridOpacity"), appearance.opacity);
            settings.setValue(QStringLiteral("viewport/gridStipple"),
                              appearance.lowAlphaStipple);
            settings.sync();
        }
    }

    void applyCameraPreferences(const ViewportCameraPreferences &preferences, bool save)
    {
        if (viewport_ == nullptr || !viewport_->setCameraPreferences(preferences)) {
            return;
        }
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("viewport/focalLengthMillimeters"),
                              preferences.focalLengthMillimeters);
            settings.setValue(QStringLiteral("viewport/clipStart"), preferences.clipStart);
            settings.setValue(QStringLiteral("viewport/clipEnd"), preferences.clipEnd);
            settings.sync();
        }
    }

    void applyNavigationPreferences(
        const ViewportNavigationPreferences &preferences,
        bool save)
    {
        if (viewport_ == nullptr) {
            return;
        }
        viewport_->setNavigationPreferences(preferences);
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("navigation/autoPerspective"),
                              preferences.autoPerspective);
            settings.setValue(QStringLiteral("navigation/zoomToMouse"),
                              preferences.zoomToMouse);
            settings.setValue(QStringLiteral("navigation/orbitAroundActive"),
                              preferences.orbitAroundActive);
            settings.setValue(QStringLiteral("navigation/useMouseDepthNavigate"),
                              preferences.useMouseDepthNavigate);
            settings.setValue(
                QStringLiteral("navigation/turntableSensitivityRadiansPerPixel"),
                preferences.turntableSensitivityRadiansPerPixel);
            settings.setValue(QStringLiteral("navigation/orbitMethod"),
                              preferences.orbitMethod == ViewportOrbitMethod::Trackball
                                  ? 1
                                  : 0);
            settings.setValue(QStringLiteral("navigation/trackballSensitivity"),
                              preferences.trackballSensitivity);
            settings.setValue(QStringLiteral("navigation/invertMouseZoom"),
                              preferences.invertMouseZoom);
            settings.setValue(QStringLiteral("navigation/invertZoomWheel"),
                              preferences.invertZoomWheel);
            settings.setValue(QStringLiteral("navigation/zoomMethod"),
                              preferences.zoomMethod == ViewportZoomMethod::Scale ? 1 : 0);
            settings.setValue(QStringLiteral("navigation/zoomAxis"),
                              preferences.zoomAxis == ViewportZoomAxis::Horizontal ? 1 : 0);
            settings.sync();
        }
    }

    void applyViewportAntiAliasingSamples(int samples, bool save)
    {
        if (viewport_ == nullptr) {
            return;
        }
        viewport_->setViewportAntiAliasingSamples(samples);
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("system/viewportAaSamples"), samples);
            settings.sync();
        }
    }

    void applySnapLabelsVisible(bool visible, bool save)
    {
        viewport_->setSnapLabelsVisible(visible);
        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("viewport/snapLabelsVisible"), visible);
            settings.sync();
            statusBar()->showMessage(visible ? QStringLiteral("Snap type labels: On")
                                             : QStringLiteral("Snap type labels: Off"));
        }
    }

    void applyPanButton(Qt::MouseButton button, bool save)
    {
        viewport_->setPanButton(button);
        updateToolHelp();

        if (save) {
            QSettings settings;
            settings.setValue(QStringLiteral("keymap/panButton"),
                              button == Qt::RightButton ? QStringLiteral("right")
                                                        : QStringLiteral("middle"));
            settings.sync();
            statusBar()->showMessage(
                button == Qt::RightButton
                    ? QStringLiteral("Right mouse pans; Shift+right mouse orbits.")
                    : QStringLiteral("Middle mouse orbits; Shift+middle mouse pans."));
        }
    }

    void updateToolHelp()
    {
        if (toolHelp_ != nullptr && viewport_ != nullptr) {
            const QString alternatePan = viewport_->panButton() == Qt::RightButton
                                             ? QStringLiteral("\n\nRMB\nAlternate pan")
                                             : QString();
            toolHelp_->setText(QStringLiteral(
                                   "LMB\nDraw\n\nMMB\nOrbit\n\nShift+MMB\nPan%1\n\nWheel\nZoom")
                                   .arg(alternatePan));
        }
    }

    LayerId selectedLayerId() const
    {
        if (layerTable_ == nullptr || layerTable_->currentRow() < 0) {
            return LayerId::invalid();
        }
        const QTableWidgetItem *nameItem = layerTable_->item(layerTable_->currentRow(), 1);
        return nameItem == nullptr
                   ? LayerId::invalid()
                   : LayerId::fromValue(nameItem->data(Qt::UserRole).toULongLong());
    }

    LayerId currentLayerId() const
    {
        return currentLayerCombo_ == nullptr || currentLayerCombo_->currentIndex() < 0
                   ? LayerId::invalid()
                   : LayerId::fromValue(
                         currentLayerCombo_->currentData().toULongLong());
    }

    void refreshLayerToolbar(const QVector<ViewportLayerInfo> &infos)
    {
        if (currentLayerCombo_ == nullptr) {
            return;
        }

        const QSignalBlocker currentBlocker(currentLayerCombo_);
        const QSignalBlocker colorBlocker(layerColorCombo_);
        const QSignalBlocker lineTypeBlocker(layerLineTypeCombo_);
        const QSignalBlocker lineWeightBlocker(layerLineWeightCombo_);
        currentLayerCombo_->clear();
        int currentIndex = -1;
        const ViewportLayerInfo *activeLayer = nullptr;
        for (const ViewportLayerInfo &info : infos) {
            QPixmap swatch(12, 12);
            swatch.fill(info.color);
            currentLayerCombo_->addItem(
                QIcon(swatch),
                info.name,
                QVariant::fromValue<qulonglong>(info.id.value()));
            if (info.active) {
                currentIndex = currentLayerCombo_->count() - 1;
                activeLayer = &info;
            }
        }
        if (currentIndex >= 0) {
            currentLayerCombo_->setCurrentIndex(currentIndex);
        }
        if (activeLayer == nullptr) {
            return;
        }

        int colorIndex = -1;
        for (int index = 0; index < layerColorCombo_->count(); ++index) {
            if (!layerColorCombo_->itemData(index, Qt::UserRole + 1).toBool() &&
                layerColorCombo_->itemData(index).value<QColor>() == activeLayer->color) {
                colorIndex = index;
                break;
            }
        }
        const int customIndex = layerColorCombo_->count() - 1;
        if (colorIndex < 0 && customIndex >= 0) {
            colorIndex = customIndex;
            layerColorCombo_->setItemText(customIndex,
                                          activeLayer->color.name(QColor::HexRgb).toUpper());
            layerColorCombo_->setItemData(customIndex,
                                          activeLayer->color,
                                          Qt::UserRole);
            QPixmap swatch(12, 12);
            swatch.fill(activeLayer->color);
            layerColorCombo_->setItemIcon(customIndex, QIcon(swatch));
        } else if (customIndex >= 0) {
            layerColorCombo_->setItemText(customIndex, QStringLiteral("Custom…"));
            layerColorCombo_->setItemData(customIndex, QColor(), Qt::UserRole);
            layerColorCombo_->setItemData(customIndex, true, Qt::UserRole + 1);
            layerColorCombo_->setItemIcon(customIndex, QIcon());
        }
        if (colorIndex >= 0) {
            layerColorCombo_->setCurrentIndex(colorIndex);
        }

        const QString activeLineType =
            canonicalLayerLineTypeName(activeLayer->lineType);
        int lineTypeIndex = layerLineTypeCombo_->findData(activeLineType);
        if (lineTypeIndex < 0) {
            layerLineTypeCombo_->addItem(makeLayerLineTypeIcon(activeLayer->lineType),
                                         activeLayer->lineType,
                                         activeLayer->lineType);
            lineTypeIndex = layerLineTypeCombo_->count() - 1;
        }
        layerLineTypeCombo_->setCurrentIndex(lineTypeIndex);

        int lineWeightIndex = -1;
        for (int index = 0; index < layerLineWeightCombo_->count(); ++index) {
            if (qFuzzyCompare(layerLineWeightCombo_->itemData(index).toDouble() + 1.0,
                              activeLayer->lineWeightMm + 1.0)) {
                lineWeightIndex = index;
                break;
            }
        }
        if (lineWeightIndex >= 0) {
            layerLineWeightCombo_->setCurrentIndex(lineWeightIndex);
        }
    }

    void updateLayerControls()
    {
        if (layerTable_ == nullptr || viewport_ == nullptr) {
            return;
        }

        const LayerId selectedId = selectedLayerId();
        ViewportLayerInfo selectedInfo;
        bool found = false;
        for (const ViewportLayerInfo &info : viewport_->layerInfos()) {
            if (info.id == selectedId) {
                selectedInfo = info;
                found = true;
                break;
            }
        }

        if (activateLayerButton_ != nullptr) {
            activateLayerButton_->setEnabled(found && !selectedInfo.active);
        }
        if (removeLayerButton_ != nullptr) {
            removeLayerButton_->setEnabled(found && layerTable_->rowCount() > 1);
        }
        if (moveLayerUpButton_ != nullptr) {
            moveLayerUpButton_->setEnabled(found && layerTable_->currentRow() > 0);
        }
        if (moveLayerDownButton_ != nullptr) {
            moveLayerDownButton_->setEnabled(found &&
                                              layerTable_->currentRow() + 1 <
                                                  layerTable_->rowCount());
        }
        if (moveSelectedToLayerButton_ != nullptr) {
            moveSelectedToLayerButton_->setEnabled(found && selectedInfo.visible &&
                                                   !selectedInfo.frozen &&
                                                   !selectedInfo.locked);
        }
    }

    void refreshLayers()
    {
        if (layerTable_ == nullptr || viewport_ == nullptr) {
            return;
        }

        const LayerId previousSelection = selectedLayerId();
        refreshingLayers_ = true;
        const QVector<ViewportLayerInfo> infos = viewport_->layerInfos();
        const QSignalBlocker blocker(layerTable_);
        layerTable_->setRowCount(infos.size());
        int activeRow = -1;
        int previousRow = -1;
        const QString filter = layerFilter_ == nullptr
                                   ? QString()
                                   : layerFilter_->text().trimmed();
        for (int index = 0; index < infos.size(); ++index) {
            const ViewportLayerInfo &info = infos[index];
            if (info.id == previousSelection) {
                previousRow = index;
            }
            if (info.active) {
                activeRow = index;
            }

            auto *currentItem = new QTableWidgetItem(info.active ? QStringLiteral("✓")
                                                                  : QString());
            currentItem->setTextAlignment(Qt::AlignCenter);
            currentItem->setToolTip(info.active ? QStringLiteral("Current layer")
                                                 : QStringLiteral("Click to make current"));
            auto *nameItem = new QTableWidgetItem(info.name);
            nameItem->setToolTip(QStringLiteral("%1 object%2 — double-click to rename")
                                     .arg(info.objectCount)
                                     .arg(info.objectCount == 1 ? QString() : QStringLiteral("s")));
            if (info.active) {
                QFont activeFont = nameItem->font();
                activeFont.setBold(true);
                nameItem->setFont(activeFont);
            }

            const auto makeCheckItem = [](bool checked, const QString &tooltip) {
                auto *item = new QTableWidgetItem;
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                               Qt::ItemIsUserCheckable);
                item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
                item->setTextAlignment(Qt::AlignCenter);
                item->setToolTip(tooltip);
                return item;
            };
            auto *visibleItem = makeCheckItem(info.visible, QStringLiteral("Layer visibility"));
            auto *frozenItem = makeCheckItem(info.frozen, QStringLiteral("Freeze layer"));
            auto *lockedItem = makeCheckItem(info.locked, QStringLiteral("Lock layer"));
            auto *plottedItem = makeCheckItem(info.plotted, QStringLiteral("Plot this layer"));

            QPixmap colorSwatch(12, 12);
            colorSwatch.fill(Qt::transparent);
            {
                QPainter swatchPainter(&colorSwatch);
                swatchPainter.fillRect(QRect(1, 1, 10, 10), info.color);
                swatchPainter.setPen(QColor(80, 80, 80));
                swatchPainter.drawRect(QRect(0, 0, 11, 11));
            }
            auto *colorItem = new QTableWidgetItem;
            colorItem->setIcon(QIcon(colorSwatch));
            colorItem->setTextAlignment(Qt::AlignCenter);
            colorItem->setToolTip(
                QStringLiteral("%1 — click to change layer color")
                    .arg(info.color.name().toUpper()));

            auto *lineTypeCombo = new LayerLineTypeComboBox;
            populateLayerLineTypeCombo(lineTypeCombo, false);
            const QString rowLineType = canonicalLayerLineTypeName(info.lineType);
            int rowLineTypeIndex = lineTypeCombo->findData(rowLineType);
            if (rowLineTypeIndex < 0) {
                lineTypeCombo->addItem(makeLayerLineTypeIcon(info.lineType),
                                       info.lineType,
                                       info.lineType);
                rowLineTypeIndex = lineTypeCombo->count() - 1;
            }
            lineTypeCombo->setCurrentIndex(rowLineTypeIndex);
            lineTypeCombo->setToolTip(
                QStringLiteral("Linetype: %1").arg(info.lineType));

            auto *lineWeightCombo = new QComboBox;
            const QList<QPair<QString, qreal>> lineWeights{
                {QStringLiteral("Default"), 0.0},
                {QStringLiteral("0.13 mm"), 0.13},
                {QStringLiteral("0.18 mm"), 0.18},
                {QStringLiteral("0.25 mm"), 0.25},
                {QStringLiteral("0.35 mm"), 0.35},
                {QStringLiteral("0.50 mm"), 0.50},
                {QStringLiteral("0.70 mm"), 0.70},
                {QStringLiteral("1.00 mm"), 1.00},
                {QStringLiteral("1.40 mm"), 1.40},
                {QStringLiteral("2.00 mm"), 2.00},
                {QStringLiteral("2.11 mm"), 2.11}};
            for (const auto &lineWeight : lineWeights) {
                lineWeightCombo->addItem(lineWeight.first, lineWeight.second);
            }
            for (int weightIndex = 0; weightIndex < lineWeightCombo->count(); ++weightIndex) {
                if (qFuzzyCompare(lineWeightCombo->itemData(weightIndex).toDouble() + 1.0,
                                  info.lineWeightMm + 1.0)) {
                    lineWeightCombo->setCurrentIndex(weightIndex);
                    break;
                }
            }
            lineWeightCombo->setToolTip(QStringLiteral("Layer lineweight"));

            auto *descriptionItem = new QTableWidgetItem(info.description);
            descriptionItem->setToolTip(
                info.description.isEmpty()
                    ? QStringLiteral("Double-click to edit description")
                    : QStringLiteral("%1\nDouble-click to edit description")
                          .arg(info.description));

            const QList<QTableWidgetItem *> rowItems{
                currentItem, nameItem, visibleItem, frozenItem, lockedItem,
                colorItem, nullptr, nullptr, plottedItem, descriptionItem};
            for (int column = 0; column < rowItems.size(); ++column) {
                QTableWidgetItem *item = rowItems[column];
                if (item != nullptr) {
                    item->setData(Qt::UserRole,
                                  QVariant::fromValue<qulonglong>(info.id.value()));
                    layerTable_->setItem(index, column, item);
                }
            }
            lineTypeCombo->setProperty("layerId", QVariant::fromValue<qulonglong>(info.id.value()));
            lineWeightCombo->setProperty("layerId", QVariant::fromValue<qulonglong>(info.id.value()));
            layerTable_->setCellWidget(index, 6, lineTypeCombo);
            layerTable_->setCellWidget(index, 7, lineWeightCombo);
            connect(lineTypeCombo, qOverload<int>(&QComboBox::activated),
                    this, [this, id = info.id, lineTypeCombo](int lineTypeIndex) {
                        if (lineTypeIndex < 0) {
                            return;
                        }
                        const QString lineType =
                            lineTypeCombo->itemData(lineTypeIndex).toString();
                        QTimer::singleShot(0, this, [this, id, lineType]() {
                            setLayerLineType(id, lineType);
                        });
                    });
            connect(lineWeightCombo, &QComboBox::activated,
                    this, [this, id = info.id, lineWeightCombo](int) {
                        const qreal lineWeightMm =
                            lineWeightCombo->currentData().toDouble();
                        QTimer::singleShot(0, this, [this, id, lineWeightMm]() {
                            setLayerLineWeight(id, lineWeightMm);
                        });
                    });
            layerTable_->setRowHeight(index, 24);
            const bool matchesFilter = filter.isEmpty() ||
                                       info.name.contains(filter, Qt::CaseInsensitive);
            layerTable_->setRowHidden(index, !matchesFilter);
        }
        int targetRow = previousRow;
        if (targetRow < 0 || layerTable_->isRowHidden(targetRow)) {
            targetRow = activeRow;
        }
        if (targetRow >= 0 && layerTable_->isRowHidden(targetRow)) {
            targetRow = -1;
            for (int row = 0; row < layerTable_->rowCount(); ++row) {
                if (!layerTable_->isRowHidden(row)) {
                    targetRow = row;
                    break;
                }
            }
        }
        if (targetRow >= 0) {
            layerTable_->setCurrentCell(targetRow, 1);
        } else {
            layerTable_->setCurrentCell(-1, 0);
        }
        refreshingLayers_ = false;
        updateLayerControls();
        refreshLayerToolbar(infos);
    }

    bool executeLayerCommand(const ViewportLayerCommandRequest &request,
                             const QString &successMessage,
                             const QString &failureMessage)
    {
        if (viewport_ == nullptr) {
            return false;
        }
        const ViewportLayerCommandResult result = viewport_->executeLayerCommand(request);
        if (!result.accepted) {
            statusBar()->showMessage(failureMessage, 4000);
            return false;
        }
        if (!successMessage.isEmpty()) {
            statusBar()->showMessage(successMessage, 3000);
        }
        refreshLayers();
        return true;
    }

    void addLayer()
    {
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Add Layer"),
                                                   QStringLiteral("Layer name:"),
                                                   QLineEdit::Normal,
                                                   QStringLiteral("Layer"),
                                                   &accepted)
                                .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }

        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Create;
        request.name = name;
        executeLayerCommand(request,
                             QStringLiteral("Layer created"),
                             QStringLiteral("Could not create layer"));
    }

    void renameSelectedLayer()
    {
        const LayerId layerId = selectedLayerId();
        if (!layerId.isValid()) {
            return;
        }

        QString currentName;
        for (const ViewportLayerInfo &info : viewport_->layerInfos()) {
            if (info.id == layerId) {
                currentName = info.name;
                break;
            }
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Rename Layer"),
                                                   QStringLiteral("Layer name:"),
                                                   QLineEdit::Normal,
                                                   currentName,
                                                   &accepted)
                                .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }

        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Rename;
        request.layerId = layerId;
        request.name = name;
        executeLayerCommand(request,
                             QStringLiteral("Layer renamed"),
                             QStringLiteral("Could not rename layer"));
    }

    void removeSelectedLayer()
    {
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Remove;
        request.layerId = selectedLayerId();
        executeLayerCommand(request,
                             QStringLiteral("Layer removed"),
                             QStringLiteral("Only empty layers can be removed"));
    }

    void activateSelectedLayer()
    {
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Activate;
        request.layerId = selectedLayerId();
        executeLayerCommand(request,
                             QStringLiteral("Active layer changed"),
                             QStringLiteral("A layer must be visible and unlocked to become active"));
    }

    void setLayerVisible(LayerId layerId, bool visible)
    {
        if (refreshingLayers_ || !layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetVisible;
        request.layerId = layerId;
        request.enabled = visible;
        executeLayerCommand(request,
                             visible ? QStringLiteral("Layer shown") : QStringLiteral("Layer hidden"),
                             QStringLiteral("At least one other visible, unlocked layer is required"));
    }

    void setLayerLocked(LayerId layerId, bool locked)
    {
        if (refreshingLayers_ || !layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetLocked;
        request.layerId = layerId;
        request.enabled = locked;
        executeLayerCommand(request,
                             locked ? QStringLiteral("Layer locked") : QStringLiteral("Layer unlocked"),
                            QStringLiteral("At least one other visible, unlocked layer is required"));
    }

    void setLayerColor(LayerId layerId, const QColor &color)
    {
        if (!layerId.isValid() || !color.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetColor;
        request.layerId = layerId;
        request.color = color;
        executeLayerCommand(request, QString(), QStringLiteral("Could not update layer color"));
    }

    void setLayerFrozen(LayerId layerId, bool frozen)
    {
        if (refreshingLayers_ || !layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetFrozen;
        request.layerId = layerId;
        request.enabled = frozen;
        executeLayerCommand(request,
                            frozen ? QStringLiteral("Layer frozen")
                                   : QStringLiteral("Layer thawed"),
                            QStringLiteral("Another visible, thawed, unlocked layer is required"));
    }

    void setLayerPlotted(LayerId layerId, bool plotted)
    {
        if (refreshingLayers_ || !layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetPlotted;
        request.layerId = layerId;
        request.enabled = plotted;
        executeLayerCommand(request, QString(), QStringLiteral("Could not update plot setting"));
    }

    void setLayerLineType(LayerId layerId, const QString &lineType)
    {
        if (refreshingLayers_ || !layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetLineType;
        request.layerId = layerId;
        request.name = lineType;
        executeLayerCommand(request, QString(), QStringLiteral("Could not update linetype"));
    }

    void setLayerLineWeight(LayerId layerId, qreal lineWeightMm)
    {
        if (refreshingLayers_ || !layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetLineWeight;
        request.layerId = layerId;
        request.lineWeightMm = lineWeightMm;
        executeLayerCommand(request, QString(), QStringLiteral("Could not update lineweight"));
    }

    void setLayerDescription(LayerId layerId, const QString &description)
    {
        if (!layerId.isValid()) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetDescription;
        request.layerId = layerId;
        request.name = description;
        executeLayerCommand(request,
                            QStringLiteral("Layer description updated"),
                            QStringLiteral("Could not update description"));
    }

    void editLayerDescription(int row)
    {
        if (layerTable_ == nullptr || viewport_ == nullptr || row < 0 ||
            row >= layerTable_->rowCount()) {
            return;
        }
        const QTableWidgetItem *nameItem = layerTable_->item(row, 1);
        if (nameItem == nullptr) {
            return;
        }
        const LayerId layerId = LayerId::fromValue(nameItem->data(Qt::UserRole).toULongLong());
        QString description;
        for (const ViewportLayerInfo &info : viewport_->layerInfos()) {
            if (info.id == layerId) {
                description = info.description;
                break;
            }
        }
        bool accepted = false;
        const QString updatedDescription = QInputDialog::getMultiLineText(
            this,
            QStringLiteral("Layer Description"),
            QStringLiteral("Description:"),
            description,
            &accepted);
        if (accepted && updatedDescription != description) {
            setLayerDescription(layerId, updatedDescription);
        }
    }

    void chooseLayerColor(int row)
    {
        if (layerTable_ == nullptr || viewport_ == nullptr || row < 0 ||
            row >= layerTable_->rowCount()) {
            return;
        }
        const QTableWidgetItem *nameItem = layerTable_->item(row, 1);
        if (nameItem == nullptr) {
            return;
        }
        const LayerId layerId = LayerId::fromValue(nameItem->data(Qt::UserRole).toULongLong());
        QColor currentColor;
        for (const ViewportLayerInfo &info : viewport_->layerInfos()) {
            if (info.id == layerId) {
                currentColor = info.color;
                break;
            }
        }
        if (!currentColor.isValid()) {
            return;
        }

        const QColor color = QColorDialog::getColor(currentColor,
                                                    this,
                                                    QStringLiteral("Layer Color"));
        if (!color.isValid() || color == currentColor) {
            return;
        }
        setLayerColor(layerId, color);
    }

    void moveSelectedLayer(int delta)
    {
        if (layerTable_ == nullptr) {
            return;
        }
        const int targetIndex = layerTable_->currentRow() + delta;
        if (targetIndex < 0 || targetIndex >= layerTable_->rowCount()) {
            return;
        }

        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Move;
        request.layerId = selectedLayerId();
        request.index = targetIndex;
        executeLayerCommand(request,
                             QStringLiteral("Layer order updated"),
                             QStringLiteral("Could not reorder layer"));
    }

    void moveSelectedObjectsToLayer()
    {
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::MoveSelectedObjects;
        request.layerId = selectedLayerId();
        const ViewportLayerCommandResult result = viewport_->executeLayerCommand(request);
        if (!result.accepted) {
            statusBar()->showMessage(QStringLiteral("Select editable objects and a visible, unlocked target layer"),
                                     5000);
            return;
        }
        statusBar()->showMessage(QStringLiteral("Moved %1 object%2 to %3")
                                     .arg(result.count)
                                     .arg(result.count == 1 ? QString() : QStringLiteral("s"))
                                     .arg(selectedLayerId().isValid()
                                              ? QStringLiteral("the selected layer")
                                              : QStringLiteral("the layer")),
                                 4000);
        refreshLayers();
    }

    QWidget *createRightPanel()
    {
        auto *panel = new QFrame;
        panel->setObjectName(QStringLiteral("rightPanel"));
        panel->setMinimumWidth(240);

        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *layersBox = new QGroupBox(QStringLiteral("Layers"));
        auto *layersLayout = new QVBoxLayout(layersBox);
        auto *layerActionsLayout = new QHBoxLayout;
        addLayerButton_ = new QPushButton(QStringLiteral("+"));
        addLayerButton_->setToolTip(QStringLiteral("Add layer"));
        removeLayerButton_ = new QPushButton(QStringLiteral("-"));
        removeLayerButton_->setToolTip(QStringLiteral("Remove selected empty layer"));
        activateLayerButton_ = new QPushButton(QStringLiteral("✓"));
        activateLayerButton_->setToolTip(QStringLiteral("Set selected layer as current"));
        moveLayerUpButton_ = new QPushButton(QStringLiteral("↑"));
        moveLayerUpButton_->setToolTip(QStringLiteral("Move layer up"));
        moveLayerDownButton_ = new QPushButton(QStringLiteral("↓"));
        moveLayerDownButton_->setToolTip(QStringLiteral("Move layer down"));
        for (QPushButton *button : {addLayerButton_,
                                    removeLayerButton_,
                                    activateLayerButton_,
                                    moveLayerUpButton_,
                                    moveLayerDownButton_}) {
            button->setFixedWidth(34);
            layerActionsLayout->addWidget(button);
        }
        layerActionsLayout->addStretch(1);
        layersLayout->addLayout(layerActionsLayout);

        layerFilter_ = new QLineEdit;
        layerFilter_->setObjectName(QStringLiteral("layerFilter"));
        layerFilter_->setPlaceholderText(QStringLiteral("Filter layers"));
        layersLayout->addWidget(layerFilter_);

        layerTable_ = new QTableWidget(0, 10);
        layerTable_->setObjectName(QStringLiteral("layerTable"));
        QFont layerHeaderFont = layerTable_->horizontalHeader()->font();
        layerHeaderFont.setBold(false);
        layerHeaderFont.setWeight(QFont::Normal);
        layerTable_->horizontalHeader()->setFont(layerHeaderFont);
        layerTable_->setHorizontalHeaderLabels(
            {QString(), QStringLiteral("Name"), QString(),
             QString(), QString(), QString(),
             QStringLiteral("Linetype"), QStringLiteral("Lineweight"),
             QString(), QStringLiteral("Description")});
        const auto setIconHeader = [this](int column, LayerHeaderIcon icon,
                                          const QString &tooltip) {
            QTableWidgetItem *header = layerTable_->horizontalHeaderItem(column);
            header->setIcon(makeLayerHeaderIcon(icon));
            header->setTextAlignment(Qt::AlignCenter);
            header->setToolTip(tooltip);
        };
        setIconHeader(2, LayerHeaderIcon::Visibility, QStringLiteral("Visibility"));
        setIconHeader(3, LayerHeaderIcon::Freeze, QStringLiteral("Freeze / thaw"));
        setIconHeader(4, LayerHeaderIcon::Lock, QStringLiteral("Lock / unlock"));
        setIconHeader(5, LayerHeaderIcon::Color, QStringLiteral("Layer color"));
        setIconHeader(8, LayerHeaderIcon::Plot, QStringLiteral("Plot / do not plot"));
        layerTable_->verticalHeader()->setVisible(false);
        layerTable_->horizontalHeader()->setStretchLastSection(false);
        layerTable_->horizontalHeader()->setMinimumSectionSize(20);
        for (int column = 0; column < 10; ++column) {
            layerTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        }
        layerTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
        layerTable_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
        layerTable_->setColumnWidth(0, 22);
        layerTable_->setColumnWidth(1, 110);
        layerTable_->setColumnWidth(2, 22);
        layerTable_->setColumnWidth(3, 22);
        layerTable_->setColumnWidth(4, 22);
        layerTable_->setColumnWidth(5, 22);
        layerTable_->setColumnWidth(6, 68);
        layerTable_->setColumnWidth(7, 72);
        layerTable_->setColumnWidth(8, 22);
        layerTable_->setColumnWidth(9, 75);
        layerTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        layerTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        layerTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        layerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layerTable_->setShowGrid(false);
        layerTable_->setMinimumHeight(190);
        layersLayout->addWidget(layerTable_, 1);

        moveSelectedToLayerButton_ = new QPushButton(QStringLiteral("Move Selected Here"));
        moveSelectedToLayerButton_->setToolTip(
            QStringLiteral("Move the selected geometry to this layer"));
        layersLayout->addWidget(moveSelectedToLayerButton_);

        connect(layerTable_, &QTableWidget::currentCellChanged,
                this, [this](int, int, int, int) {
                    updateLayerControls();
                });
        connect(layerTable_, &QTableWidget::itemChanged,
                this, [this](QTableWidgetItem *item) {
                    if (refreshingLayers_ || item == nullptr) {
                        return;
                    }
                    const LayerId layerId = LayerId::fromValue(
                        item->data(Qt::UserRole).toULongLong());
                    if (item->column() == 2) {
                        setLayerVisible(layerId, item->checkState() == Qt::Checked);
                    } else if (item->column() == 3) {
                        setLayerFrozen(layerId, item->checkState() == Qt::Checked);
                    } else if (item->column() == 4) {
                        setLayerLocked(layerId, item->checkState() == Qt::Checked);
                    } else if (item->column() == 8) {
                        setLayerPlotted(layerId, item->checkState() == Qt::Checked);
                    }
                });
        connect(layerTable_, &QTableWidget::cellClicked,
                this, [this](int row, int column) {
                    layerTable_->setCurrentCell(row, column);
                    if (column == 0) {
                        activateSelectedLayer();
                    } else if (column == 5) {
                        chooseLayerColor(row);
                    }
                });
        connect(layerTable_, &QTableWidget::cellDoubleClicked,
                this, [this](int row, int column) {
                    if (column == 1) {
                        layerTable_->setCurrentCell(row, column);
                        renameSelectedLayer();
                    } else if (column == 9) {
                        editLayerDescription(row);
                    }
                });
        connect(layerFilter_, &QLineEdit::textChanged,
                this, [this](const QString &) {
                    refreshLayers();
                });
        connect(addLayerButton_, &QPushButton::clicked, this, [this]() {
            addLayer();
        });
        connect(removeLayerButton_, &QPushButton::clicked, this, [this]() {
            removeSelectedLayer();
        });
        connect(moveLayerUpButton_, &QPushButton::clicked, this, [this]() {
            moveSelectedLayer(-1);
        });
        connect(moveLayerDownButton_, &QPushButton::clicked, this, [this]() {
            moveSelectedLayer(1);
        });
        connect(activateLayerButton_, &QPushButton::clicked, this, [this]() {
            activateSelectedLayer();
        });
        connect(moveSelectedToLayerButton_, &QPushButton::clicked,
                this, [this]() {
                    moveSelectedObjectsToLayer();
                });

        layout->addWidget(layersBox, 1);

        return panel;
    }

    void applyTheme()
    {
        setStyleSheet(QStringLiteral(R"(
            QMainWindow, QWidget {
                background: #282828;
                color: #d6d6d6;
                font-family: "Sans";
                font-size: 11px;
            }
            QMenuBar {
                background: #202020;
                border-bottom: 1px solid #121212;
                padding: 2px 4px;
            }
            QMenuBar::item {
                padding: 5px 9px;
                background: transparent;
            }
            QMenuBar::item:selected, QMenu::item:selected {
                background: #4a4a4a;
            }
            QMenu {
                background: #303030;
                border: 1px solid #151515;
            }
            QMenu::item {
                padding: 5px 28px 5px 12px;
            }
            QToolBar#workspaceBar {
                background: #242424;
                border: 0;
                border-bottom: 1px solid #171717;
                spacing: 5px;
                padding: 4px 8px;
            }
            QToolBar#layerPropertiesBar {
                background: #242424;
                border: 0;
                border-bottom: 1px solid #171717;
                spacing: 5px;
                padding: 4px 8px;
            }
            QToolBar#layerPropertiesBar QComboBox {
                min-height: 22px;
                padding: 2px 5px;
                background: #303030;
                border: 1px solid #444444;
            }
            QLabel#brand {
                color: #f0a45a;
                font-size: 15px;
                font-weight: bold;
                padding-right: 8px;
            }
            QToolButton#workspaceButton {
                border: 1px solid transparent;
                border-radius: 3px;
                padding: 5px 10px;
            }
            QToolButton#workspaceButton:checked,
            QToolButton#workspaceButton:hover {
                background: #454545;
                border-color: #5d5d5d;
            }
            QToolBar#osnapLane {
                background: #232323;
                border-top: 1px solid #151515;
                border-bottom: 1px solid #151515;
                spacing: 4px;
                padding: 3px 8px;
            }
            QLabel#osnapLaneLabel {
                color: #777777;
                font-size: 9px;
                font-weight: bold;
                padding-right: 6px;
            }
            QCheckBox#osnapCheckBox {
                color: #c7c7c7;
                spacing: 5px;
                padding: 3px 7px;
                border-radius: 3px;
            }
            QCheckBox#osnapCheckBox:hover {
                background: #414141;
            }
            QCheckBox#osnapCheckBox::indicator {
                width: 13px;
                height: 13px;
                background: #303030;
                border: 1px solid #686868;
                border-radius: 2px;
            }
            QCheckBox#osnapCheckBox::indicator:checked {
                background: #537da0;
                border-color: #82c7ec;
            }
            QFrame#toolShelf, QFrame#rightPanel {
                background: #232323;
                border: 0;
            }
            QFrame#toolShelf {
                border-right: 1px solid #151515;
            }
            QFrame#rightPanel {
                border-left: 1px solid #151515;
            }
            QSplitter#workspaceSplitter::handle {
                background: #202020;
            }
            QSplitter#workspaceSplitter::handle:horizontal {
                width: 6px;
            }
            QSplitter#workspaceSplitter::handle:horizontal:hover {
                background: #303030;
            }
            QLabel#shelfLabel {
                color: #777777;
                font-size: 9px;
                font-weight: bold;
                padding-bottom: 4px;
            }
            QToolButton#toolButton {
                background: #303030;
                border: 1px solid #3b3b3b;
                border-radius: 3px;
                color: #c7c7c7;
                min-height: 43px;
                padding: 3px 1px;
            }
            QToolButton#toolButton:hover {
                background: #414141;
                border-color: #686868;
            }
            QToolButton#toolButton:checked {
                background: #9b5b2e;
                border-color: #e39a54;
                color: #ffffff;
            }
            QLabel#toolHelp, QLabel#panelHint {
                color: #777777;
            }
            QGroupBox {
                border: 1px solid #3d3d3d;
                border-radius: 3px;
                margin-top: 8px;
                padding: 8px 6px 6px 6px;
                font-weight: bold;
                color: #bbbbbb;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 8px;
                padding: 0 4px;
            }
            QListWidget {
                background: #2b2b2b;
                border: 0;
                padding: 3px;
            }
            QListWidget::item {
                padding: 4px;
            }
            QListWidget::item:selected {
                background: #5a3824;
            }
            QTableWidget#layerTable {
                background: #2b2b2b;
                alternate-background-color: #303030;
                border: 0;
                selection-background-color: #5a3824;
                selection-color: #ffffff;
            }
            QTableWidget#layerTable QComboBox {
                min-height: 20px;
                padding: 1px 3px;
                background: #303030;
                border: 0;
            }
            QTableWidget#layerTable::item {
                padding: 2px 3px;
            }
            QHeaderView::section {
                background: #303030;
                border: 0;
                color: #999999;
                padding: 3px;
            }
            QLineEdit#layerFilter {
                background: #222222;
                border: 1px solid #444444;
                padding: 4px;
            }
            QListWidget#preferencesCategories {
                background: #3a3a3a;
                border: 0;
                padding: 2px;
            }
            QListWidget#preferencesCategories::item {
                padding: 7px 6px;
                border-bottom: 1px solid #454545;
            }
            QListWidget#preferencesCategories::item:selected {
                background: #537db5;
                color: #ffffff;
            }
            QLabel#preferencesTitle {
                color: #eeeeee;
                font-size: 16px;
                font-weight: bold;
                padding-bottom: 6px;
            }
            QLabel#preferencesHint {
                color: #999999;
            }
            QDialog QGroupBox {
                background: #303030;
            }
            QToolButton#statusToggle {
                background: transparent;
                border: 1px solid transparent;
                border-radius: 3px;
                color: #999999;
                padding: 2px 9px;
                margin: 1px 3px;
            }
            QToolButton#statusToggle:hover {
                background: #3d3d3d;
            }
            QToolButton#statusToggle:checked {
                background: #9b5b2e;
                border-color: #e39a54;
                color: #ffffff;
            }
            QStatusBar {
                background: #202020;
                color: #999999;
                border-top: 1px solid #151515;
            }
        )"));
    }

    ViewportWidgetApi *viewport_ = nullptr;
    QLabel *coordinateLabel_ = nullptr;
    QLabel *toolHelp_ = nullptr;
    QToolButton *selectToolButton_ = nullptr;
    QToolButton *pointToolButton_ = nullptr;
    QToolButton *bezierToolButton_ = nullptr;
    QToolButton *lineToolButton_ = nullptr;
    QToolButton *arcToolButton_ = nullptr;
    QToolButton *rectangleToolButton_ = nullptr;
    QToolButton *polygonToolButton_ = nullptr;
    QToolButton *circleToolButton_ = nullptr;
    QToolButton *ellipseToolButton_ = nullptr;
    QToolButton *dimensionToolButton_ = nullptr;
    QToolButton *eraseToolButton_ = nullptr;
    QToolButton *trimToolButton_ = nullptr;
    QToolButton *controlPointsButton_ = nullptr;
    QToolButton *subdivideButton_ = nullptr;
    QToolButton *joinButton_ = nullptr;
    QToolButton *explodeButton_ = nullptr;
    QToolButton *rotateToolButton_ = nullptr;
    QToolButton *mirrorToolButton_ = nullptr;
    QToolButton *scaleToolButton_ = nullptr;
    ScaleMode scaleMode_ = ScaleMode::TwoD;
    QComboBox *currentLayerCombo_ = nullptr;
    QActionGroup *workPlaneActionGroup_ = nullptr;
    QComboBox *layerColorCombo_ = nullptr;
    QComboBox *layerLineTypeCombo_ = nullptr;
    QComboBox *layerLineWeightCombo_ = nullptr;
    QTableWidget *layerTable_ = nullptr;
    QLineEdit *layerFilter_ = nullptr;
    QPushButton *addLayerButton_ = nullptr;
    QPushButton *removeLayerButton_ = nullptr;
    QPushButton *moveLayerUpButton_ = nullptr;
    QPushButton *moveLayerDownButton_ = nullptr;
    QPushButton *activateLayerButton_ = nullptr;
    QPushButton *moveSelectedToLayerButton_ = nullptr;
    bool refreshingLayers_ = false;
    QVector<QToolButton *> toolButtons_;
    QString currentProjectPath_;
    bool documentModified_ = false;
    bool suppressDirtyTracking_ = false;
    bool updateRestartInProgress_ = false;
    QAction *undoAction_ = nullptr;
    QAction *redoAction_ = nullptr;
    QAction *subdivideAction_ = nullptr;
    QAction *joinAction_ = nullptr;
    QAction *eraseAction_ = nullptr;
    QAction *trimAction_ = nullptr;
    QAction *updateAction_ = nullptr;
    QAction *orthoAction_ = nullptr;
    QAction *osnapAction_ = nullptr;
    QCheckBox *endpointSnapCheckBox_ = nullptr;
    QCheckBox *midpointSnapCheckBox_ = nullptr;
    QCheckBox *intersectionSnapCheckBox_ = nullptr;
    QCheckBox *centerSnapCheckBox_ = nullptr;
    QCheckBox *perpendicularSnapCheckBox_ = nullptr;
    QCheckBox *tangentSnapCheckBox_ = nullptr;
    QCheckBox *nearSnapCheckBox_ = nullptr;
    QCheckBox *controlPointSnapCheckBox_ = nullptr;
    QToolBar *osnapLane_ = nullptr;
    QProcess *updateProcess_ = nullptr;
};

int runApplication(QApplication &application, const QString &updateSessionPath)
{
    MainWindow window;
    if (!updateSessionPath.isEmpty()) {
        window.restoreUpdateSession(updateSessionPath);
    }
    window.show();
    return application.exec();
}

} // namespace classiCAD
