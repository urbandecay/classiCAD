#include "main_window.h"
#include "../app/application_session.h"
#include "../app/project_controller.h"
#include "../app/update_controller.h"
#include "viewport_widget_api.h"
#include "panels/document_grid_dialog.h"
#include "panels/layers_panel.h"
#include "panels/layer_style_widgets.h"
#include "panels/preferences_dialog.h"
#include "panels/preferences_store.h"
#include "panels/tool_shelf.h"
#include "theme/classicad_theme.h"
#include "viewport/line_type_style.h"
#include "input_helpers.h"
#include "../core/debug_log.h"
#include "../core/serialization/blender_project_file.h"
#include "../core/serialization/rhino3dm_interchange.h"

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMainWindow>
#include <QPushButton>
#include <QPixmap>
#include <QShortcut>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QWidget>
#include <QtMath>

#include <utility>

namespace classiCAD {

class MainWindow final : public QMainWindow {
public:
    MainWindow()
    {
        session_.setChangeObserver([this](ApplicationSessionChange change) {
            switch (change) {
            case ApplicationSessionChange::History:
                updateHistoryActions();
                markDocumentModified();
                break;
            case ApplicationSessionChange::Layers:
                refreshLayers();
                markDocumentModified();
                break;
            case ApplicationSessionChange::Selection:
                break;
            case ApplicationSessionChange::DocumentWillBeReplaced:
                if (viewport_ != nullptr) {
                    viewport_->prepareForDocumentReplacement();
                }
                break;
            case ApplicationSessionChange::DocumentReplaced:
                updateHistoryActions();
                refreshLayers();
                markDocumentModified();
                break;
            }
        });
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
        if (viewport_ == nullptr) {
            statusBar()->showMessage(QStringLiteral("Update session could not be restored"), 8000);
            return false;
        }

        UpdateSessionWindowState windowState;
        QString windowStateError;
        const bool hasWindowState =
            updateController_.readWindowState(path, &windowState,
                                              &windowStateError);
        if (!hasWindowState && !windowStateError.isEmpty()) {
            DebugLog::instance().write(
                QStringLiteral("restoreUpdateWindowGeometry read failed path=%1 error=%2")
                    .arg(path, windowStateError));
        }

        suppressDirtyTracking_ = true;
        const bool sessionRestored = viewport_->restoreUpdateSession(path);
        suppressDirtyTracking_ = false;
        if (!sessionRestored) {
            statusBar()->showMessage(QStringLiteral("Update session could not be restored"), 8000);
            return false;
        }

        if (controlPointsButton_ != nullptr) {
            controlPointsButton_->setChecked(viewport_->controlPointsVisible());
        }
        if (hasWindowState) {
            currentProjectPath_ = windowState.projectPath;
            documentModified_ = windowState.documentModified;
            updateWindowTitle();
            if (!windowState.windowGeometry.isEmpty() &&
                !restoreGeometry(windowState.windowGeometry)) {
                DebugLog::instance().write(
                    QStringLiteral("restoreUpdateWindowGeometry rejected path=%1")
                        .arg(path));
            }
            if (!windowState.workspaceSplitterState.isEmpty()) {
                if (QSplitter *splitter =
                        findChild<QSplitter *>(QStringLiteral("workspaceSplitter"))) {
                    splitter->restoreState(windowState.workspaceSplitterState);
                }
            }
            if (!windowState.workspaceName.isEmpty()) {
                const auto workspaceButtons =
                    findChildren<QToolButton *>(QStringLiteral("workspaceButton"));
                for (QToolButton *button : workspaceButtons) {
                    if (button->text() == windowState.workspaceName) {
                        button->setChecked(true);
                        break;
                    }
                }
            }
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
        if (!projectController_.save(path,
                                     viewport_->projectCameraSettings(),
                                     &errorMessage)) {
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
        ProjectViewportCameraSettings cameraSettings;
        suppressDirtyTracking_ = true;
        const bool loaded = viewport_ != nullptr &&
                            projectController_.open(path,
                                                    &cameraSettings,
                                                    &errorMessage);
        if (loaded) {
            viewport_->applyProjectCameraSettings(cameraSettings);
        }
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
        if (!projectController_.importRhino3dm(path, &report, &errorMessage)) {
            QMessageBox::critical(this,
                                  QStringLiteral("Could Not Import Rhino Model"),
                                  errorMessage.isEmpty()
                                      ? QStringLiteral("The Rhino model could not be imported.")
                                      : errorMessage);
            return;
        }
        viewport_->refreshAfterProjectImport();

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
        projectController_.createNewDocument();
        viewport_->refreshAfterNewDocument();
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
        if (updateController_.isRunning()) {
            statusBar()->showMessage(QStringLiteral("Update already in progress"), 3000);
            return;
        }
        if (viewport_ == nullptr) {
            statusBar()->showMessage(QStringLiteral("Update cancelled — viewport is unavailable"),
                                     8000);
            return;
        }

        UpdateSessionWindowState windowState;
        windowState.windowGeometry = saveGeometry();
        if (QSplitter *splitter =
                findChild<QSplitter *>(QStringLiteral("workspaceSplitter"))) {
            windowState.workspaceSplitterState = splitter->saveState();
        }
        windowState.projectPath = currentProjectPath_;
        windowState.documentModified = documentModified_;
        const auto workspaceButtons =
            findChildren<QToolButton *>(QStringLiteral("workspaceButton"));
        for (QToolButton *button : workspaceButtons) {
            if (button->isChecked()) {
                windowState.workspaceName = button->text();
                break;
            }
        }

        UpdateController::Callbacks callbacks;
        callbacks.status = [this](const QString &message) {
            statusBar()->showMessage(message);
        };
        callbacks.finished = [this](bool restarted, const QString &message) {
            if (!restarted && updateAction_ != nullptr) {
                updateAction_->setEnabled(true);
            }
            statusBar()->showMessage(
                restarted ? message : QStringLiteral("Update failed — %1").arg(message),
                8000);
        };
        callbacks.restarting = [this]() {
            updateRestartInProgress_ = true;
            close();
        };

        updateAction_->setEnabled(false);
        QString errorMessage;
        const bool started = updateController_.start(
            QCoreApplication::applicationFilePath(),
            QCoreApplication::applicationDirPath(),
            [this](const QString &sessionPath) {
                return viewport_ != nullptr &&
                       viewport_->saveUpdateSession(sessionPath);
            },
            windowState,
            callbacks,
            &errorMessage);
        if (!started) {
            updateAction_->setEnabled(true);
            statusBar()->showMessage(
                QStringLiteral("Update cancelled — %1").arg(errorMessage), 8000);
        }
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
        const RotateToolPreferences preferences = viewport_->rotateToolPreferences();
        const qreal snapIncrement = preferences.useRadians
                                        ? preferences.angleSnapIncrementRadiansDegrees
                                        : preferences.angleSnapIncrementDegrees;
        const QString snapIncrementLabel = preferences.useRadians
                                               ? QStringLiteral("π/%1")
                                                     .arg(qRound(180.0 / snapIncrement))
                                               : QStringLiteral("%1°")
                                                     .arg(snapIncrement, 0, 'g', 4);
        statusBar()->showMessage(
            QStringLiteral("Rotate: click pivot, reference, then end direction  •  A angle (deg)  •  C %1 snap")
                .arg(snapIncrementLabel));
    }

    void startPointExtrude()
    {
        const ViewportCommandResult result =
            viewport_ == nullptr
                ? ViewportCommandResult{}
                : viewport_->executeCommand(ViewportCommand::BeginPointExtrude);
        if (!result.accepted) {
            if (pointExtrudeToolButton_ != nullptr) {
                pointExtrudeToolButton_->setChecked(false);
            }
            if (selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
            }
            statusBar()->showMessage(
                QStringLiteral("Select editable points, curves, or planar faces before using Extrude"),
                4000);
            return;
        }

        if (pointExtrudeToolButton_ != nullptr) {
            pointExtrudeToolButton_->setChecked(true);
        }
        statusBar()->showMessage(
            QStringLiteral("Extrude: click endpoint; same offset for %1 source%2  •  X/Y/Z locks axis  •  OSnap when enabled  •  Esc cancels")
                .arg(result.count)
                .arg(result.count == 1 ? QString() : QStringLiteral("s")));
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
            statusBar()->showMessage(
                QStringLiteral("Select a rectangle or joined spline first"), 4000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Exploded into %1 separate curves")
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

        QAction *explodeAction = editMenu->addAction(QStringLiteral("Explode Curves"));
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

        QAction *pointExtrudeAction =
            editMenu->addAction(QStringLiteral("Extrude"));
        pointExtrudeAction->setShortcut(QKeySequence(Qt::Key_E));
        pointExtrudeAction->setShortcutContext(Qt::WindowShortcut);
        connect(pointExtrudeAction, &QAction::triggered, this, [this]() {
            startPointExtrude();
        });


        QAction *mirrorAction = editMenu->addAction(QStringLiteral("Mirror"));
        mirrorAction->setShortcut(QKeySequence(Qt::Key_M));
        mirrorAction->setShortcutContext(Qt::WindowShortcut);
        connect(mirrorAction, &QAction::triggered, this, [this]() {
            startMirror();
        });

        QAction *duplicateAction = editMenu->addAction(QStringLiteral("Duplicate"));
        duplicateAction->setShortcut(QKeySequence(QStringLiteral("Shift+D")));
        duplicateAction->setShortcutContext(Qt::WindowShortcut);
        connect(duplicateAction, &QAction::triggered, this, [this]() {
            startDuplicate();
        });
        QAction *duplicateInPlaceAction =
            editMenu->addAction(QStringLiteral("Duplicate in Place"));
        connect(duplicateInPlaceAction, &QAction::triggered, this, [this]() {
            duplicateInPlace();
        });

        eraseAction_ = editMenu->addAction(QStringLiteral("Erase"));
        eraseAction_->setShortcuts(QList<QKeySequence>{
            QKeySequence(QStringLiteral("E, R"))});
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
        updateAction_->setToolTip(QStringLiteral("Restart classiCAD, preserving the current scene"));
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
        addColorOption(QStringLiteral("ByLayer"), QColor(QStringLiteral("#000000")));
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

        viewport_ = createViewportWidget(session_, workspaceSplitter);
        ViewportUiCallbacks viewportCallbacks;
        viewportCallbacks.commandFinished = [this](ToolId tool) {
            if (tool == Tool::Select && selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
                statusBar()->showMessage(QStringLiteral("Select mode"));
            }
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
        joinFeedbackLabel_ = new QLabel;
        joinFeedbackLabel_->setObjectName(QStringLiteral("joinFeedback"));
        joinFeedbackLabel_->hide();
        statusBar()->addWidget(joinFeedbackLabel_);

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
            saveOrthoPreference(enabled);
            statusBar()->showMessage(enabled ? QStringLiteral("Ortho: On")
                                             : QStringLiteral("Ortho: Off"));
        });

        connect(osnapAction_, &QAction::toggled, this, [this](bool enabled) {
            osnapLane_->setVisible(enabled);
            viewport_->setOsnapEnabled(enabled);
            saveOsnapEnabledPreference(enabled);
            statusBar()->showMessage(enabled ? QStringLiteral("OSnap: On")
                                             : QStringLiteral("OSnap: Off"));
        });

        viewportCallbacks.coordinateUpdate = [this](const QString &text) {
            coordinateLabel_->setText(text);
        };
        viewportCallbacks.toolStatusUpdate = [this](const QString &message) {
            statusBar()->showMessage(message);
        };
        viewportCallbacks.subdivisionStatusUpdate = [this](const QString &message) {
            if (message.isEmpty()) {
                statusBar()->clearMessage();
            } else {
                statusBar()->showMessage(message);
            }
        };
        viewportCallbacks.joinStatusUpdate = [this](const QString &message) {
            if (message.startsWith(QStringLiteral("Joined "))) {
                const quint64 feedbackGeneration = ++joinFeedbackGeneration_;
                joinFeedbackLabel_->setText(QStringLiteral("✓ %1").arg(message));
                joinFeedbackLabel_->show();
                QTimer::singleShot(6000, joinFeedbackLabel_, [this, feedbackGeneration]() {
                    if (feedbackGeneration != joinFeedbackGeneration_) {
                        return;
                    }
                    joinFeedbackLabel_->clear();
                    joinFeedbackLabel_->hide();
                });
            } else if (message.startsWith(QStringLiteral("Join"))) {
                ++joinFeedbackGeneration_;
                joinFeedbackLabel_->clear();
                joinFeedbackLabel_->hide();
            }
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
            StoredPreferences modes;
            modes.endpointSnapEnabled = endpointSnapCheckBox_->isChecked();
            modes.midpointSnapEnabled = midpointSnapCheckBox_->isChecked();
            modes.intersectionSnapEnabled = intersectionSnapCheckBox_->isChecked();
            modes.centerSnapEnabled = centerSnapCheckBox_->isChecked();
            modes.perpendicularSnapEnabled = perpendicularSnapCheckBox_->isChecked();
            modes.tangentSnapEnabled = tangentSnapCheckBox_->isChecked();
            modes.nearSnapEnabled = nearSnapCheckBox_->isChecked();
            modes.controlPointSnapEnabled = controlPointSnapCheckBox_->isChecked();
            saveSnapModesPreference(modes);
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
        ToolShelfCallbacks callbacks;
        callbacks.toolRequested = [this](ToolId tool) {
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
            if (tool == Tool::PointExtrude) {
                startPointExtrude();
                return;
            }
            if (tool == Tool::Arc) {
                viewport_->setArcMode(ArcMode::OnePoint);
            }
            viewport_->setTool(tool);
            statusBar()->showMessage(
                QStringLiteral("Active tool: %1").arg(toolName(tool)));
        };
        callbacks.configureToolMenu = [this](ToolId tool, QToolButton *button) {
            switch (tool) {
            case Tool::Point: createPointToolMenu(button); break;
            case Tool::Line: createLineToolMenu(button); break;
            case Tool::Arc: createArcToolMenu(button); break;
            case Tool::Bezier: createCurveToolMenu(button); break;
            case Tool::Rectangle: createRectangleToolMenu(button); break;
            case Tool::PolygonCenterCorner: createPolygonToolMenu(button); break;
            case Tool::Circle: createCircleToolMenu(button); break;
            case Tool::Ellipse: createEllipseToolMenu(button); break;
            case Tool::LinearDimension: createDimensionToolMenu(button); break;
            case Tool::Scale: createScaleToolMenu(button); break;
            default: break;
            }
        };
        callbacks.duplicateRequested = [this]() { startDuplicate(); };
        callbacks.controlPointsVisibilityChanged = [this](bool visible) {
            viewport_->setControlPointsVisible(visible);
            saveControlPointsPreference(visible);
            statusBar()->showMessage(visible ? QStringLiteral("Control points: On")
                                             : QStringLiteral("Control points: Off"));
        };
        callbacks.subdivideRequested = [this]() {
            subdivideWithNumberOfPoints();
        };
        callbacks.joinRequested = [this]() { startJoinMode(); };
        callbacks.explodeRequested = [this]() { explodeSelectedShapes(); };

        toolShelf_ = new ToolShelf(std::move(callbacks));
        selectToolButton_ = toolShelf_->button(Tool::Select);
        pointToolButton_ = toolShelf_->button(Tool::Point);
        lineToolButton_ = toolShelf_->button(Tool::Line);
        arcToolButton_ = toolShelf_->button(Tool::Arc);
        bezierToolButton_ = toolShelf_->button(Tool::Bezier);
        rectangleToolButton_ = toolShelf_->button(Tool::Rectangle);
        polygonToolButton_ = toolShelf_->button(Tool::PolygonCenterCorner);
        circleToolButton_ = toolShelf_->button(Tool::Circle);
        ellipseToolButton_ = toolShelf_->button(Tool::Ellipse);
        dimensionToolButton_ = toolShelf_->button(Tool::LinearDimension);
        eraseToolButton_ = toolShelf_->button(Tool::Erase);
        trimToolButton_ = toolShelf_->button(Tool::Trim);
        pointExtrudeToolButton_ = toolShelf_->button(Tool::PointExtrude);
        rotateToolButton_ = toolShelf_->button(Tool::Rotate);
        mirrorToolButton_ = toolShelf_->button(Tool::Mirror);
        scaleToolButton_ = toolShelf_->button(Tool::Scale);
        controlPointsButton_ = toolShelf_->controlPointsButton();
        updateToolHelp();
        return toolShelf_;
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
        const StoredPreferences stored = loadStoredPreferences();
        const PreferencesDialogValues &preferences = stored.dialogValues;

        applyPanButton(preferences.panButton, false);
        viewport_->setSnapLabelsVisible(preferences.snapLabelsVisible);
        viewport_->setSmoothCurveDisplay(preferences.smoothCurveDisplay);
        viewport_->setGridAppearance(preferences.gridAppearance);
        if (!viewport_->setCameraPreferences(preferences.cameraPreferences)) {
            viewport_->setCameraPreferences(ViewportCameraPreferences{});
        }
        viewport_->setNavigationPreferences(preferences.navigationPreferences);
        if (!viewport_->setRotateToolPreferences(preferences.rotateToolPreferences)) {
            viewport_->setRotateToolPreferences(RotateToolPreferences{});
        }
        viewport_->setViewportAntiAliasingSamples(preferences.viewportAaSamples);
        viewport_->setSmoothWirePreferences(preferences.smoothWiresOverlay,
                                            preferences.smoothWiresEditMode);
        viewport_->setArchitecturalDimensionFont(
            preferences.architecturalDimensionFont);

        if (orthoAction_ != nullptr) {
            orthoAction_->setChecked(stored.orthoEnabled);
        }
        if (endpointSnapCheckBox_ != nullptr) {
            endpointSnapCheckBox_->setChecked(stored.endpointSnapEnabled);
            midpointSnapCheckBox_->setChecked(stored.midpointSnapEnabled);
            intersectionSnapCheckBox_->setChecked(stored.intersectionSnapEnabled);
            centerSnapCheckBox_->setChecked(stored.centerSnapEnabled);
            perpendicularSnapCheckBox_->setChecked(stored.perpendicularSnapEnabled);
            tangentSnapCheckBox_->setChecked(stored.tangentSnapEnabled);
            nearSnapCheckBox_->setChecked(stored.nearSnapEnabled);
            controlPointSnapCheckBox_->setChecked(stored.controlPointSnapEnabled);
        }
        if (osnapAction_ != nullptr) {
            osnapAction_->setChecked(stored.osnapEnabled);
        }
        if (controlPointsButton_ != nullptr) {
            controlPointsButton_->setChecked(stored.controlPointsVisible);
        }
    }



    void openPreferences()
    {
        const StoredPreferences stored = loadStoredPreferences();
        PreferencesDialogValues initialValues = stored.dialogValues;
        initialValues.panButton = viewport_->panButton();
        initialValues.gridAppearance = viewport_->gridAppearance();
        initialValues.cameraPreferences = viewport_->cameraPreferences();
        initialValues.navigationPreferences = viewport_->navigationPreferences();
        initialValues.rotateToolPreferences = viewport_->rotateToolPreferences();
        initialValues.viewportAaSamples = viewport_->viewportAntiAliasingSamples();
        initialValues.smoothWiresOverlay = viewport_->smoothWiresOverlay();
        initialValues.smoothWiresEditMode = viewport_->smoothWiresEditMode();

        PreferencesDialogValues selectedValues;
        if (!showPreferencesDialog(this, initialValues, &selectedValues)) {
            return;
        }
        applyPanButton(selectedValues.panButton, true);
        applySnapLabelsVisible(selectedValues.snapLabelsVisible, true);
        applySmoothCurveDisplay(selectedValues.smoothCurveDisplay, true);
        applyArchitecturalDimensionFont(selectedValues.architecturalDimensionFont, true);
        applyGridAppearance(selectedValues.gridAppearance, true);
        applyCameraPreferences(selectedValues.cameraPreferences, true);
        applyNavigationPreferences(selectedValues.navigationPreferences, true);
        applyRotateToolPreferences(selectedValues.rotateToolPreferences, true);
        applyViewportAntiAliasingSamples(selectedValues.viewportAaSamples, true);
        applySmoothWirePreferences(selectedValues.smoothWiresOverlay,
                                   selectedValues.smoothWiresEditMode, true);
    }

    void openDocumentGridSettings()
    {
        if (viewport_ == nullptr) {
            return;
        }
        DocumentSettings selectedSettings;
        if (!showDocumentGridDialog(this,
                                    viewport_->documentSettings(),
                                    &selectedSettings)) {
            return;
        }
        if (!viewport_->setDocumentSettings(selectedSettings)) {
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
            saveArchitecturalDimensionFontPreference(enabled);
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
            saveSmoothCurveDisplayPreference(enabled);
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
            saveGridAppearancePreference(appearance);
        }
    }

    void applyCameraPreferences(const ViewportCameraPreferences &preferences, bool save)
    {
        if (viewport_ == nullptr || !viewport_->setCameraPreferences(preferences)) {
            return;
        }
        if (save) {
            saveCameraPreferences(preferences);
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
            saveNavigationPreferences(preferences);
        }
    }

    void applyRotateToolPreferences(
        const RotateToolPreferences &preferences,
        bool save)
    {
        if (viewport_ == nullptr ||
            !viewport_->setRotateToolPreferences(preferences)) {
            return;
        }
        if (save) {
            saveRotateToolPreferences(preferences);
        }
    }

    void applyViewportAntiAliasingSamples(int samples, bool save)
    {
        if (viewport_ == nullptr) {
            return;
        }
        viewport_->setViewportAntiAliasingSamples(samples);
        if (save) {
            saveViewportAaSamplesPreference(samples);
        }
    }

    void applySmoothWirePreferences(bool overlay, bool editMode, bool save)
    {
        if (viewport_ == nullptr) return;
        viewport_->setSmoothWirePreferences(overlay, editMode);
        if (save) saveSmoothWirePreferences(overlay, editMode);
    }

    void applySnapLabelsVisible(bool visible, bool save)
    {
        viewport_->setSnapLabelsVisible(visible);
        if (save) {
            saveSnapLabelsPreference(visible);
            statusBar()->showMessage(visible ? QStringLiteral("Snap type labels: On")
                                             : QStringLiteral("Snap type labels: Off"));
        }
    }

    void applyPanButton(Qt::MouseButton button, bool save)
    {
        viewport_->setPanButton(button);
        updateToolHelp();

        if (save) {
            savePanButtonPreference(button);
            statusBar()->showMessage(
                button == Qt::RightButton
                    ? QStringLiteral("Right-click selects; right-drag pans; Shift+right-drag orbits.")
                    : QStringLiteral("Middle mouse orbits; Shift+middle mouse pans."));
        }
    }

    void updateToolHelp()
    {
        if (toolShelf_ != nullptr && viewport_ != nullptr) {
            const QString alternatePan = viewport_->panButton() == Qt::RightButton
                                             ? QStringLiteral("\n\nRMB click\nSelect\n\nRMB drag\nAlternate pan")
                                             : QString();
            toolShelf_->setHelpText(QStringLiteral(
                                        "LMB\nDraw\n\nMMB\nOrbit\n\nShift+MMB\nPan%1\n\nWheel\nZoom")
                                        .arg(alternatePan));
        }
    }

    LayerId selectedLayerId() const
    {
        return layersPanel_ == nullptr ? LayerId::invalid()
                                       : layersPanel_->selectedLayerId();
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

    void refreshLayers()
    {
        if (layersPanel_ != nullptr) {
            layersPanel_->refresh();
        }
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

    void renameLayer(LayerId layerId)
    {
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

    void removeLayer(LayerId layerId)
    {
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Remove;
        request.layerId = layerId;
        executeLayerCommand(request,
                             QStringLiteral("Layer removed"),
                             QStringLiteral("Only empty layers can be removed"));
    }

    void activateLayer(LayerId layerId)
    {
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Activate;
        request.layerId = layerId;
        executeLayerCommand(request,
                             QStringLiteral("Active layer changed"),
                             QStringLiteral("A layer must be visible and unlocked to become active"));
    }

    void setLayerVisible(LayerId layerId, bool visible)
    {
        if (!layerId.isValid()) {
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
        if (!layerId.isValid()) {
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
        if (!layerId.isValid()) {
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
        if (!layerId.isValid()) {
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
        if (!layerId.isValid()) {
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
        if (!layerId.isValid()) {
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

    void editLayerDescription(LayerId layerId)
    {
        if (!layerId.isValid() || viewport_ == nullptr) {
            return;
        }
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

    void chooseLayerColor(LayerId layerId)
    {
        if (!layerId.isValid() || viewport_ == nullptr) {
            return;
        }
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

    void moveLayer(LayerId layerId, int targetIndex)
    {
        if (!layerId.isValid() || targetIndex < 0) {
            return;
        }
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::Move;
        request.layerId = layerId;
        request.index = targetIndex;
        executeLayerCommand(request,
                             QStringLiteral("Layer order updated"),
                             QStringLiteral("Could not reorder layer"));
    }

    void moveSelectedObjectsToLayer(LayerId layerId)
    {
        ViewportLayerCommandRequest request;
        request.command = ViewportLayerCommand::MoveSelectedObjects;
        request.layerId = layerId;
        const ViewportLayerCommandResult result = viewport_->executeLayerCommand(request);
        if (!result.accepted) {
            statusBar()->showMessage(QStringLiteral("Select editable objects and a visible, unlocked target layer"),
                                     5000);
            return;
        }
        statusBar()->showMessage(QStringLiteral("Moved %1 object%2 to %3")
                                     .arg(result.count)
                                     .arg(result.count == 1 ? QString() : QStringLiteral("s"))
                                     .arg(layerId.isValid()
                                              ? QStringLiteral("the selected layer")
                                              : QStringLiteral("the layer")),
                                 4000);
        refreshLayers();
    }

    QWidget *createRightPanel()
    {
        LayersPanelCallbacks callbacks;
        callbacks.layerInfos = [this]() {
            return viewport_ == nullptr ? QVector<ViewportLayerInfo>()
                                        : viewport_->layerInfos();
        };
        callbacks.addLayer = [this]() { addLayer(); };
        callbacks.removeLayer = [this](LayerId id) { removeLayer(id); };
        callbacks.renameLayer = [this](LayerId id) { renameLayer(id); };
        callbacks.activateLayer = [this](LayerId id) { activateLayer(id); };
        callbacks.setVisible = [this](LayerId id, bool value) {
            setLayerVisible(id, value);
        };
        callbacks.setFrozen = [this](LayerId id, bool value) {
            setLayerFrozen(id, value);
        };
        callbacks.setLocked = [this](LayerId id, bool value) {
            setLayerLocked(id, value);
        };
        callbacks.setPlotted = [this](LayerId id, bool value) {
            setLayerPlotted(id, value);
        };
        callbacks.chooseColor = [this](LayerId id) { chooseLayerColor(id); };
        callbacks.editDescription = [this](LayerId id) {
            editLayerDescription(id);
        };
        callbacks.setLineType = [this](LayerId id, const QString &lineType) {
            setLayerLineType(id, lineType);
        };
        callbacks.setLineWeight = [this](LayerId id, qreal lineWeightMm) {
            setLayerLineWeight(id, lineWeightMm);
        };
        callbacks.moveLayer = [this](LayerId id, int index) {
            moveLayer(id, index);
        };
        callbacks.moveSelectedObjects = [this](LayerId id) {
            moveSelectedObjectsToLayer(id);
        };
        callbacks.layersRefreshed = [this](const QVector<ViewportLayerInfo> &infos) {
            refreshLayerToolbar(infos);
        };

        layersPanel_ = new LayersPanel(callbacks);
        return layersPanel_;
    }

    void applyTheme()
    {
        setStyleSheet(classicadThemeStyleSheet());
    }

    ApplicationSession session_;
    ProjectController projectController_{session_};
    UpdateController updateController_;
    ViewportWidgetApi *viewport_ = nullptr;
    ToolShelf *toolShelf_ = nullptr;
    QLabel *coordinateLabel_ = nullptr;
    QLabel *joinFeedbackLabel_ = nullptr;
    quint64 joinFeedbackGeneration_ = 0;
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
    QToolButton *rotateToolButton_ = nullptr;
    QToolButton *pointExtrudeToolButton_ = nullptr;
    QToolButton *mirrorToolButton_ = nullptr;
    QToolButton *scaleToolButton_ = nullptr;
    ScaleMode scaleMode_ = ScaleMode::TwoD;
    QComboBox *currentLayerCombo_ = nullptr;
    QActionGroup *workPlaneActionGroup_ = nullptr;
    QComboBox *layerColorCombo_ = nullptr;
    QComboBox *layerLineTypeCombo_ = nullptr;
    QComboBox *layerLineWeightCombo_ = nullptr;
    LayersPanel *layersPanel_ = nullptr;
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
};

int runApplication(QApplication &application, const QString &updateSessionPath)
{
    MainWindow window;
    window.show();
    if (!updateSessionPath.isEmpty()) {
        window.restoreUpdateSession(updateSessionPath);
    }
    return application.exec();
}

} // namespace classiCAD
