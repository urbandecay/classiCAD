#include "main_window.h"
#include "viewport_widget_api.h"
#include "input_helpers.h"
#include "../core/debug_log.h"

#include <QApplication>
#include <QAction>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

namespace classiCAD {

class PreferencesDialog final : public QDialog {
public:
    explicit PreferencesDialog(Qt::MouseButton panButton,
                               bool snapLabelsVisible,
                               bool smoothCurveDisplay,
                               QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(760, 520);
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
                                                     smoothCurveDisplay));
            } else if (category == QStringLiteral("Keymap")) {
                pages_->addWidget(createKeymapPage(panButton));
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
        panButtonCombo_->addItem(QStringLiteral("Middle Mouse Button"));
        panButtonCombo_->addItem(QStringLiteral("Right Mouse Button"));
        panButtonCombo_->setCurrentIndex(panButton == Qt::RightButton ? 1 : 0);
        navigationLayout->addRow(QStringLiteral("Pan with"), panButtonCombo_);
        layout->addWidget(navigationBox);

        auto *hint = new QLabel(QStringLiteral(
            "Choose which mouse button pans the 2D viewport. The mouse wheel continues to zoom, "
            "and Alt + Left Mouse Button remains available as an alternate pan shortcut."));
        hint->setObjectName(QStringLiteral("preferencesHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addStretch(1);
        return page;
    }

    QWidget *createViewportPage(bool snapLabelsVisible, bool smoothCurveDisplay)
    {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
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
        layout->addStretch(1);
        return page;
    }

    QListWidget *categoryList_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QComboBox *panButtonCombo_ = nullptr;
    QCheckBox *snapLabelsCheckBox_ = nullptr;
    QCheckBox *smoothCurveDisplayCheckBox_ = nullptr;
};

class MainWindow final : public QMainWindow {
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("classiCAD — 2D NURBS Modeler"));
        resize(1440, 900);
        setMinimumSize(980, 620);

        createMenus();
        createWorkspaceBar();
        createMainLayout();
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

private:
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
                    QTimer::singleShot(0, []() {
                        QCoreApplication::quit();
                    });
                });

        process->start(QStringLiteral("cmake"),
                       QStringList{QStringLiteral("--build"), buildDirectory});
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
        fileMenu->addAction(QStringLiteral("New Document"));
        fileMenu->addAction(QStringLiteral("Open…"));
        fileMenu->addSeparator();
        fileMenu->addAction(QStringLiteral("Save"));
        fileMenu->addAction(QStringLiteral("Save As…"));
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
        QLabel *mode = new QLabel(QStringLiteral("2D NURBS"));
        mode->setObjectName(QStringLiteral("modeLabel"));
        bar->addWidget(mode);

        bar->addSeparator();
        updateAction_ = new QAction(QStringLiteral("Update"), this);
        updateAction_->setToolTip(QStringLiteral("Rebuild and restart classiCAD, preserving the current scene"));
        connect(updateAction_, &QAction::triggered, this, [this]() {
            updateApplication();
        });
        bar->addAction(updateAction_);

        addToolBar(Qt::TopToolBarArea, bar);
    }

    void createMainLayout()
    {
        auto *root = new QWidget;
        auto *rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        rootLayout->addWidget(createToolShelf());

        viewport_ = createViewportWidget(root);
        ViewportUiCallbacks viewportCallbacks;
        viewportCallbacks.commandFinished = [this](ToolId tool) {
            if (tool == Tool::Select && selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
                statusBar()->showMessage(QStringLiteral("Select mode"));
            }
        };
        viewportCallbacks.toolRepeated = [this](ToolId tool) {
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
        rootLayout->addWidget(viewport_, 1);

        rootLayout->addWidget(createRightPanel());
        setCentralWidget(root);
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
        viewportCallbacks.historyChanged = [this]() {
            updateHistoryActions();
        };
        viewportCallbacks.layersChanged = [this]() {
            refreshLayers();
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
        viewport_->setUiCallbacks(viewportCallbacks);
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
        auto *shelf = new QFrame;
        shelf->setObjectName(QStringLiteral("toolShelf"));
        shelf->setFixedWidth(82);

        auto *layout = new QVBoxLayout(shelf);
        layout->setContentsMargins(7, 10, 7, 10);
        layout->setSpacing(6);

        QLabel *label = new QLabel(QStringLiteral("TOOLS"));
        label->setObjectName(QStringLiteral("shelfLabel"));
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);

        auto *group = new QButtonGroup(shelf);
        group->setExclusive(true);

        selectToolButton_ = addToolButton(layout, group, QStringLiteral("↖\nSelect"), Tool::Select, true);
        addToolButton(layout, group, QStringLiteral("•\nPoint"), Tool::Point);
        lineToolButton_ = addToolButton(layout, group, QStringLiteral("╱\nLine"), Tool::Line);
        createLineToolMenu(lineToolButton_);
        arcToolButton_ = addToolButton(layout, group, QStringLiteral("⌒\nArc"), Tool::Arc);
        createArcToolMenu(arcToolButton_);
        addToolButton(layout, group, QStringLiteral("∿\nBezier"), Tool::Bezier);
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
        addToolButton(layout, group, QStringLiteral("○\nCircle"), Tool::Circle);
        ellipseToolButton_ = addToolButton(layout,
                                           group,
                                           QStringLiteral("⬭\nEllipse"),
                                           Tool::Ellipse);
        createEllipseToolMenu(ellipseToolButton_);
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

        layout->addSpacing(8);
        controlPointsButton_ = new QToolButton;
        controlPointsButton_->setObjectName(QStringLiteral("toolButton"));
        controlPointsButton_->setText(QStringLiteral("CP\nPoints"));
        controlPointsButton_->setToolTip(QStringLiteral("Control Points"));
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

        return shelf;
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
            if (tool == Tool::Rotate) {
                startRotate();
                return;
            }
            if (tool == Tool::Mirror) {
                startMirror();
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
    }

    void createArcToolMenu(QToolButton *button)
    {
        if (button == nullptr) {
            return;
        }

        auto *menu = new QMenu(button);
        QAction *onePointAction = menu->addAction(QStringLiteral("1 Point Arc"));
        QAction *twoPointAction = menu->addAction(QStringLiteral("2 Point Arc"));
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
    }

    void activateEllipseTool(ToolId tool)
    {
        if (ellipseToolButton_ != nullptr) {
            ellipseToolButton_->setChecked(true);
        }
        viewport_->setTool(tool);
        statusBar()->showMessage(QStringLiteral("Active tool: %1").arg(toolName(tool)));
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
            this);
        if (dialog.exec() == QDialog::Accepted) {
            applyPanButton(dialog.panButton(), true);
            applySnapLabelsVisible(dialog.snapLabelsVisible(), true);
            applySmoothCurveDisplay(dialog.smoothCurveDisplay(), true);
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
            statusBar()->showMessage(QStringLiteral("Viewport pan: %1").arg(mouseButtonName(button)));
        }
    }

    void updateToolHelp()
    {
        if (toolHelp_ != nullptr && viewport_ != nullptr) {
            toolHelp_->setText(QStringLiteral("LMB\nDraw\n\n%1\nPan\n\nWheel\nZoom")
                                   .arg(mouseButtonName(viewport_->panButton())));
        }
    }

    LayerId selectedLayerId() const
    {
        if (layerList_ == nullptr || layerList_->currentItem() == nullptr) {
            return LayerId::invalid();
        }
        return LayerId::fromValue(
            layerList_->currentItem()->data(Qt::UserRole).toULongLong());
    }

    void updateLayerControls()
    {
        if (layerList_ == nullptr || viewport_ == nullptr) {
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

        if (layerVisibleCheckBox_ != nullptr) {
            const QSignalBlocker blocker(layerVisibleCheckBox_);
            layerVisibleCheckBox_->setChecked(found && selectedInfo.visible);
            layerVisibleCheckBox_->setEnabled(found);
        }
        if (layerLockedCheckBox_ != nullptr) {
            const QSignalBlocker blocker(layerLockedCheckBox_);
            layerLockedCheckBox_->setChecked(found && selectedInfo.locked);
            layerLockedCheckBox_->setEnabled(found);
        }
        if (activateLayerButton_ != nullptr) {
            activateLayerButton_->setEnabled(found && !selectedInfo.active);
        }
        if (renameLayerButton_ != nullptr) {
            renameLayerButton_->setEnabled(found);
        }
        if (removeLayerButton_ != nullptr) {
            removeLayerButton_->setEnabled(found && layerList_->count() > 1);
        }
        if (moveLayerUpButton_ != nullptr) {
            moveLayerUpButton_->setEnabled(found && layerList_->currentRow() > 0);
        }
        if (moveLayerDownButton_ != nullptr) {
            moveLayerDownButton_->setEnabled(found &&
                                              layerList_->currentRow() + 1 < layerList_->count());
        }
        if (moveSelectedToLayerButton_ != nullptr) {
            moveSelectedToLayerButton_->setEnabled(found && selectedInfo.visible &&
                                                   !selectedInfo.locked);
        }
    }

    void refreshLayers()
    {
        if (layerList_ == nullptr || viewport_ == nullptr) {
            return;
        }

        refreshingLayers_ = true;
        const QVector<ViewportLayerInfo> infos = viewport_->layerInfos();
        layerList_->clear();
        int activeRow = -1;
        for (int index = 0; index < infos.size(); ++index) {
            const ViewportLayerInfo &info = infos[index];
            QString prefix;
            if (info.active) {
                prefix += QStringLiteral("◆ ");
                activeRow = index;
            }
            if (!info.visible) {
                prefix += QStringLiteral("[hidden] ");
            }
            if (info.locked) {
                prefix += QStringLiteral("[locked] ");
            }

            auto *item = new QListWidgetItem(
                QStringLiteral("%1%2  (%3 objects)")
                    .arg(prefix, info.name)
                    .arg(info.objectCount));
            item->setData(Qt::UserRole,
                          QVariant::fromValue<qulonglong>(info.id.value()));
            layerList_->addItem(item);
        }
        if (activeRow >= 0) {
            layerList_->setCurrentRow(activeRow);
        } else if (layerList_->count() > 0) {
            layerList_->setCurrentRow(0);
        }
        refreshingLayers_ = false;
        updateLayerControls();
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

    void setSelectedLayerVisible(bool visible)
    {
        if (refreshingLayers_) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetVisible;
        request.layerId = selectedLayerId();
        request.enabled = visible;
        executeLayerCommand(request,
                             visible ? QStringLiteral("Layer shown") : QStringLiteral("Layer hidden"),
                             QStringLiteral("At least one other visible, unlocked layer is required"));
    }

    void setSelectedLayerLocked(bool locked)
    {
        if (refreshingLayers_) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::SetLocked;
        request.layerId = selectedLayerId();
        request.enabled = locked;
        executeLayerCommand(request,
                             locked ? QStringLiteral("Layer locked") : QStringLiteral("Layer unlocked"),
                             QStringLiteral("At least one other visible, unlocked layer is required"));
    }

    void moveSelectedLayer(int delta)
    {
        if (layerList_ == nullptr) {
            return;
        }
        const int targetIndex = layerList_->currentRow() + delta;
        if (targetIndex < 0 || targetIndex >= layerList_->count()) {
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
        panel->setFixedWidth(286);

        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *layersBox = new QGroupBox(QStringLiteral("Layers"));
        auto *layersLayout = new QVBoxLayout(layersBox);
        layerList_ = new QListWidget;
        layerList_->setObjectName(QStringLiteral("layerList"));
        layerList_->setSelectionMode(QAbstractItemView::SingleSelection);
        layerList_->setMinimumHeight(170);
        layersLayout->addWidget(layerList_, 1);

        auto *layerOrderLayout = new QHBoxLayout;
        addLayerButton_ = new QPushButton(QStringLiteral("+"));
        addLayerButton_->setToolTip(QStringLiteral("Add layer"));
        renameLayerButton_ = new QPushButton(QStringLiteral("Rename"));
        removeLayerButton_ = new QPushButton(QStringLiteral("-"));
        removeLayerButton_->setToolTip(QStringLiteral("Remove empty layer"));
        moveLayerUpButton_ = new QPushButton(QStringLiteral("Up"));
        moveLayerDownButton_ = new QPushButton(QStringLiteral("Down"));
        layerOrderLayout->addWidget(addLayerButton_);
        layerOrderLayout->addWidget(renameLayerButton_);
        layerOrderLayout->addWidget(removeLayerButton_);
        layerOrderLayout->addWidget(moveLayerUpButton_);
        layerOrderLayout->addWidget(moveLayerDownButton_);
        layersLayout->addLayout(layerOrderLayout);

        auto *layerStateLayout = new QHBoxLayout;
        layerVisibleCheckBox_ = new QCheckBox(QStringLiteral("Visible"));
        layerLockedCheckBox_ = new QCheckBox(QStringLiteral("Locked"));
        activateLayerButton_ = new QPushButton(QStringLiteral("Set Active"));
        layerStateLayout->addWidget(layerVisibleCheckBox_);
        layerStateLayout->addWidget(layerLockedCheckBox_);
        layerStateLayout->addWidget(activateLayerButton_);
        layersLayout->addLayout(layerStateLayout);

        moveSelectedToLayerButton_ = new QPushButton(QStringLiteral("Move Selected Here"));
        layersLayout->addWidget(moveSelectedToLayerButton_);

        connect(layerList_, &QListWidget::currentRowChanged,
                this, [this](int) {
                    updateLayerControls();
                });
        connect(layerList_, &QListWidget::itemDoubleClicked,
                this, [this](QListWidgetItem *) {
                    activateSelectedLayer();
                });
        connect(addLayerButton_, &QPushButton::clicked, this, [this]() {
            addLayer();
        });
        connect(renameLayerButton_, &QPushButton::clicked, this, [this]() {
            renameSelectedLayer();
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
        connect(layerVisibleCheckBox_, &QCheckBox::toggled,
                this, [this](bool visible) {
                    setSelectedLayerVisible(visible);
                });
        connect(layerLockedCheckBox_, &QCheckBox::toggled,
                this, [this](bool locked) {
                    setSelectedLayerLocked(locked);
                });
        connect(moveSelectedToLayerButton_, &QPushButton::clicked,
                this, [this]() {
                    moveSelectedObjectsToLayer();
                });

        layout->addWidget(layersBox, 1);

        auto *propertiesBox = new QGroupBox(QStringLiteral("Properties"));
        auto *propertiesLayout = new QFormLayout(propertiesBox);
        propertiesLayout->addRow(QStringLiteral("Active tool"), new QLabel(QStringLiteral("Select")));
        propertiesLayout->addRow(QStringLiteral("Selection"), new QLabel(QStringLiteral("None")));
        propertiesLayout->addRow(QStringLiteral("Degree"), new QLabel(QStringLiteral("3")));
        propertiesLayout->addRow(QStringLiteral("Units"), new QLabel(QStringLiteral("Millimeters")));
        layout->addWidget(propertiesBox);

        auto *notes = new QLabel(QStringLiteral("NURBS geometry and command history will appear here."));
        notes->setObjectName(QStringLiteral("panelHint"));
        notes->setWordWrap(true);
        layout->addWidget(notes);

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
            QLabel#brand {
                color: #f0a45a;
                font-size: 15px;
                font-weight: bold;
                padding-right: 8px;
            }
            QLabel#modeLabel {
                color: #999999;
                padding-left: 12px;
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
    QToolButton *lineToolButton_ = nullptr;
    QToolButton *arcToolButton_ = nullptr;
    QToolButton *rectangleToolButton_ = nullptr;
    QToolButton *polygonToolButton_ = nullptr;
    QToolButton *ellipseToolButton_ = nullptr;
    QToolButton *eraseToolButton_ = nullptr;
    QToolButton *trimToolButton_ = nullptr;
    QToolButton *controlPointsButton_ = nullptr;
    QToolButton *subdivideButton_ = nullptr;
    QToolButton *joinButton_ = nullptr;
    QToolButton *explodeButton_ = nullptr;
    QToolButton *rotateToolButton_ = nullptr;
    QToolButton *mirrorToolButton_ = nullptr;
    QListWidget *layerList_ = nullptr;
    QPushButton *addLayerButton_ = nullptr;
    QPushButton *renameLayerButton_ = nullptr;
    QPushButton *removeLayerButton_ = nullptr;
    QPushButton *moveLayerUpButton_ = nullptr;
    QPushButton *moveLayerDownButton_ = nullptr;
    QPushButton *activateLayerButton_ = nullptr;
    QPushButton *moveSelectedToLayerButton_ = nullptr;
    QCheckBox *layerVisibleCheckBox_ = nullptr;
    QCheckBox *layerLockedCheckBox_ = nullptr;
    bool refreshingLayers_ = false;
    QVector<QToolButton *> toolButtons_;
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
