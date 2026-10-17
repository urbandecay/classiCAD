#include "main_window.h"
#include "../app/application_session.h"
#include "../app/project_controller.h"
#include "../app/update_controller.h"
#include "viewport_widget_api.h"
#include "panels/document_grid_dialog.h"
#include "panels/collections_panel.h"
#include "panels/preferences_dialog.h"
#include "panels/preferences_store.h"
#include "panels/tool_shelf.h"
#include "theme/classicad_theme.h"
#include "input_helpers.h"
#include "../core/debug_log.h"
#include "../core/serialization/blender_project_file.h"
#include "../core/serialization/rhino3dm_interchange.h"

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
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
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTabWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QWidget>
#include <QtMath>

#include <array>
#include <algorithm>
#include <cmath>
#include <utility>

namespace classiCAD {

namespace {

QJsonObject activeImportedBlenderScene(const BlenderSceneImportReport &report)
{
    for (const QJsonValue &sceneValue : report.scenes) {
        const QJsonObject scene = sceneValue.toObject();
        if (scene.value(QStringLiteral("scene_id")).toString() ==
            report.activeSceneId) {
            return scene;
        }
    }
    return report.scenes.isEmpty() ? QJsonObject{} : report.scenes.first().toObject();
}

} // namespace

class MainWindow final : public QMainWindow {
public:
    MainWindow()
    {
        session_.setChangeObserver([this](ApplicationSessionChange change) {
            switch (change) {
            case ApplicationSessionChange::History:
                updateHistoryActions();
                refreshCollections();
                if (viewport_ != nullptr) viewport_->update();
                markDocumentModified();
                break;
            case ApplicationSessionChange::Collections:
                refreshCollections();
                if (viewport_ != nullptr) viewport_->update();
                markDocumentModified();
                break;
            case ApplicationSessionChange::Selection:
                if (!session_.document().settings().activeViewLayerKey.isEmpty()) {
                    SavedViewLayerSelection &saved = viewLayerSelections_[
                        session_.document().settings().activeViewLayerKey];
                    saved.objectIds = session_.selection().objectIds();
                    saved.activeObjectId =
                        session_.selection().activeObjectId();
                }
                if (collectionsPanel_ != nullptr) {
                    collectionsPanel_->syncSelection();
                }
                if (viewport_ != nullptr) viewport_->update();
                break;
            case ApplicationSessionChange::DocumentWillBeReplaced:
                if (viewport_ != nullptr) {
                    viewport_->prepareForDocumentReplacement();
                }
                break;
            case ApplicationSessionChange::DocumentReplaced:
                activeViewLayerByScene_.clear();
                viewLayerSelections_.clear();
                updateHistoryActions();
                refreshCollections();
                markDocumentModified();
                break;
            }
        });
        setWindowTitle(QStringLiteral("Untitled — Vignola"));
        resize(1440, 900);
        setMinimumSize(980, 620);

        createMenus();
        createWorkspaceBar();
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
            const QSignalBlocker blocker(controlPointsButton_);
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

    void saveWorkbenchRenderResult()
    {
        if (viewport_ == nullptr) {
            return;
        }
        const QSize viewportSize = viewport_->viewportPixelSize();
        QSize sceneOutputSize;
        const QString activeLayerKey =
            session_.document().settings().activeViewLayerKey;
        for (const QJsonValue &sceneValue :
             session_.document().settings().importedBlenderScenes) {
            const QJsonObject scene = sceneValue.toObject();
            bool activeScene = activeLayerKey.isEmpty();
            for (const QJsonValue &layerValue :
                 scene.value(QStringLiteral("view_layers")).toArray()) {
                if (layerValue.toObject().value(QStringLiteral("key")).toString() ==
                    activeLayerKey) {
                    activeScene = true;
                    break;
                }
            }
            if (!activeScene) continue;
            const QJsonObject renderOutput =
                scene.value(QStringLiteral("render_output")).toObject();
            const int resolutionX = renderOutput.value(
                QStringLiteral("resolution_x")).toInt();
            const int resolutionY = renderOutput.value(
                QStringLiteral("resolution_y")).toInt();
            const int percentage = renderOutput.value(
                QStringLiteral("resolution_percentage")).toInt();
            if (resolutionX > 0 && resolutionY > 0 &&
                percentage > 0 && percentage <= 32767) {
                const qint64 outputWidth =
                    qint64(resolutionX) * percentage / 100;
                const qint64 outputHeight =
                    qint64(resolutionY) * percentage / 100;
                if (outputWidth > 0 && outputHeight > 0 &&
                    outputWidth <= 8192 && outputHeight <= 8192) {
                    sceneOutputSize = QSize(int(outputWidth), int(outputHeight));
                }
            }
            break;
        }
        const QString viewportLabel =
            QStringLiteral("Viewport (%1 × %2)")
                .arg(viewportSize.width()).arg(viewportSize.height());
        const QString sceneLabel = sceneOutputSize.isValid()
            ? QStringLiteral("Blender Scene Output (%1 × %2)")
                  .arg(sceneOutputSize.width()).arg(sceneOutputSize.height())
            : QString();
        QStringList labels{viewportLabel};
        if (!sceneLabel.isEmpty()) labels.append(sceneLabel);
        labels.append({QStringLiteral("1280 × 720 (HD)"),
                       QStringLiteral("1920 × 1080 (Full HD)"),
                       QStringLiteral("2560 × 1440 (QHD)"),
                       QStringLiteral("3840 × 2160 (4K)"),
                       QStringLiteral("Custom…")});
        bool accepted = false;
        const QString choice = QInputDialog::getItem(
            this, QStringLiteral("Render Result Resolution"),
            QStringLiteral("Output size"), labels,
            sceneLabel.isEmpty() ? 0 : 1, false, &accepted);
        if (!accepted) {
            return;
        }
        QSize outputSize = viewportSize;
        if (choice == sceneLabel && sceneOutputSize.isValid()) {
            outputSize = sceneOutputSize;
        } else if (choice == QStringLiteral("1280 × 720 (HD)")) {
            outputSize = QSize(1280, 720);
        } else if (choice == QStringLiteral("1920 × 1080 (Full HD)")) {
            outputSize = QSize(1920, 1080);
        } else if (choice == QStringLiteral("2560 × 1440 (QHD)")) {
            outputSize = QSize(2560, 1440);
        } else if (choice == QStringLiteral("3840 × 2160 (4K)")) {
            outputSize = QSize(3840, 2160);
        } else if (choice == QStringLiteral("Custom…")) {
            QSettings settings;
            bool widthAccepted = false;
            const int width = QInputDialog::getInt(
                this, QStringLiteral("Render Output Settings"),
                QStringLiteral("Resolution X (pixels)"),
                settings.value(QStringLiteral("render/workbenchWidth"),
                               viewportSize.width()).toInt(),
                64, 8192, 1, &widthAccepted);
            if (!widthAccepted) return;
            bool heightAccepted = false;
            const int height = QInputDialog::getInt(
                this, QStringLiteral("Render Output Settings"),
                QStringLiteral("Resolution Y (pixels)"),
                settings.value(QStringLiteral("render/workbenchHeight"),
                               viewportSize.height()).toInt(),
                64, 8192, 1, &heightAccepted);
            if (!heightAccepted) return;
            outputSize = QSize(width, height);
            settings.setValue(QStringLiteral("render/workbenchWidth"), width);
            settings.setValue(QStringLiteral("render/workbenchHeight"), height);
        }
        const WorkbenchRenderResult result =
            viewport_->renderWorkbenchResult(outputSize);
        if (!result.isValid()) {
            QMessageBox::warning(
                this, QStringLiteral("Render Result Unavailable"),
                QStringLiteral("The offscreen scene render failed."));
            return;
        }
        lastWorkbenchRenderResult_ = result;
        showWorkbenchRenderResult(lastWorkbenchRenderResult_);
    }

    void showLastWorkbenchRenderResult()
    {
        if (!lastWorkbenchRenderResult_.isValid()) {
            QMessageBox::information(
                this, QStringLiteral("No Render Result"),
                QStringLiteral("Render a Workbench result first."));
            return;
        }
        showWorkbenchRenderResult(lastWorkbenchRenderResult_);
    }

    void showWorkbenchRenderResult(const WorkbenchRenderResult &result)
    {
        const QSize outputSize = result.outputSize.isEmpty()
                                     ? result.combined.size()
                                     : result.outputSize;
        const QString combinedFormatError =
            result.combinedImageFormatError();
        QDialog window(this);
        window.setWindowTitle(QStringLiteral("Last Workbench Render Result — %1 × %2")
                                  .arg(outputSize.width()).arg(outputSize.height()));
        window.resize(1000, 760);
        auto *layout = new QVBoxLayout(&window);
        const QString combinedDescription = result.combinedPassAvailable &&
                                                   combinedFormatError.isEmpty()
            ? QStringLiteral("%1 × %2 .%3 (%4, %5-bit output)")
                  .arg(outputSize.width()).arg(outputSize.height())
                  .arg(result.combinedFileExtension()).arg(result.colorMode)
                  .arg(result.writtenColorDepth())
            : result.combinedPassAvailable
                  ? QStringLiteral("format unavailable: %1")
                        .arg(combinedFormatError)
            : QStringLiteral("disabled in Blender view layer");
        auto *info = new QLabel(
            QStringLiteral("Last completed result (not live)   •   Combined: %1   •   Depth: %2")
                .arg(combinedDescription)
                .arg(result.depthPassAvailable
                         ? QStringLiteral("Z depth PFM")
                         : QStringLiteral("disabled in Blender view layer")),
            &window);
        layout->addWidget(info);
        auto *preview = new QLabel(&window);
        preview->setAlignment(Qt::AlignCenter);
        preview->setMinimumSize(640, 480);
        if (result.combinedPassAvailable) {
            preview->setPixmap(QPixmap::fromImage(result.combined).scaled(
                960, 680, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            preview->setText(QStringLiteral(
                "Combined pass disabled in the active Blender view layer."));
        }
        layout->addWidget(preview, 1);
        auto *buttons = new QHBoxLayout;
        auto *saveCombined = new QPushButton(
            combinedFormatError.isEmpty()
                ? QStringLiteral("Save Combined %1…")
                      .arg(result.combinedFileExtension().toUpper())
                : QStringLiteral("Save Combined (Unavailable)…"), &window);
        saveCombined->setEnabled(result.combinedPassAvailable &&
                                 combinedFormatError.isEmpty());
        if (!combinedFormatError.isEmpty())
            saveCombined->setToolTip(combinedFormatError);
        auto *saveDepth = new QPushButton(QStringLiteral("Save Depth PFM…"), &window);
        saveDepth->setEnabled(result.hasDepthPass());
        auto *close = new QPushButton(QStringLiteral("Close"), &window);
        buttons->addWidget(saveCombined);
        buttons->addWidget(saveDepth);
        buttons->addStretch();
        buttons->addWidget(close);
        layout->addLayout(buttons);
        connect(saveCombined, &QPushButton::clicked, &window, [this, &result]() {
            const QString extension = result.combinedFileExtension();
            const QString initialPath = workbenchCombinedInitialSavePath(
                result.blenderRenderFilepath, extension,
                result.useFileExtension);
            QString path = QFileDialog::getSaveFileName(
                this, QStringLiteral("Save Combined Render"),
                initialPath,
                result.useFileExtension
                    ? QStringLiteral("Image Files (*.%1)").arg(extension)
                    : QStringLiteral("All Files (*)"));
            if (path.isEmpty()) return;
            path = workbenchCombinedSavePath(path, extension,
                                             result.useFileExtension);
            QString error;
            if (!result.saveCombinedImage(path, &error))
                QMessageBox::critical(this, QStringLiteral("Save Failed"), error);
        });
        connect(saveDepth, &QPushButton::clicked, &window, [this, &result]() {
            QString path = QFileDialog::getSaveFileName(
                this, QStringLiteral("Save Depth Pass"),
                QStringLiteral("WorkbenchRender_Depth.pfm"),
                QStringLiteral("Portable Float Map (*.pfm)"));
            if (path.isEmpty()) return;
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".pfm");
            QString error;
            if (!result.saveDepthPfm(path, &error)) QMessageBox::critical(this, QStringLiteral("Save Failed"), error);
        });
        connect(close, &QPushButton::clicked, &window, &QDialog::accept);
        window.exec();
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
        BlenderSceneImportReport ordinaryImportReport;
        suppressDirtyTracking_ = true;
        const bool loaded = viewport_ != nullptr &&
                            projectController_.open(path,
                                                    &cameraSettings,
                                                    &errorMessage,
                                                    &ordinaryImportReport);
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

        const bool importedOrdinaryBlend =
            !ordinaryImportReport.blenderVersion.isEmpty();
        if (importedOrdinaryBlend && !ordinaryImportReport.scenes.isEmpty()) {
            const QJsonObject scene = activeImportedBlenderScene(ordinaryImportReport);
            QJsonObject shading = scene.value(
                QStringLiteral("workbench_viewport_shading")).toObject();
            if (shading.isEmpty()) {
                // Older extractors and headless imports may not have a saved
                // VIEW_3D area; retain their scene-level shading fallback.
                shading = scene.value(
                    QStringLiteral("workbench_shading")).toObject();
            }
            if (!shading.isEmpty()) {
                const QStringList unsupported =
                    viewport_->applyImportedBlenderShading(shading);
                if (!unsupported.isEmpty()) {
                    const QString summary = QStringLiteral(
                        "Solid viewport settings not reproduced: %1")
                        .arg(unsupported.join(QStringLiteral(", ")));
                    ordinaryImportReport.warningMessage =
                        ordinaryImportReport.warningMessage.isEmpty()
                            ? summary
                            : ordinaryImportReport.warningMessage +
                                  QStringLiteral("; ") + summary;
                }
            }
            session_.selection().setObjectIds(
                ordinaryImportReport.selectedObjectIds,
                ordinaryImportReport.activeObjectId);
            session_.selection().setActiveObjectId(
                ordinaryImportReport.activeObjectId);
        }
        // Opening a normal Blender file imports its supported scene data into
        // a new classiCAD document. Never let Ctrl+S replace that source file.
        currentProjectPath_ = importedOrdinaryBlend
                                  ? QString{}
                                  : QFileInfo(path).absoluteFilePath();
        documentModified_ = false;
        QSettings settings;
        settings.setValue(QStringLiteral("files/lastProjectDirectory"),
                          QFileInfo(path).absolutePath());
        updateWindowTitle();
        statusBar()->showMessage(
            importedOrdinaryBlend
                ? QStringLiteral("Imported Blender scene from %1; use Save As to create a classiCAD project")
                      .arg(QFileInfo(path).fileName())
                : QStringLiteral("Opened %1").arg(currentProjectPath_),
            8000);
        if (!ordinaryImportReport.blenderVersion.isEmpty() &&
            (ordinaryImportReport.skippedObjectCount > 0 ||
             !ordinaryImportReport.warningMessage.isEmpty() ||
             ordinaryImportReport.importedObjectCount == 0)) {
            QMessageBox::warning(
                this,
                QStringLiteral("Blender Scene Open Report"),
                QStringLiteral("Loaded %1 supported object(s); skipped %2 object(s) from Blender %3.\n\n%4")
                    .arg(ordinaryImportReport.importedObjectCount)
                    .arg(ordinaryImportReport.skippedObjectCount)
                    .arg(ordinaryImportReport.blenderVersion,
                         ordinaryImportReport.warningMessage.isEmpty()
                             ? QStringLiteral("No supported objects were found.")
                             : ordinaryImportReport.warningMessage));
        }
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

    void importBlenderScene()
    {
        QString initialDirectory;
        if (!currentProjectPath_.isEmpty()) {
            initialDirectory = QFileInfo(currentProjectPath_).absolutePath();
        } else {
            QSettings settings;
            initialDirectory = settings.value(QStringLiteral("files/lastProjectDirectory"),
                                              QDir::homePath()).toString();
        }
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Import Blender Scene"),
            initialDirectory,
            QStringLiteral("Blender Scene (*.blend)"));
        if (path.isEmpty()) {
            return;
        }

        const QStringList modes{
            QStringLiteral("Original mesh (do not apply modifiers)"),
            QStringLiteral("Evaluated mesh (bake evaluated geometry)")};
        bool modeAccepted = false;
        const QString selectedMode = QInputDialog::getItem(
            this,
            QStringLiteral("Blender Mesh Import Mode"),
            QStringLiteral("Choose which mesh geometry to import:"),
            modes,
            0,
            false,
            &modeAccepted);
        if (!modeAccepted) {
            return;
        }
        const QString importMode = selectedMode.startsWith(QStringLiteral("Evaluated"))
            ? QStringLiteral("evaluated") : QStringLiteral("original");

        QVector<SceneObject> importedObjects;
        QVector<DocumentCollection> importedCollections;
        QVector<DocumentMaterial> importedMaterials;
        BlenderSceneImportReport report;
        QString errorMessage;
        if (!classiCAD::importBlenderScene(path, importMode, &importedObjects,
                                           &importedCollections,
                                           &importedMaterials,
                                           &report, &errorMessage)) {
            QMessageBox::critical(
                this,
                QStringLiteral("Could Not Import Blender Scene"),
                errorMessage.isEmpty()
                    ? QStringLiteral("The Blender scene could not be imported.")
                    : errorMessage);
            return;
        }

        if (!importedObjects.isEmpty() || !importedCollections.isEmpty() ||
            !importedMaterials.isEmpty() || !report.scenes.isEmpty()) {
            DocumentTransaction transaction = session_.beginTransaction();
            QHash<quint64, CollectionId> collectionRemap;
            QVector<DocumentCollection> pendingCollections = importedCollections;
            while (!pendingCollections.isEmpty()) {
                QVector<DocumentCollection> deferred;
                bool insertedAny = false;
                for (DocumentCollection collection : pendingCollections) {
                    const bool parentsReady = std::all_of(
                        collection.parentIds.cbegin(),
                        collection.parentIds.cend(),
                        [&collectionRemap](CollectionId parentId) {
                            return collectionRemap.contains(parentId.value());
                        });
                    if (!parentsReady) {
                        deferred.append(std::move(collection));
                        continue;
                    }
                    for (CollectionId &parentId : collection.parentIds) {
                        const auto parent = collectionRemap.constFind(parentId.value());
                        if (parent == collectionRemap.cend()) {
                            continue;
                        }
                        parentId = parent.value();
                    }
                    const quint64 sourceValue = collection.id.value();
                    collection.id = CollectionId::invalid();
                    const CollectionId inserted =
                        transaction.insertCollection(collection);
                    if (!inserted.isValid()) {
                        QMessageBox::critical(
                            this,
                            QStringLiteral("Could Not Import Blender Scene"),
                            QStringLiteral("A Blender collection could not be added to the document."));
                        return;
                    }
                    collectionRemap.insert(sourceValue, inserted);
                    insertedAny = true;
                }
                if (!insertedAny) {
                    QMessageBox::critical(
                        this,
                        QStringLiteral("Could Not Import Blender Scene"),
                        QStringLiteral("The Blender collection hierarchy contains an unresolved parent."));
                    return;
                }
                pendingCollections = std::move(deferred);
            }
            for (SceneObject &object : importedObjects) {
                for (CollectionId &collectionId : object.collectionIds) {
                    const auto mapped = collectionRemap.constFind(collectionId.value());
                    if (mapped == collectionRemap.cend()) {
                        QMessageBox::critical(
                            this,
                            QStringLiteral("Could Not Import Blender Scene"),
                            QStringLiteral("An imported mesh refers to an unresolved Blender collection."));
                        return;
                    }
                    collectionId = mapped.value();
                }
            }
            for (const DocumentMaterial &material : importedMaterials) {
                if (!transaction.addMaterial(material)) {
                    QMessageBox::critical(
                        this,
                        QStringLiteral("Could Not Import Blender Scene"),
                        QStringLiteral("A Blender material identity conflicts with an existing material."));
                    return;
                }
            }
            DocumentSettings settings = session_.document().settings();
            qsizetype colorLutBytes = 0;
            for (auto it = settings.importedBlenderColorLuts.cbegin();
                 it != settings.importedBlenderColorLuts.cend(); ++it) {
                colorLutBytes += it.value().size();
            }
            QStringList omittedColorLutIds;
            QStringList conflictingColorLutIds;
            for (auto it = report.importedBlenderColorLuts.cbegin();
                 it != report.importedBlenderColorLuts.cend(); ++it) {
                if (!settings.importedBlenderColorLuts.contains(it.key())) {
                    if (settings.importedBlenderColorLuts.size() >= 8 ||
                        colorLutBytes + it.value().size() > 32 * 1024 * 1024) {
                        omittedColorLutIds.append(it.key());
                        continue;
                    }
                    settings.importedBlenderColorLuts.insert(it.key(), it.value());
                    settings.importedBlenderColorLutMetadata.insert(
                        it.key(), report.importedBlenderColorLutMetadata.value(it.key()));
                    colorLutBytes += it.value().size();
                } else if (settings.importedBlenderColorLutMetadata.value(it.key()) !=
                           report.importedBlenderColorLutMetadata.value(it.key())) {
                    conflictingColorLutIds.append(it.key());
                }
            }
            if (!omittedColorLutIds.isEmpty()) {
                report.warningMessage += QStringLiteral("\nSome imported scene LUTs were omitted because the document limit is 8 LUTs or 32 MiB: %1")
                                             .arg(omittedColorLutIds.join(QStringLiteral(", ")));
            }
            if (!conflictingColorLutIds.isEmpty()) {
                report.warningMessage += QStringLiteral("\nSome imported LUT references reused an existing payload ID with different domain/shaper metadata; existing document metadata was retained: %1")
                                             .arg(conflictingColorLutIds.join(QStringLiteral(", ")));
            }
            for (const QJsonValue &sceneValue : report.scenes) {
                const QJsonObject importedScene = sceneValue.toObject();
                const QString sceneId = importedScene.value(
                    QStringLiteral("scene_id")).toString();
                bool replaced = false;
                for (int index = 0;
                     index < settings.importedBlenderScenes.size(); ++index) {
                    if (settings.importedBlenderScenes[index].toObject().value(
                            QStringLiteral("scene_id")).toString() == sceneId) {
                        settings.importedBlenderScenes.replace(index, importedScene);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) settings.importedBlenderScenes.append(importedScene);
            }
            if (!report.activeViewLayerKey.isEmpty()) {
                settings.activeViewLayerKey = report.activeViewLayerKey;
            } else if (settings.activeViewLayerKey.isEmpty()) {
                for (const SceneObject &object : importedObjects) {
                    if (!object.defaultViewLayerKey.isEmpty()) {
                        settings.activeViewLayerKey = object.defaultViewLayerKey;
                        break;
                    }
                }
            }
            transaction.setSettings(settings);
            const QVector<ObjectId> insertedObjectIds =
                transaction.insertObjects(session_.document().size(),
                                          importedObjects);
            if (!session_.commitTransaction(transaction)) {
                QMessageBox::critical(
                    this,
                    QStringLiteral("Could Not Import Blender Scene"),
                    QStringLiteral("The imported Blender objects could not be added to the document."));
                return;
            }
            QVector<ObjectId> selectedImportedObjectIds;
            for (const int index : report.selectedObjectIndices) {
                if (index >= 0 && index < insertedObjectIds.size()) {
                    selectedImportedObjectIds.append(insertedObjectIds[index]);
                }
            }
            ObjectId activeImportedObjectId = ObjectId::invalid();
            if (report.activeObjectIndex >= 0 &&
                report.activeObjectIndex < insertedObjectIds.size()) {
                activeImportedObjectId =
                    insertedObjectIds[report.activeObjectIndex];
            }
            if (!selectedImportedObjectIds.isEmpty()) {
                session_.selection().setObjectIds(selectedImportedObjectIds,
                                                  activeImportedObjectId);
            }
            if (activeImportedObjectId.isValid()) {
                session_.selection().setActiveObjectId(activeImportedObjectId);
            }
            if (viewport_ != nullptr) {
                viewport_->refreshAfterProjectImport();
                if (!report.scenes.isEmpty()) {
                    const QJsonObject scene = activeImportedBlenderScene(report);
                    QJsonObject shading = scene.value(
                        QStringLiteral("workbench_viewport_shading")).toObject();
                    if (shading.isEmpty()) {
                        // Keep compatibility with older extractors and files
                        // that do not contain a saved VIEW_3D area.
                        shading = scene.value(
                            QStringLiteral("workbench_shading")).toObject();
                    }
                    if (!shading.isEmpty()) {
                        const QStringList unsupported =
                            viewport_->applyImportedBlenderShading(shading);
                        if (!unsupported.isEmpty()) {
                            const QString summary = QStringLiteral(
                                "Solid viewport settings not reproduced: %1")
                                .arg(unsupported.join(QStringLiteral(", ")));
                            report.warningMessage = report.warningMessage.isEmpty()
                                ? summary
                                : report.warningMessage + QStringLiteral("; ") +
                                      summary;
                        }
                    }
                }
            }
        }

        QSettings settings;
        settings.setValue(QStringLiteral("files/lastProjectDirectory"),
                          QFileInfo(path).absolutePath());
        const QString status = QStringLiteral("Imported %1 object(s) from %2 using Blender %3 (%4 mode)")
                                   .arg(report.importedObjectCount)
                                   .arg(QFileInfo(path).fileName(),
                                        report.blenderVersion,
                                        report.importMode);
        statusBar()->showMessage(status, 7000);
        if (!report.warningMessage.isEmpty() || report.importedObjectCount == 0) {
            QMessageBox::warning(
                this,
                QStringLiteral("Blender Scene Import Report"),
                QStringLiteral("Imported %1 object(s); skipped %2 object(s).\n\n%3")
                    .arg(report.importedObjectCount)
                    .arg(report.skippedObjectCount)
                    .arg(report.warningMessage.isEmpty()
                             ? QStringLiteral("No supported objects were found.")
                             : report.warningMessage));
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

        auto *weldShortcut =
            new QShortcut(QKeySequence(QStringLiteral("W")), viewport_);
        weldShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(weldShortcut, &QShortcut::activated, this, [this]() {
            startWeld();
        });
    }

    void startWeld()
    {
        if (viewport_ == nullptr) {
            return;
        }
        const ViewportCommandResult result =
            viewport_->executeCommand(ViewportCommand::Weld);
        if (result.accepted) {
            statusBar()->showMessage(
                QStringLiteral("Welded %1 curve crossing%2")
                    .arg(result.count)
                    .arg(result.count == 1 ? QString() : QStringLiteral("s")),
                5000);
        }
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
            QStringLiteral("Duplicate: move the copy, X/Y/Z constrains an axis; click or Enter confirms"));
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
        QAction *importBlenderSceneAction = importMenu->addAction(
            QStringLiteral("Blender Scene…"));
        connect(importBlenderSceneAction, &QAction::triggered, this, [this]() {
            importBlenderScene();
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
        QAction *renderResultAction = fileMenu->addAction(
            QStringLiteral("Render Workbench Result…"));
        renderResultAction->setShortcut(QKeySequence(Qt::Key_F12));
        connect(renderResultAction, &QAction::triggered,
                this, [this]() { saveWorkbenchRenderResult(); });
        QAction *showLastRenderResultAction = fileMenu->addAction(
            QStringLiteral("Show Last Workbench Render Result"));
        connect(showLastRenderResultAction, &QAction::triggered,
                this, [this]() { showLastWorkbenchRenderResult(); });
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
            if (lineAutoWeldCheckBox_ != nullptr) {
                lineAutoWeldCheckBox_->hide();
            }
            if (tool == Tool::Select && selectToolButton_ != nullptr) {
                selectToolButton_->setChecked(true);
                statusBar()->showMessage(QStringLiteral("Select mode"));
            }
        };
        viewportCallbacks.viewportControlsChanged =
            [this](int interactionMode, int,
                   bool xrayEnabled, bool wireframeEnabled) {
                if (interactionModeComboBox_ != nullptr) {
                    const int index = interactionModeComboBox_->findData(
                        interactionMode);
                    if (index >= 0) {
                        const QSignalBlocker blocker(interactionModeComboBox_);
                        interactionModeComboBox_->setCurrentIndex(index);
                    }
                }
                if (componentModeButtonGroup_ != nullptr) {
                    for (QAbstractButton *button :
                         componentModeButtonGroup_->buttons()) {
                        button->setVisible(interactionMode == 1);
                        button->setEnabled(
                            interactionMode == 1 && viewport_ != nullptr &&
                            viewport_->supportsComponentSelectionMode(
                                componentModeButtonGroup_->id(button)));
                    }
                }
                if (controlPointsButton_ != nullptr) {
                    controlPointsButton_->setEnabled(interactionMode == 1);
                    const QSignalBlocker blocker(controlPointsButton_);
                    controlPointsButton_->setChecked(
                        interactionMode == 1 &&
                        viewport_->controlPointsVisible());
                }
                if (componentModeButtonGroup_ != nullptr) {
                    if (viewport_ != nullptr) {
                        const int activeModes =
                            viewport_->componentSelectionModesMask();
                        for (QAbstractButton *button :
                             componentModeButtonGroup_->buttons()) {
                            const int mode =
                                componentModeButtonGroup_->id(button);
                            button->setChecked(
                                mode >= 0 && mode < 3 &&
                                (activeModes & (1 << mode)) != 0);
                        }
                    }
                }
                if (xrayToggleButton_ != nullptr) {
                    xrayToggleButton_->setChecked(xrayEnabled);
                }
                if (shadingModeButtonGroup_ != nullptr) {
                    const int shadingModeId = wireframeEnabled ? 1 : 2;
                    if (QAbstractButton *button =
                            shadingModeButtonGroup_->button(shadingModeId)) {
                        button->setChecked(true);
                    }
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
        viewportCallbacks.viewportControlsChanged(
            viewport_->interactionMode(),
            viewport_->componentSelectionMode(),
            viewport_->viewportXrayEnabled(),
            viewport_->viewportWireframeEnabled());
        viewportCallbacks.viewStateUpdate(viewport_->workPlane(),
                                          viewport_->workPlaneOffset(),
                                          viewport_->viewPreset());
        refreshCollections();
        updateHistoryActions();
    }

    void createOsnapLane()
    {
        osnapLane_ = new QToolBar(QStringLiteral("Object Snaps"), this);
        osnapLane_->setObjectName(QStringLiteral("osnapLane"));
        osnapLane_->setMovable(false);
        osnapLane_->setFloatable(false);
        osnapLane_->setToolButtonStyle(Qt::ToolButtonIconOnly);

        interactionModeComboBox_ = new QComboBox(osnapLane_);
        interactionModeComboBox_->setObjectName(
            QStringLiteral("viewportInteractionMode"));
        interactionModeComboBox_->addItem(QStringLiteral("Object Mode"), 0);
        interactionModeComboBox_->addItem(QStringLiteral("Edit Mode"), 1);
        interactionModeComboBox_->setToolTip(
            QStringLiteral("Switch between Object Mode and Edit Mode (Tab)"));
        osnapLane_->addWidget(interactionModeComboBox_);
        connect(interactionModeComboBox_,
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int index) {
                    if (index >= 0 && interactionModeComboBox_ != nullptr) {
                        viewport_->setInteractionMode(
                            interactionModeComboBox_->currentData().toInt());
                    }
                });
        osnapLane_->addSeparator();

        componentModeButtonGroup_ = new QButtonGroup(this);
        componentModeButtonGroup_->setExclusive(false);
        const std::array<QString, 3> componentModeIcons{
            QStringLiteral(":/blender-shading/selection_vertex.png"),
            QStringLiteral(":/blender-shading/selection_edge.png"),
            QStringLiteral(":/blender-shading/selection_face.png")};
        const std::array<QString, 3> componentModeTips{
            QStringLiteral("Vertex Select (1; Shift+1 toggles curve multi-select)"),
            QStringLiteral("Edge Select (2; Shift+2 toggles curve multi-select)"),
            QStringLiteral("Face Select (3)")};
        for (int index = 0; index < 3; ++index) {
            auto *button = new QToolButton(osnapLane_);
            button->setObjectName(QStringLiteral("viewportControlButton"));
            button->setIcon(QIcon(componentModeIcons[index]));
            button->setIconSize(QSize(16, 16));
            button->setFixedSize(20, 20);
            button->setCheckable(true);
            button->setToolTip(componentModeTips[index]);
            componentModeButtonGroup_->addButton(button, index);
            osnapLane_->addWidget(button);
            connect(button, &QToolButton::clicked, this,
                    [this, button, index]() {
                if (button->isChecked()) {
                    if (QApplication::keyboardModifiers().testFlag(
                            Qt::ShiftModifier)) {
                        viewport_->toggleComponentSelectionMode(index);
                    } else {
                        viewport_->setComponentSelectionMode(index);
                    }
                } else {
                    viewport_->toggleComponentSelectionMode(index);
                }
            });
        }
        osnapLane_->addSeparator();

        // Balance the snap controls between the component buttons on the left
        // and viewport shading controls on the right.
        auto *leftCenteringSpacer = new QWidget(osnapLane_);
        leftCenteringSpacer->setSizePolicy(QSizePolicy::Expanding,
                                           QSizePolicy::Preferred);
        osnapLane_->addWidget(leftCenteringSpacer);

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

        lineAutoWeldCheckBox_ = new QCheckBox(QStringLiteral("Weld"));
        lineAutoWeldCheckBox_->setObjectName(QStringLiteral("lineAutoWeldHudCheckBox"));
        lineAutoWeldCheckBox_->setToolTip(QStringLiteral(
            "Automatically weld planar intersections when finishing a line"));
        lineAutoWeldCheckBox_->setParent(viewport_);
        lineAutoWeldCheckBox_->setFixedSize(68, 20);
        lineAutoWeldCheckBox_->installEventFilter(this);
        viewport_->installEventFilter(this);
        positionLineAutoWeldCheckBox();
        lineAutoWeldCheckBox_->hide();
        connect(lineAutoWeldCheckBox_, &QCheckBox::toggled, this,
                [this](bool enabled) {
                    viewport_->setLineAutoWeldEnabled(enabled);
                    saveLineAutoWeldPreference(enabled);
                });

        auto *spacer = new QWidget(osnapLane_);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        osnapLane_->addWidget(spacer);
        osnapLane_->addSeparator();

        const auto makeViewportButton = [this](const QString &iconPath,
                                               const QString &toolTip,
                                               bool checkable = true) {
            auto *button = new QToolButton(osnapLane_);
            button->setObjectName(QStringLiteral("viewportControlButton"));
            if (!iconPath.isEmpty()) {
                button->setIcon(QIcon(iconPath));
            }
            button->setIconSize(QSize(16, 16));
            button->setFixedSize(20, 20);
            button->setCheckable(checkable);
            button->setToolTip(toolTip);
            return button;
        };

        xrayToggleButton_ = makeViewportButton(
            QStringLiteral(":/blender-shading/xray.png"),
            QStringLiteral("X-Ray (Alt+Z)"));
        osnapLane_->addWidget(xrayToggleButton_);
        connect(xrayToggleButton_, &QToolButton::clicked, this, [this]() {
            viewport_->activateViewportShadingControl(0);
        });

        shadingModeButtonGroup_ = new QButtonGroup(this);
        shadingModeButtonGroup_->setExclusive(true);
        auto *wireframeButton = makeViewportButton(
            QStringLiteral(":/blender-shading/shading_wire.png"),
            QStringLiteral("Wireframe"));
        auto *solidButton = makeViewportButton(
            QStringLiteral(":/blender-shading/shading_solid.png"),
            QStringLiteral("Solid"));
        shadingModeButtonGroup_->addButton(wireframeButton, 1);
        shadingModeButtonGroup_->addButton(solidButton, 2);
        osnapLane_->addWidget(wireframeButton);
        osnapLane_->addWidget(solidButton);
        connect(wireframeButton, &QToolButton::clicked, this, [this]() {
            viewport_->activateViewportShadingControl(1);
        });
        connect(solidButton, &QToolButton::clicked, this, [this]() {
            viewport_->activateViewportShadingControl(2);
        });

        auto *settingsButton = makeViewportButton(
            QString(), QStringLiteral("Viewport Shading Settings"), false);
        settingsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        settingsButton->setText(QString::fromUtf8("⌄"));
        osnapLane_->addWidget(settingsButton);
        connect(settingsButton, &QToolButton::clicked, this,
                [this, settingsButton]() {
                    viewport_->showViewportShadingSettings(
                        QRect(settingsButton->mapToGlobal(QPoint(0, 0)),
                              settingsButton->size()));
                });

        addToolBar(Qt::BottomToolBarArea, osnapLane_);
    }

    QWidget *createToolShelf()
    {
        ToolShelfCallbacks callbacks;
        callbacks.toolRequested = [this](ToolId tool) {
            if (lineAutoWeldCheckBox_ != nullptr) {
                lineAutoWeldCheckBox_->setVisible(tool == Tool::Line);
                positionLineAutoWeldCheckBox();
            }
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
        callbacks.weldRequested = [this]() { startWeld(); };
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

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == viewport_ && event->type() == QEvent::Resize) {
            positionLineAutoWeldCheckBox();
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void positionLineAutoWeldCheckBox()
    {
        if (viewport_ == nullptr || lineAutoWeldCheckBox_ == nullptr) {
            return;
        }
        constexpr int panelLeft = 12;
        constexpr int panelWidth = 750;
        const int x = std::max(panelLeft,
                               std::min(panelLeft + panelWidth - 76,
                                        viewport_->width() - 12 -
                                            lineAutoWeldCheckBox_->width()));
        lineAutoWeldCheckBox_->move(x, viewport_->height() - 37);
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

        viewport_->setControlPointsVisible(stored.controlPointsVisible);

        applyPanButton(preferences.panButton, false);
        viewport_->setSnapLabelsVisible(preferences.snapLabelsVisible);
        viewport_->setSmoothCurveDisplay(preferences.smoothCurveDisplay);
        viewport_->setGridAppearance(preferences.gridAppearance);
        viewport_->setThemeBackground(preferences.themeBackground);
        if (!viewport_->setCameraPreferences(preferences.cameraPreferences)) {
            viewport_->setCameraPreferences(ViewportCameraPreferences{});
        }
        viewport_->setNavigationPreferences(preferences.navigationPreferences);
        if (!viewport_->setRotateToolPreferences(preferences.rotateToolPreferences)) {
            viewport_->setRotateToolPreferences(RotateToolPreferences{});
        }
        viewport_->setViewportAntiAliasingMode(preferences.viewportAaMode);
        viewport_->setSmoothWirePreferences(preferences.smoothWiresOverlay,
                                            preferences.smoothWiresEditMode);
        viewport_->setThemeOutlineWidth(preferences.themeOutlineWidth);
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
            const QSignalBlocker blocker(controlPointsButton_);
            controlPointsButton_->setChecked(stored.controlPointsVisible);
        }
        if (lineAutoWeldCheckBox_ != nullptr) {
            const QSignalBlocker blocker(lineAutoWeldCheckBox_);
            lineAutoWeldCheckBox_->setChecked(stored.lineAutoWeldEnabled);
        }
        viewport_->setLineAutoWeldEnabled(stored.lineAutoWeldEnabled);
    }



    void openPreferences()
    {
        const StoredPreferences stored = loadStoredPreferences();
        PreferencesDialogValues initialValues = stored.dialogValues;
        initialValues.panButton = viewport_->panButton();
        initialValues.gridAppearance = viewport_->gridAppearance();
        initialValues.themeBackground = viewport_->themeBackground();
        initialValues.cameraPreferences = viewport_->cameraPreferences();
        initialValues.navigationPreferences = viewport_->navigationPreferences();
        initialValues.rotateToolPreferences = viewport_->rotateToolPreferences();
        initialValues.viewportAaMode = viewport_->viewportAntiAliasingMode();
        initialValues.smoothWiresOverlay = viewport_->smoothWiresOverlay();
        initialValues.themeOutlineWidth = viewport_->themeOutlineWidth();
        // The saved checkbox value may differ from the active value until
        // restart. Keep showing the requested value while previewing only
        // settings that take effect live.
        const bool activeSmoothWiresEditMode = viewport_->smoothWiresEditMode();

        PreferencesDialogValues selectedValues;
        const auto previewViewportQuality =
            [this, activeSmoothWiresEditMode](int mode, bool overlay, bool) {
                if (viewport_ == nullptr) {
                    return;
                }
                viewport_->setViewportAntiAliasingMode(mode);
                viewport_->setSmoothWirePreferences(overlay,
                                                    activeSmoothWiresEditMode);
            };
        if (!showPreferencesDialog(this, initialValues, &selectedValues,
                                   previewViewportQuality)) {
            // Preview changes are temporary until OK. Cancel returns the
            // active viewport to the values it had when the dialog opened.
            viewport_->setViewportAntiAliasingMode(initialValues.viewportAaMode);
            viewport_->setSmoothWirePreferences(
                initialValues.smoothWiresOverlay,
                activeSmoothWiresEditMode);
            return;
        }
        applyPanButton(selectedValues.panButton, true);
        applySnapLabelsVisible(selectedValues.snapLabelsVisible, true);
        applySmoothCurveDisplay(selectedValues.smoothCurveDisplay, true);
        applyArchitecturalDimensionFont(selectedValues.architecturalDimensionFont, true);
        applyGridAppearance(selectedValues.gridAppearance, true);
        applyThemeBackground(selectedValues.themeBackground, true);
        applyCameraPreferences(selectedValues.cameraPreferences, true);
        applyNavigationPreferences(selectedValues.navigationPreferences, true);
        applyRotateToolPreferences(selectedValues.rotateToolPreferences, true);
        applyViewportAntiAliasingMode(selectedValues.viewportAaMode, true);
        applySmoothWirePreferences(selectedValues.smoothWiresOverlay,
                                   selectedValues.smoothWiresEditMode, true);
        applyThemeOutlineWidth(selectedValues.themeOutlineWidth, true);
        if (selectedValues.smoothWiresEditMode != activeSmoothWiresEditMode) {
            statusBar()->showMessage(
                QStringLiteral("Edit Mode Smooth Wires will take effect after restarting classiCAD."),
                10000);
        }
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

    void applyThemeBackground(const ViewportThemeBackground &background,
                              bool save)
    {
        if (viewport_ == nullptr ||
            !isValidViewportThemeBackground(background)) {
            return;
        }
        viewport_->setThemeBackground(background);
        if (save) saveThemeBackgroundPreference(background);
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

    void applyViewportAntiAliasingMode(int mode, bool save)
    {
        if (viewport_ == nullptr) {
            return;
        }
        viewport_->setViewportAntiAliasingMode(mode);
        if (save) {
            saveViewportAaModePreference(mode);
        }
    }

    void applySmoothWirePreferences(bool overlay, bool editMode, bool save)
    {
        if (viewport_ == nullptr) return;
        viewport_->setSmoothWirePreferences(overlay,
                                            viewport_->smoothWiresEditMode());
        if (save) saveSmoothWirePreferences(overlay, editMode);
    }

    void applyThemeOutlineWidth(qreal width, bool save)
    {
        if (viewport_ == nullptr || !std::isfinite(width) || width < 0.0 ||
            width > 10.0) {
            return;
        }
        viewport_->setThemeOutlineWidth(width);
        if (save) {
            saveThemeOutlineWidthPreference(width);
        }
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

    void refreshCollections()
    {
        if (collectionsPanel_ != nullptr) {
            collectionsPanel_->refresh();
        }
    }

    void pruneOutlinerSelection()
    {
        SelectionModel &selection = session_.selection();
        selection.prune(session_.document());
        if (!selection.contains(selection.activeObjectId())) {
            selection.setActiveObjectId(selection.primaryObjectId());
        }
    }

    QWidget *createRightPanel()
    {
        CollectionsPanelCallbacks collectionCallbacks;
        const auto activeSceneRecord = [this]() {
            const DocumentSettings &settings = session_.document().settings();
            for (const QJsonValue &sceneValue : settings.importedBlenderScenes) {
                const QJsonObject scene = sceneValue.toObject();
                for (const QJsonValue &layerValue : scene.value(
                         QStringLiteral("view_layers")).toArray()) {
                    if (layerValue.toObject().value(QStringLiteral("key"))
                            .toString() == settings.activeViewLayerKey) {
                        return scene;
                    }
                }
            }
            for (const QJsonValue &sceneValue : settings.importedBlenderScenes) {
                const QJsonObject scene = sceneValue.toObject();
                if (scene.value(QStringLiteral("is_active_scene")).toBool()) {
                    return scene;
                }
            }
            return settings.importedBlenderScenes.isEmpty()
                ? QJsonObject{}
                : settings.importedBlenderScenes.first().toObject();
        };
        const auto collectionBelongsToActiveScene =
            [activeSceneRecord](const DocumentCollection &collection) {
                const QString sceneName = activeSceneRecord().value(
                    QStringLiteral("name")).toString();
                return sceneName.isEmpty() ||
                    collection.sourceSceneNames.isEmpty() ||
                    collection.sourceSceneNames.contains(sceneName);
            };
        collectionCallbacks.entries = [this]() {
            QVector<CollectionPanelEntry> entries;
            const Document &document = session_.document();
            entries.reserve(document.collections().size());
            for (const DocumentCollection &collection : document.collections()) {
                entries.append({collection});
            }
            return entries;
        };
        collectionCallbacks.objects = [this]() {
            QVector<OutlinerObjectEntry> objects;
            const Document &document = session_.document();
            objects.reserve(document.objects().size());
            QHash<QString, QString> materialNamesById;
            for (const DocumentMaterial &material : document.materials()) {
                materialNamesById.insert(material.id, material.name);
            }
            QHash<QString, QStringList> sceneKeysBySourceObjectId;
            QHash<QString, QString> sceneKeyByViewLayer;
            QHash<QString, QString> sceneKeyByName;
            for (const QJsonValue &sceneValue :
                 document.settings().importedBlenderScenes) {
                const QJsonObject scene = sceneValue.toObject();
                const QString sceneKey = scene.value(
                    QStringLiteral("scene_id")).toString();
                const QString sceneName = scene.value(
                    QStringLiteral("name")).toString();
                if (sceneKey.isEmpty()) continue;
                sceneKeyByName.insert(sceneName, sceneKey);
                for (const QJsonValue &objectIdValue : scene.value(
                         QStringLiteral("object_ids")).toArray()) {
                    if (objectIdValue.isString()) {
                        sceneKeysBySourceObjectId[
                            objectIdValue.toString()].append(sceneKey);
                    }
                }
                for (const QJsonValue &layerValue : scene.value(
                         QStringLiteral("view_layers")).toArray()) {
                    const QString key = layerValue.toObject().value(
                        QStringLiteral("key")).toString();
                    if (!key.isEmpty()) sceneKeyByViewLayer.insert(key, sceneKey);
                }
            }
            QHash<quint64, QStringList> sceneKeysByCollectionId;
            for (const DocumentCollection &collection : document.collections()) {
                for (const QString &sceneName : collection.sourceSceneNames) {
                    const QString sceneKey = sceneKeyByName.value(sceneName);
                    if (!sceneKey.isEmpty()) {
                        sceneKeysByCollectionId[collection.id.value()]
                            .append(sceneKey);
                    }
                }
            }
            QHash<QString, ObjectId> objectIdsByIdentity;
            for (const SceneObject &object : document.objects()) {
                if (object.blenderLooseMeshEdgeCompanion) continue;
                const QString identity = object.blenderSourceObjectId.isEmpty()
                    ? QStringLiteral("classiCAD-local-object:%1")
                          .arg(object.id.value())
                    : object.blenderSourceObjectId;
                objectIdsByIdentity.insert(identity, object.id);
            }
            const auto toOutlinerAnimation = [](
                const BlenderSourceAnimationData &source) {
                OutlinerAnimationEntry animation;
                animation.hasAnimationData = source.hasAnimationData;
                animation.actionName = source.actionName;
                animation.actionSlotName = source.actionSlotName;
                animation.hasDrivers = source.hasDrivers;
                animation.driverTargetNames = source.driverTargetNames;
                for (const BlenderSourceNlaTrack &track : source.nlaTracks) {
                    animation.nlaTracks.append({track.name, track.actionNames});
                }
                return animation;
            };
            for (const SceneObject &object : document.objects()) {
                if (object.blenderLooseMeshEdgeCompanion) continue;
                QString name = object.displayName.trimmed();
                if (name.isEmpty()) {
                    name = QStringLiteral("Object %1").arg(object.id.value());
                }
                const ObjectId parentObjectId = objectIdsByIdentity.value(
                    object.blenderParentObjectId, ObjectId::invalid());
                OutlinerObjectEntry entry;
                entry.id = object.id;
                entry.name = name;
                entry.blenderType = object.blenderSourceObjectType;
                entry.dataBlockName = object.blenderSourceDataName;
                entry.shapeKeyDataName =
                    object.blenderSourceShapeKeyDataName;
                entry.shapeKeyNames = object.blenderSourceShapeKeyNames;
                entry.dataMaterialNames =
                    object.blenderSourceDataMaterialNames;
                for (qsizetype index = 0;
                     index < entry.dataMaterialNames.size(); ++index) {
                    OutlinerMaterialEntry material;
                    material.name = entry.dataMaterialNames[index];
                    if (index < object.blenderSourceDataMaterialAnimations.size()) {
                        material.animation = toOutlinerAnimation(
                            object.blenderSourceDataMaterialAnimations[index]);
                    }
                    entry.dataMaterialEntries.append(std::move(material));
                }
                entry.dataHasAnimationData =
                    object.blenderSourceDataAnimation.hasAnimationData;
                entry.dataAnimationActionName =
                    object.blenderSourceDataAnimation.actionName;
                entry.dataAnimationActionSlotName =
                    object.blenderSourceDataAnimation.actionSlotName;
                entry.dataHasDrivers =
                    object.blenderSourceDataAnimation.hasDrivers;
                entry.dataDriverTargetNames =
                    object.blenderSourceDataAnimation.driverTargetNames;
                for (const BlenderSourceNlaTrack &track :
                     object.blenderSourceDataAnimation.nlaTracks) {
                    entry.dataNlaTracks.append({track.name, track.actionNames});
                }
                entry.modifierNames = object.blenderSourceModifierNames;
                for (const BlenderSourceModifier &modifier :
                     object.blenderSourceModifiers) {
                    entry.modifiers.append({modifier.name, modifier.linkedName,
                                            modifier.linkedType});
                }
                entry.constraintNames = object.blenderSourceConstraintNames;
                entry.vertexGroupNames = object.blenderSourceVertexGroupNames;
                entry.hasAnimationData = object.blenderSourceHasAnimationData;
                entry.animationActionName =
                    object.blenderSourceAnimationActionName;
                entry.animationActionSlotName =
                    object.blenderSourceAnimationActionSlotName;
                entry.hasDrivers = object.blenderSourceHasDrivers;
                entry.driverTargetNames =
                    object.blenderSourceDriverTargetNames;
                for (const BlenderSourceNlaTrack &track :
                     object.blenderSourceNlaTracks) {
                    entry.nlaTracks.append({track.name, track.actionNames});
                }
                entry.sceneKeys = sceneKeysBySourceObjectId.value(
                    object.blenderSourceObjectId);
                for (const CollectionId collectionId : object.collectionIds) {
                    const QStringList sceneKeys =
                        sceneKeysByCollectionId.value(collectionId.value());
                    for (const QString &sceneKey : sceneKeys) {
                        if (!entry.sceneKeys.contains(sceneKey)) {
                            entry.sceneKeys.append(sceneKey);
                        }
                    }
                }
                const QString defaultSceneKey = sceneKeyByViewLayer.value(
                    object.defaultViewLayerKey);
                if (!defaultSceneKey.isEmpty() &&
                    !entry.sceneKeys.contains(defaultSceneKey)) {
                    entry.sceneKeys.append(defaultSceneKey);
                }
                entry.materialNames = object.blenderSourceMaterialNames;
                if (entry.materialNames.isEmpty() &&
                    object.geometry.geometryType == GeometryType::PolygonMesh) {
                    const PolygonMesh3D &mesh = object.geometry.polygonMesh;
                    if (!mesh.materialSlotIds.isEmpty()) {
                        for (const QString &materialId : mesh.materialSlotIds) {
                            const QString materialName =
                                materialNamesById.value(materialId);
                            if (!materialName.isEmpty()) {
                                entry.materialNames.append(materialName);
                            }
                        }
                    } else {
                        for (const PolygonMeshMaterialSlot &material :
                             mesh.materialSlots) {
                            if (!material.name.isEmpty()) {
                                entry.materialNames.append(material.name);
                            }
                        }
                    }
                }
                for (qsizetype index = 0;
                     index < entry.materialNames.size(); ++index) {
                    OutlinerMaterialEntry material;
                    material.name = entry.materialNames[index];
                    if (index < object.blenderSourceMaterialAnimations.size()) {
                        material.animation = toOutlinerAnimation(
                            object.blenderSourceMaterialAnimations[index]);
                    }
                    entry.materialEntries.append(std::move(material));
                }
                entry.collectionIds = object.collectionIds;
                entry.hideViewport = object.hideViewport;
                entry.hideRender = object.hideRender;
                entry.hideSelect = object.hideSelect;
                entry.visible = document.isObjectVisible(object.id);
                entry.selectable = document.isObjectEditable(object.id);
                entry.viewLayerHidden = object.viewLayerHidden;
                entry.parentObjectId = parentObjectId;
                objects.append(std::move(entry));
            }
            return objects;
        };
        collectionCallbacks.scenes = [this]() {
            QVector<OutlinerSceneEntry> scenes;
            for (const QJsonValue &sceneValue :
                 session_.document().settings().importedBlenderScenes) {
                const QJsonObject scene = sceneValue.toObject();
                const QString key = scene.value(
                    QStringLiteral("scene_id")).toString();
                const QString name = scene.value(
                    QStringLiteral("name")).toString();
                if (!key.isEmpty() && !name.isEmpty()) {
                    OutlinerSceneEntry entry;
                    entry.key = key;
                    entry.displayName = name;
                    for (const QJsonValue &layerValue : scene.value(
                             QStringLiteral("view_layers")).toArray()) {
                        const QJsonObject layer = layerValue.toObject();
                        const QString layerKey = layer.value(
                            QStringLiteral("key")).toString();
                        const QString layerName = layer.value(
                            QStringLiteral("name")).toString();
                        if (layerKey.isEmpty() || layerName.isEmpty()) continue;
                        entry.viewLayers.append({layerKey, layerName});
                        if (layer.value(
                                QStringLiteral("is_active_view_layer")).toBool()) {
                            entry.activeViewLayerKey = layerKey;
                        }
                    }
                    if (entry.activeViewLayerKey.isEmpty() &&
                        !entry.viewLayers.isEmpty()) {
                        entry.activeViewLayerKey = entry.viewLayers.first().key;
                    }
                    const QString savedLayerKey =
                        activeViewLayerByScene_.value(key);
                    const auto savedLayer = std::find_if(
                        entry.viewLayers.cbegin(), entry.viewLayers.cend(),
                        [&savedLayerKey](const OutlinerViewLayerEntry &layer) {
                            return layer.key == savedLayerKey;
                        });
                    if (savedLayer != entry.viewLayers.cend()) {
                        entry.activeViewLayerKey = savedLayer->key;
                    }
                    scenes.append(std::move(entry));
                }
            }
            return scenes;
        };
        collectionCallbacks.activeSceneKey = [activeSceneRecord]() {
            return activeSceneRecord().value(
                QStringLiteral("scene_id")).toString();
        };
        collectionCallbacks.activeViewLayerKey = [this]() {
            return session_.document().settings().activeViewLayerKey;
        };
        collectionCallbacks.viewLayers = [activeSceneRecord]() {
            QVector<OutlinerViewLayerEntry> viewLayers;
            const QJsonObject scene = activeSceneRecord();
            const QString sceneName = scene.value(
                QStringLiteral("name")).toString();
            for (const QJsonValue &layerValue : scene.value(
                     QStringLiteral("view_layers")).toArray()) {
                const QJsonObject layer = layerValue.toObject();
                const QString key = layer.value(
                    QStringLiteral("key")).toString();
                const QString name = layer.value(
                    QStringLiteral("name")).toString();
                if (key.isEmpty() || name.isEmpty()) continue;
                viewLayers.append({key, QStringLiteral("%1 / %2")
                                            .arg(sceneName, name)});
            }
            return viewLayers;
        };
        collectionCallbacks.stateScopeKey = [this]() {
            return currentProjectPath_;
        };
        const auto outlinerStateSceneKey = [activeSceneRecord]() {
            const QString sceneId = activeSceneRecord().value(
                QStringLiteral("scene_id")).toString();
            return sceneId.isEmpty() ? QStringLiteral("classiCAD-default-scene")
                                     : sceneId;
        };
        collectionCallbacks.treeStateContextKey = outlinerStateSceneKey;
        collectionCallbacks.loadExpandedTreePaths =
            [this, outlinerStateSceneKey](QStringList *paths) {
                if (paths == nullptr) return false;
                const auto &states = session_.document()
                                        .outlinerExpandedPathsByScene();
                const auto saved = states.constFind(outlinerStateSceneKey());
                if (saved == states.cend()) return false;
                *paths = saved.value();
                return true;
            };
        collectionCallbacks.saveExpandedTreePaths =
            [this, outlinerStateSceneKey](const QStringList &paths) {
                Document &document = session_.document();
                QHash<QString, QStringList> states =
                    document.outlinerExpandedPathsByScene();
                const QString key = outlinerStateSceneKey();
                if (states.value(key) == paths && states.contains(key)) return;
                states.insert(key, paths);
                if (document.setOutlinerExpandedPathsByScene(states)) {
                    markDocumentModified();
                }
            };
        collectionCallbacks.setActiveViewLayer = [this](const QString &key) {
            if (key.isEmpty() || key ==
                    session_.document().settings().activeViewLayerKey) {
                return;
            }
            const QString previousKey =
                session_.document().settings().activeViewLayerKey;
            if (!previousKey.isEmpty()) {
                SavedViewLayerSelection &saved =
                    viewLayerSelections_[previousKey];
                saved.objectIds = session_.selection().objectIds();
                saved.activeObjectId = session_.selection().activeObjectId();
            }
            DocumentSettings settings = session_.document().settings();
            settings.activeViewLayerKey = key;
            DocumentTransaction transaction = session_.beginTransaction();
            if (!transaction.setSettings(settings) ||
                !session_.commitTransaction(transaction)) {
                statusBar()->showMessage(
                    QStringLiteral("Could not switch the active view layer"),
                    4000);
                return;
            }

            for (const QJsonValue &sceneValue : settings.importedBlenderScenes) {
                const QJsonObject scene = sceneValue.toObject();
                for (const QJsonValue &layerValue : scene.value(
                         QStringLiteral("view_layers")).toArray()) {
                    if (layerValue.toObject().value(QStringLiteral("key"))
                            .toString() == key) {
                        activeViewLayerByScene_.insert(
                            scene.value(QStringLiteral("scene_id")).toString(),
                            key);
                        break;
                    }
                }
            }

            QHash<QString, ObjectId> objectIdsBySourceId;
            for (const SceneObject &object : session_.document().objects()) {
                if (!object.blenderLooseMeshEdgeCompanion &&
                    !object.blenderSourceObjectId.isEmpty()) {
                    objectIdsBySourceId.insert(object.blenderSourceObjectId,
                                               object.id);
                }
            }
            QVector<ObjectId> selectedIds;
            ObjectId activeId = ObjectId::invalid();
            const auto saved = viewLayerSelections_.constFind(key);
            if (saved != viewLayerSelections_.cend()) {
                for (const ObjectId id : saved->objectIds) {
                    if (id.isValid() &&
                        session_.document().isObjectEditable(id) &&
                        !selectedIds.contains(id)) {
                        selectedIds.append(id);
                    }
                }
                if (selectedIds.contains(saved->activeObjectId)) {
                    activeId = saved->activeObjectId;
                }
            } else {
                for (const QJsonValue &sceneValue : settings.importedBlenderScenes) {
                    const QJsonObject scene = sceneValue.toObject();
                    for (const QJsonValue &layerValue : scene.value(
                             QStringLiteral("view_layers")).toArray()) {
                        const QJsonObject layer = layerValue.toObject();
                        if (layer.value(QStringLiteral("key")).toString() != key) {
                            continue;
                        }
                        for (const QJsonValue &sourceIdValue : layer.value(
                                 QStringLiteral("selected_object_ids")).toArray()) {
                            const ObjectId id = objectIdsBySourceId.value(
                                sourceIdValue.toString(), ObjectId::invalid());
                            if (id.isValid() &&
                                session_.document().isObjectEditable(id) &&
                                !selectedIds.contains(id)) {
                                selectedIds.append(id);
                            }
                        }
                        const ObjectId requestedActiveId = objectIdsBySourceId.value(
                            layer.value(QStringLiteral("active_object_id")).toString(),
                            ObjectId::invalid());
                        if (selectedIds.contains(requestedActiveId)) {
                            activeId = requestedActiveId;
                        }
                        break;
                    }
                    if (activeId.isValid() || !selectedIds.isEmpty()) break;
                }
            }
            if (!activeId.isValid() && !selectedIds.isEmpty()) {
                activeId = selectedIds.back();
            }
            session_.selection().setObjectIds(selectedIds, activeId);
            session_.selection().setActiveObjectId(activeId);
            refreshCollections();
        };
        const auto setActiveViewLayer = collectionCallbacks.setActiveViewLayer;
        collectionCallbacks.setActiveScene =
            [this, setActiveViewLayer](const QString &sceneKey) {
                if (sceneKey.isEmpty()) return;
                for (const QJsonValue &sceneValue :
                     session_.document().settings().importedBlenderScenes) {
                    const QJsonObject scene = sceneValue.toObject();
                    if (scene.value(QStringLiteral("scene_id")).toString() !=
                        sceneKey) {
                        continue;
                    }
                    QString layerKey;
                    const QJsonArray layers = scene.value(
                        QStringLiteral("view_layers")).toArray();
                    const QString savedLayerKey =
                        activeViewLayerByScene_.value(sceneKey);
                    for (const QJsonValue &layerValue : layers) {
                        const QJsonObject layer = layerValue.toObject();
                        if (layer.value(QStringLiteral("key")).toString() ==
                            savedLayerKey) {
                            layerKey = savedLayerKey;
                            break;
                        }
                    }
                    for (const QJsonValue &layerValue : layers) {
                        if (!layerKey.isEmpty()) break;
                        const QJsonObject layer = layerValue.toObject();
                        if (layer.value(QStringLiteral("is_active_view_layer"))
                                .toBool()) {
                            layerKey = layer.value(
                                QStringLiteral("key")).toString();
                            break;
                        }
                    }
                    if (layerKey.isEmpty() && !layers.isEmpty()) {
                        layerKey = layers.first().toObject().value(
                            QStringLiteral("key")).toString();
                    }
                    if (!layerKey.isEmpty()) setActiveViewLayer(layerKey);
                    return;
                }
            };
        collectionCallbacks.selectedObjectIds = [this]() {
            return session_.selection().objectIds();
        };
        collectionCallbacks.activeObjectId = [this]() {
            return session_.selection().activeObjectId();
        };
        collectionCallbacks.canSelectObject = [this](ObjectId id) {
            return session_.document().isObjectEditable(id);
        };
        collectionCallbacks.objectVisibleInViewLayer =
            [this](ObjectId id, const QString &viewLayerKey) {
                return session_.document().isObjectVisible(id, viewLayerKey);
            };
        collectionCallbacks.objectSelectableInViewLayer =
            [this](ObjectId id, const QString &viewLayerKey) {
                return session_.document().isObjectEditable(id, viewLayerKey);
            };
        collectionCallbacks.setObjectSelection =
            [this](const QVector<ObjectId> &objectIds, ObjectId activeObjectId) {
                QVector<ObjectId> selectableIds;
                for (const ObjectId id : objectIds) {
                    if (session_.document().isObjectEditable(id) &&
                        !selectableIds.contains(id)) {
                        selectableIds.append(id);
                    }
                }
                ObjectId active = activeObjectId;
                if (!selectableIds.contains(active)) {
                    active = selectableIds.isEmpty() ? ObjectId::invalid()
                                                     : selectableIds.back();
                }
                session_.selection().setObjectIds(selectableIds, active);
                session_.selection().setActiveObjectId(active);
            };
        collectionCallbacks.createCollection =
            [this, activeSceneRecord](const QString &name,
                                      CollectionId parentId) {
                DocumentCollection collection;
                collection.name = name;
                if (parentId.isValid()) collection.parentIds = {parentId};
                const QJsonObject activeScene = activeSceneRecord();
                const QString sceneId = activeScene.value(
                    QStringLiteral("scene_id")).toString();
                const QString sceneName = activeScene.value(
                    QStringLiteral("name")).toString();
                if (!sceneId.isEmpty() && !sceneName.isEmpty()) {
                    collection.sourceSceneName = sceneName;
                    collection.sourceSceneNames.append(sceneName);
                }
                DocumentTransaction transaction = session_.beginTransaction();
                const CollectionId id = transaction.insertCollection(collection);
                if (!id.isValid() ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not create collection"), 4000);
                    return CollectionId::invalid();
                }
                return id;
            };
        collectionCallbacks.renameCollection =
            [this](CollectionId id, const QString &name) {
                DocumentTransaction transaction = session_.beginTransaction();
                if (!transaction.renameCollection(id, name) ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not rename collection"), 4000);
                }
            };
        collectionCallbacks.removeCollection = [this](CollectionId id) {
            DocumentTransaction transaction = session_.beginTransaction();
            if (!transaction.removeCollection(id) ||
                !session_.commitTransaction(transaction)) {
                statusBar()->showMessage(
                    QStringLiteral("Could not remove collection"), 4000);
                return;
            }
            pruneOutlinerSelection();
        };
        collectionCallbacks.linkObjects =
            [this](const QVector<ObjectId> &objectIds, CollectionId collectionId) {
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const ObjectId id : objectIds) {
                    const SceneObject *object = session_.document().object(id);
                    if (object == nullptr ||
                        object->collectionIds.contains(collectionId)) {
                        continue;
                    }
                    QVector<CollectionId> memberships = object->collectionIds;
                    memberships.append(collectionId);
                    changed = transaction.setObjectCollections(id, memberships) ||
                              changed;
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not link objects to collection"),
                        4000);
                }
            };
        collectionCallbacks.dropRows =
            [this](const QVector<OutlinerRowReference> &sources,
                   const OutlinerRowReference &target,
                   bool link,
                   bool parentObjects) {
                if (sources.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                CollectionId destinationCollection =
                    target.kind == OutlinerRowKind::Collection
                        ? target.collectionId
                        : CollectionId::invalid();
                if (target.kind == OutlinerRowKind::Object) {
                    destinationCollection = target.parentCollectionId;
                }
                const ObjectId destinationObject =
                    target.kind == OutlinerRowKind::Object
                        ? target.objectId : ObjectId::invalid();
                QSet<quint64> handledObjects;
                QSet<quint64> handledCollections;
                for (const OutlinerRowReference &source : sources) {
                    if (source.kind == OutlinerRowKind::Collection) {
                        const CollectionId collectionId = source.collectionId;
                        const DocumentCollection *collection =
                            session_.document().collection(collectionId);
                        if (collection == nullptr ||
                            !collection->libraryPath.isEmpty() ||
                            handledCollections.contains(collectionId.value())) {
                            continue;
                        }
                        handledCollections.insert(collectionId.value());
                        if (target.kind == OutlinerRowKind::Collection &&
                            target.collectionId == collectionId) {
                            continue;
                        }
                        QVector<CollectionId> parents = collection->parentIds;
                        if (!link && source.parentCollectionId.isValid()) {
                            parents.removeAll(source.parentCollectionId);
                        }
                        if (destinationCollection.isValid() &&
                            !parents.contains(destinationCollection)) {
                            parents.append(destinationCollection);
                        }
                        if (parents != collection->parentIds) {
                            changed = transaction.setCollectionParents(
                                          collectionId, parents) || changed;
                        }
                        continue;
                    }

                    if (source.kind != OutlinerRowKind::Object ||
                        !source.objectId.isValid() ||
                        handledObjects.contains(source.objectId.value())) {
                        continue;
                    }
                    handledObjects.insert(source.objectId.value());
                    const SceneObject *object =
                        session_.document().object(source.objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty()) {
                        continue;
                    }
                    if (parentObjects && destinationObject.isValid()) {
                        const SceneObject *parentObject =
                            session_.document().object(destinationObject);
                        const QString parentIdentity = parentObject == nullptr
                            ? QString{}
                            : (parentObject->blenderSourceObjectId.isEmpty()
                                   ? QStringLiteral("classiCAD-local-object:%1")
                                         .arg(destinationObject.value())
                                   : parentObject->blenderSourceObjectId);
                        if (source.objectId != destinationObject &&
                            !parentIdentity.isEmpty() &&
                            object->blenderParentObjectId != parentIdentity) {
                            changed = transaction.setObjectParent(
                                          source.objectId, destinationObject) ||
                                      changed;
                        }
                        continue;
                    }
                    if (link && !destinationCollection.isValid()) continue;

                    QVector<CollectionId> memberships = object->collectionIds;
                    if (!link && source.linkedCollectionId.isValid() &&
                        !source.syntheticParentRow) {
                        memberships.removeAll(source.linkedCollectionId);
                    }
                    if (destinationCollection.isValid() &&
                        !memberships.contains(destinationCollection)) {
                        memberships.append(destinationCollection);
                    }
                    if (memberships != object->collectionIds) {
                        changed = transaction.setObjectCollections(
                                      source.objectId, memberships) || changed;
                    }
                }
                if (changed && !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not apply Outliner drop"), 4000);
                    return;
                }
                if (changed) pruneOutlinerSelection();
            };
        collectionCallbacks.renameObject =
            [this](ObjectId id, const QString &name) {
                DocumentTransaction transaction = session_.beginTransaction();
                if (!transaction.setObjectDisplayName(id, name) ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not rename object"), 4000);
                }
            };
        collectionCallbacks.unlinkObject =
            [this](ObjectId id, CollectionId collectionId) {
                const SceneObject *object = session_.document().object(id);
                if (object == nullptr ||
                    !object->collectionIds.contains(collectionId)) return;
                QVector<CollectionId> memberships = object->collectionIds;
                memberships.removeAll(collectionId);
                DocumentTransaction transaction = session_.beginTransaction();
                if (!transaction.setObjectCollections(id, memberships) ||
                    !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not unlink object from collection"),
                        4000);
                }
            };
        const auto objectHierarchyIds = [this](ObjectId root) {
            QVector<ObjectId> result;
            const Document &document = session_.document();
            const SceneObject *rootObject = document.object(root);
            if (rootObject == nullptr ||
                rootObject->blenderLooseMeshEdgeCompanion) {
                return result;
            }
            result.append(root);
            QSet<quint64> visited{root.value()};
            const auto identityFor = [](const SceneObject &object) {
                return object.blenderSourceObjectId.isEmpty()
                    ? QStringLiteral("classiCAD-local-object:%1")
                          .arg(object.id.value())
                    : object.blenderSourceObjectId;
            };
            for (qsizetype index = 0; index < result.size(); ++index) {
                const SceneObject *parent = document.object(result[index]);
                if (parent == nullptr) continue;
                const QString parentIdentity = identityFor(*parent);
                for (const SceneObject &candidate : document.objects()) {
                    if (candidate.blenderLooseMeshEdgeCompanion ||
                        candidate.blenderParentObjectId != parentIdentity ||
                        visited.contains(candidate.id.value())) {
                        continue;
                    }
                    visited.insert(candidate.id.value());
                    result.append(candidate.id);
                }
            }

            QSet<QString> sourceIdentities;
            for (const ObjectId id : result) {
                const SceneObject *object = document.object(id);
                if (object != nullptr) sourceIdentities.insert(identityFor(*object));
            }
            for (const SceneObject &candidate : document.objects()) {
                if (candidate.blenderLooseMeshEdgeCompanion &&
                    sourceIdentities.contains(candidate.blenderSourceObjectId) &&
                    !visited.contains(candidate.id.value())) {
                    visited.insert(candidate.id.value());
                    result.append(candidate.id);
                }
            }
            return result;
        };
        const auto collectionHierarchyIds =
            [this, collectionBelongsToActiveScene](CollectionId root) {
            QVector<CollectionId> result;
            const DocumentCollection *rootCollection =
                session_.document().collection(root);
            if (rootCollection == nullptr ||
                !collectionBelongsToActiveScene(*rootCollection)) {
                return result;
            }
            result.append(root);
            QSet<quint64> visited{root.value()};
            const QVector<DocumentCollection> collections =
                session_.document().collections();
            for (qsizetype index = 0; index < result.size(); ++index) {
                const CollectionId parentId = result[index];
                for (const DocumentCollection &candidate : collections) {
                    if (collectionBelongsToActiveScene(candidate) &&
                        candidate.parentIds.contains(parentId) &&
                        !visited.contains(candidate.id.value())) {
                        visited.insert(candidate.id.value());
                        result.append(candidate.id);
                    }
                }
            }
            return result;
        };
        const auto collectionRelationIds =
            [this, collectionHierarchyIds,
             collectionBelongsToActiveScene](CollectionId root) {
                QVector<CollectionId> result = collectionHierarchyIds(root);
                QSet<quint64> visited;
                for (const CollectionId id : result) visited.insert(id.value());
                QVector<CollectionId> pending{root};
                for (qsizetype index = 0; index < pending.size(); ++index) {
                    const DocumentCollection *collection =
                        session_.document().collection(pending[index]);
                    if (collection == nullptr) continue;
                    for (const CollectionId parentId : collection->parentIds) {
                        if (visited.contains(parentId.value())) continue;
                        const DocumentCollection *parent =
                            session_.document().collection(parentId);
                        if (parent == nullptr ||
                            !collectionBelongsToActiveScene(*parent)) {
                            continue;
                        }
                        visited.insert(parentId.value());
                        result.append(parentId);
                        pending.append(parentId);
                    }
                }
                return result;
            };
        const auto collectionIsolationPlan =
            [this, collectionRelationIds, collectionBelongsToActiveScene](
                CollectionId root,
                bool clickedHidden,
                const std::function<bool(const DocumentCollection &)> &flagValue) {
                const QVector<DocumentCollection> collections =
                    session_.document().collections();
                const QVector<CollectionId> relatedIds =
                    collectionRelationIds(root);
                QSet<quint64> related;
                for (const CollectionId id : relatedIds) {
                    related.insert(id.value());
                }

                bool alreadyIsolated = clickedHidden && !related.isEmpty();
                if (alreadyIsolated) {
                    for (const DocumentCollection &candidate : collections) {
                        if (!collectionBelongsToActiveScene(candidate) ||
                            candidate.id == root ||
                            !candidate.libraryPath.isEmpty()) {
                            continue;
                        }
                        const bool expectedHidden =
                            !related.contains(candidate.id.value());
                        if (flagValue(candidate) != expectedHidden) {
                            alreadyIsolated = false;
                            break;
                        }
                    }
                }

                QHash<quint64, bool> plan;
                for (const DocumentCollection &candidate : collections) {
                    if (!collectionBelongsToActiveScene(candidate)) continue;
                    const bool hidden = alreadyIsolated
                        ? false : !related.contains(candidate.id.value());
                    plan.insert(candidate.id.value(), hidden);
                }
                return plan;
            };
        const auto objectsInCollections = [this](
            const QVector<CollectionId> &collectionIds) {
            QVector<ObjectId> result;
            QSet<quint64> collectionValues;
            for (const CollectionId id : collectionIds) {
                collectionValues.insert(id.value());
            }
            QSet<quint64> visited;
            for (const SceneObject &object : session_.document().objects()) {
                bool member = false;
                for (const CollectionId id : object.collectionIds) {
                    if (collectionValues.contains(id.value())) {
                        member = true;
                        break;
                    }
                }
                if (member && !visited.contains(object.id.value())) {
                    visited.insert(object.id.value());
                    result.append(object.id);
                }
            }
            return result;
        };
        const auto objectInViewLayer = [this](const SceneObject &object,
                                               const QString &viewLayerKey) {
            if (viewLayerKey.isEmpty() || object.collectionIds.isEmpty()) {
                return true;
            }
            const Document &document = session_.document();
            const auto matchesPath = [](const QJsonArray &stored,
                                        const QStringList &expected) {
                if (stored.size() != expected.size()) return false;
                for (qsizetype index = 0; index < expected.size(); ++index) {
                    if (!stored[index].isString() ||
                        stored[index].toString() != expected[index]) {
                        return false;
                    }
                }
                return true;
            };
            for (const CollectionId membershipId : object.collectionIds) {
                const DocumentCollection *membership =
                    document.collection(membershipId);
                if (membership == nullptr) continue;
                for (const QJsonValue &membershipValue :
                     membership->viewLayerStates) {
                    const QJsonObject membershipState =
                        membershipValue.toObject();
                    if (membershipState.value(
                            QStringLiteral("view_layer_key")).toString() !=
                        viewLayerKey) {
                        continue;
                    }
                    QStringList sourcePath;
                    const QJsonArray storedPath = membershipState.value(
                        QStringLiteral("path")).toArray();
                    for (const QJsonValue &part : storedPath) {
                        if (!part.isString()) {
                            sourcePath.clear();
                            break;
                        }
                        sourcePath.append(part.toString());
                    }
                    if (sourcePath.isEmpty() ||
                        sourcePath.back() != membership->sourceId) {
                        continue;
                    }

                    bool included = true;
                    CollectionId previous = CollectionId::invalid();
                    for (qsizetype index = 0; index < sourcePath.size(); ++index) {
                        const QString &sourceId = sourcePath[index];
                        const auto node = std::find_if(
                            document.collections().cbegin(),
                            document.collections().cend(),
                            [&sourceId, previous](
                                const DocumentCollection &candidate) {
                                return candidate.sourceId == sourceId &&
                                    (!previous.isValid() ||
                                     candidate.parentIds.contains(previous));
                            });
                        if (node == document.collections().cend()) {
                            included = false;
                            break;
                        }
                        QStringList prefix;
                        for (qsizetype prefixIndex = 0;
                             prefixIndex <= index; ++prefixIndex) {
                            prefix.append(sourcePath[prefixIndex]);
                        }
                        bool foundState = false;
                        bool excluded = false;
                        for (const QJsonValue &stateValue : node->viewLayerStates) {
                            const QJsonObject state = stateValue.toObject();
                            if (state.value(QStringLiteral("view_layer_key"))
                                    .toString() == viewLayerKey &&
                                matchesPath(state.value(QStringLiteral("path"))
                                                .toArray(),
                                            prefix)) {
                                foundState = true;
                                excluded = state.value(
                                    QStringLiteral("exclude")).toBool();
                                break;
                            }
                        }
                        if (!foundState || excluded) {
                            included = false;
                            break;
                        }
                        previous = node->id;
                    }
                    if (included) return true;
                }
            }
            return false;
        };
        collectionCallbacks.setViewportVisibility =
            [this, collectionHierarchyIds, objectsInCollections,
             collectionIsolationPlan, collectionBelongsToActiveScene](
                CollectionId id, bool hideViewport, bool recursive,
                bool isolate) {
                if (isolate) {
                    const QHash<quint64, bool> plan = collectionIsolationPlan(
                        id, hideViewport, [](const DocumentCollection &entry) {
                            return entry.hideViewport;
                        });
                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const DocumentCollection &entry :
                         session_.document().collections()) {
                        if (!collectionBelongsToActiveScene(entry) ||
                            !entry.libraryPath.isEmpty() ||
                            !plan.contains(entry.id.value())) {
                            continue;
                        }
                        const bool hidden = plan.value(entry.id.value());
                        if (entry.hideViewport == hidden) continue;
                        changed = transaction.setCollectionVisibility(
                                      entry.id, hidden, entry.hideRender) || changed;
                    }
                    if (changed && !session_.commitTransaction(transaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not isolate collection visibility"),
                            4000);
                        return;
                    }
                    if (changed) pruneOutlinerSelection();
                    return;
                }
                QVector<CollectionId> collectionIds = recursive
                    ? collectionHierarchyIds(id) : QVector<CollectionId>{id};
                QVector<ObjectId> objectIds = recursive
                    ? objectsInCollections(collectionIds) : QVector<ObjectId>{};
                if (collectionIds.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const CollectionId collectionId : collectionIds) {
                    const DocumentCollection *collection =
                        session_.document().collection(collectionId);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty() ||
                        collection->hideViewport == hideViewport) {
                        continue;
                    }
                    changed = transaction.setCollectionVisibility(
                                  collectionId, hideViewport,
                                  collection->hideRender) || changed;
                }
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        object->hideViewport == hideViewport) {
                        continue;
                    }
                    changed = transaction.setObjectVisibilityFlags(
                                  objectId, hideViewport, object->hideSelect) ||
                              changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update collection visibility"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setCollectionRenderVisibility =
            [this, collectionHierarchyIds, objectsInCollections,
             collectionIsolationPlan, collectionBelongsToActiveScene](
                CollectionId id, bool hideRender, bool recursive,
                bool isolate) {
                if (isolate) {
                    const QHash<quint64, bool> plan = collectionIsolationPlan(
                        id, hideRender, [](const DocumentCollection &entry) {
                            return entry.hideRender;
                        });
                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const DocumentCollection &entry :
                         session_.document().collections()) {
                        if (!collectionBelongsToActiveScene(entry) ||
                            !entry.libraryPath.isEmpty() ||
                            !plan.contains(entry.id.value())) {
                            continue;
                        }
                        const bool hidden = plan.value(entry.id.value());
                        if (entry.hideRender == hidden) continue;
                        changed = transaction.setCollectionVisibility(
                                      entry.id, entry.hideViewport, hidden) || changed;
                    }
                    if (changed && !session_.commitTransaction(transaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not isolate collection render visibility"),
                            4000);
                    }
                    return;
                }
                QVector<CollectionId> collectionIds = recursive
                    ? collectionHierarchyIds(id) : QVector<CollectionId>{id};
                QVector<ObjectId> objectIds = recursive
                    ? objectsInCollections(collectionIds) : QVector<ObjectId>{};
                if (collectionIds.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const CollectionId collectionId : collectionIds) {
                    const DocumentCollection *collection =
                        session_.document().collection(collectionId);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty() ||
                        collection->hideRender == hideRender) {
                        continue;
                    }
                    changed = transaction.setCollectionVisibility(
                                  collectionId, collection->hideViewport,
                                  hideRender) || changed;
                }
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        object->hideRender == hideRender) {
                        continue;
                    }
                    changed = transaction.setObjectRenderVisibility(
                                  objectId, hideRender) || changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update collection render visibility"),
                        4000);
                }
            };
        collectionCallbacks.setCollectionSelectability =
            [this, collectionHierarchyIds, objectsInCollections,
             collectionIsolationPlan, collectionBelongsToActiveScene](
                CollectionId id, bool hideSelect, bool recursive,
                bool isolate) {
                if (isolate) {
                    const QHash<quint64, bool> plan = collectionIsolationPlan(
                        id, hideSelect, [](const DocumentCollection &entry) {
                            return entry.hideSelect;
                        });
                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const DocumentCollection &entry :
                         session_.document().collections()) {
                        if (!collectionBelongsToActiveScene(entry) ||
                            !entry.libraryPath.isEmpty() ||
                            !plan.contains(entry.id.value())) {
                            continue;
                        }
                        const bool hidden = plan.value(entry.id.value());
                        if (entry.hideSelect == hidden) continue;
                        changed = transaction.setCollectionHideSelect(
                                      entry.id, hidden) || changed;
                    }
                    if (changed && !session_.commitTransaction(transaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not isolate collection selection"),
                            4000);
                        return;
                    }
                    pruneOutlinerSelection();
                    return;
                }
                QVector<CollectionId> collectionIds = recursive
                    ? collectionHierarchyIds(id) : QVector<CollectionId>{id};
                QVector<ObjectId> objectIds = recursive
                    ? objectsInCollections(collectionIds) : QVector<ObjectId>{};
                if (collectionIds.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const CollectionId collectionId : collectionIds) {
                    const DocumentCollection *collection =
                        session_.document().collection(collectionId);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty() ||
                        collection->hideSelect == hideSelect) {
                        continue;
                    }
                    changed = transaction.setCollectionHideSelect(
                                  collectionId, hideSelect) || changed;
                }
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        object->hideSelect == hideSelect) {
                        continue;
                    }
                    changed = transaction.setObjectVisibilityFlags(
                                  objectId, object->hideViewport, hideSelect) ||
                              changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update collection selection lock"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setObjectViewportVisibility =
            [this, objectHierarchyIds](ObjectId id, bool hideViewport,
                                       bool recursive) {
                const QVector<ObjectId> objectIds = recursive
                    ? objectHierarchyIds(id) : QVector<ObjectId>{id};
                if (objectIds.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        object->hideViewport == hideViewport) {
                        continue;
                    }
                    changed = transaction.setObjectVisibilityFlags(
                                  objectId, hideViewport, object->hideSelect) ||
                              changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update object visibility"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setObjectRenderVisibility =
            [this, objectHierarchyIds](ObjectId id, bool hideRender,
                                       bool recursive) {
                const QVector<ObjectId> objectIds = recursive
                    ? objectHierarchyIds(id) : QVector<ObjectId>{id};
                if (objectIds.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        object->hideRender == hideRender) {
                        continue;
                    }
                    changed = transaction.setObjectRenderVisibility(
                                  objectId, hideRender) || changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update object render visibility"),
                        4000);
                }
            };
        collectionCallbacks.setObjectSelectability =
            [this, objectHierarchyIds](ObjectId id, bool hideSelect,
                                       bool recursive) {
                const QVector<ObjectId> objectIds = recursive
                    ? objectHierarchyIds(id) : QVector<ObjectId>{id};
                if (objectIds.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        object->hideSelect == hideSelect) {
                        continue;
                    }
                    changed = transaction.setObjectVisibilityFlags(
                                  objectId, object->hideViewport, hideSelect) ||
                              changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update object selection lock"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setObjectViewLayerHidden =
            [this, objectHierarchyIds, objectInViewLayer](ObjectId id,
                                       const QString &viewLayerKey,
                                       bool hidden, bool recursive) {
                const QVector<ObjectId> objectIds = recursive
                    ? objectHierarchyIds(id) : QVector<ObjectId>{id};
                if (objectIds.isEmpty() || viewLayerKey.isEmpty()) return;
                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const ObjectId objectId : objectIds) {
                    const SceneObject *object =
                        session_.document().object(objectId);
                    if (object == nullptr || !object->libraryPath.isEmpty() ||
                        !objectInViewLayer(*object, viewLayerKey) ||
                        (object->viewLayerHidden.contains(viewLayerKey) &&
                         object->viewLayerHidden.value(viewLayerKey).toBool() ==
                             hidden)) {
                        continue;
                    }
                    changed = transaction.setObjectViewLayerHidden(
                                  objectId, viewLayerKey, hidden) || changed;
                }
                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update active view layer visibility"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionCallbacks.setCollectionViewLayerState =
            [this, objectsInCollections, collectionRelationIds,
             collectionBelongsToActiveScene](
                   CollectionId id,
                   const QString &viewLayerKey,
                   const QStringList &sourcePath,
                   const QString &stateKey,
                   bool value,
                   bool recursive,
                   bool isolate) {
                if (viewLayerKey.isEmpty() || sourcePath.isEmpty()) return;

                struct LayerCollectionPath {
                    CollectionId id = CollectionId::invalid();
                    QStringList sourcePath;
                    QSet<quint64> ancestors;
                    bool excluded = false;
                };
                const auto stateForPath = [](const DocumentCollection &collection,
                                             const QString &layerKey,
                                             const QStringList &path) {
                    for (const QJsonValue &value : collection.viewLayerStates) {
                        const QJsonObject state = value.toObject();
                        if (state.value(QStringLiteral("view_layer_key"))
                                .toString() != layerKey) {
                            continue;
                        }
                        const QJsonArray storedPath =
                            state.value(QStringLiteral("path")).toArray();
                        if (storedPath.size() != path.size()) continue;
                        bool matches = true;
                        for (qsizetype index = 0; index < path.size(); ++index) {
                            if (!storedPath[index].isString() ||
                                storedPath[index].toString() != path[index]) {
                                matches = false;
                                break;
                            }
                        }
                        if (matches) return state;
                    }
                    return QJsonObject{};
                };

                if (isolate) {
                    QVector<LayerCollectionPath> allPaths;
                    for (const DocumentCollection &collection :
                         session_.document().collections()) {
                        if (!collectionBelongsToActiveScene(collection)) continue;
                        for (const QJsonValue &stateValue :
                             collection.viewLayerStates) {
                            const QJsonObject state = stateValue.toObject();
                            if (state.value(QStringLiteral("view_layer_key"))
                                    .toString() != viewLayerKey) {
                                continue;
                            }
                            QStringList path;
                            for (const QJsonValue &part : state.value(
                                     QStringLiteral("path")).toArray()) {
                                if (!part.isString()) {
                                    path.clear();
                                    break;
                                }
                                path.append(part.toString());
                            }
                            if (path.isEmpty()) continue;
                            LayerCollectionPath entry;
                            entry.id = collection.id;
                            entry.sourcePath = path;
                            allPaths.append(entry);
                        }
                    }
                    const auto isPrefix = [](const QStringList &prefix,
                                             const QStringList &path) {
                        if (prefix.size() > path.size()) return false;
                        for (qsizetype index = 0; index < prefix.size(); ++index) {
                            if (prefix[index] != path[index]) return false;
                        }
                        return true;
                    };
                    QSet<quint64> relatedCollections;
                    for (const CollectionId relatedId :
                         collectionRelationIds(id)) {
                        relatedCollections.insert(relatedId.value());
                    }
                    const bool isHideState =
                        stateKey == QStringLiteral("exclude") ||
                        stateKey.startsWith(QStringLiteral("hide_"));
                    bool alreadyIsolated = value == isHideState;
                    if (alreadyIsolated) {
                        for (const LayerCollectionPath &path : allPaths) {
                            const DocumentCollection *collection =
                                session_.document().collection(path.id);
                            if (collection == nullptr ||
                                !collection->libraryPath.isEmpty() ||
                                path.id == id) {
                                continue;
                            }
                            const QJsonObject current = stateForPath(
                                *collection, viewLayerKey, path.sourcePath);
                            const bool related = relatedCollections.contains(
                                path.id.value());
                            const bool expectedValue = related
                                ? !isHideState : isHideState;
                            if (current.value(stateKey).toBool() !=
                                expectedValue) {
                                alreadyIsolated = false;
                                break;
                            }
                        }
                    }

                    DocumentTransaction transaction = session_.beginTransaction();
                    bool changed = false;
                    for (const LayerCollectionPath &path : allPaths) {
                        const DocumentCollection *collection =
                            session_.document().collection(path.id);
                        if (collection == nullptr ||
                            !collection->libraryPath.isEmpty()) {
                            continue;
                        }
                        const QJsonObject current = stateForPath(
                            *collection, viewLayerKey, path.sourcePath);
                        if (current.isEmpty()) continue;
                        const bool visibleInTargetBranch =
                            isPrefix(sourcePath, path.sourcePath) ||
                            isPrefix(path.sourcePath, sourcePath);
                        const bool flagValue = alreadyIsolated
                            ? false
                            : (visibleInTargetBranch ? !isHideState
                                                     : isHideState);
                        if (current.value(stateKey).isBool() &&
                            current.value(stateKey).toBool() == flagValue) {
                            continue;
                        }
                        changed = transaction.setCollectionViewLayerState(
                                      path.id, viewLayerKey, path.sourcePath,
                                      stateKey, flagValue) || changed;
                    }
                    if (changed && !session_.commitTransaction(transaction)) {
                        statusBar()->showMessage(
                            QStringLiteral("Could not isolate active view layer collection"),
                            4000);
                        return;
                    }
                    if (changed) pruneOutlinerSelection();
                    return;
                }

                QVector<LayerCollectionPath> paths;
                const DocumentCollection *rootCollection =
                    session_.document().collection(id);
                if (rootCollection == nullptr) return;
                LayerCollectionPath rootPath;
                rootPath.id = id;
                rootPath.sourcePath = sourcePath;
                rootPath.ancestors.insert(id.value());
                rootPath.excluded = stateForPath(*rootCollection, viewLayerKey,
                                                  sourcePath)
                                        .value(QStringLiteral("exclude"))
                                        .toBool();
                paths.append(rootPath);
                if (recursive) {
                    const QVector<DocumentCollection> collections =
                        session_.document().collections();
                    for (qsizetype index = 0; index < paths.size(); ++index) {
                        const LayerCollectionPath parent = paths[index];
                        for (const DocumentCollection &candidate : collections) {
                            if (!collectionBelongsToActiveScene(candidate) ||
                                !candidate.parentIds.contains(parent.id) ||
                                parent.ancestors.contains(candidate.id.value())) {
                                continue;
                            }
                            LayerCollectionPath child;
                            child.id = candidate.id;
                            child.sourcePath = parent.sourcePath;
                            child.sourcePath.append(candidate.sourceId);
                            child.ancestors = parent.ancestors;
                            child.ancestors.insert(candidate.id.value());
                            child.excluded = parent.excluded ||
                                stateForPath(candidate, viewLayerKey,
                                             child.sourcePath)
                                    .value(QStringLiteral("exclude"))
                                    .toBool();
                            paths.append(child);
                        }
                    }
                }

                DocumentTransaction transaction = session_.beginTransaction();
                bool changed = false;
                for (const LayerCollectionPath &path : paths) {
                    const DocumentCollection *collection =
                        session_.document().collection(path.id);
                    if (collection == nullptr ||
                        !collection->libraryPath.isEmpty()) {
                        continue;
                    }
                    const QJsonObject current = stateForPath(
                        *collection, viewLayerKey, path.sourcePath);
                    if (current.isEmpty() ||
                        (current.value(stateKey).isBool() &&
                         current.value(stateKey).toBool() == value)) {
                        continue;
                    }
                    changed = transaction.setCollectionViewLayerState(
                                  path.id, viewLayerKey, path.sourcePath,
                                  stateKey, value) || changed;
                }

                if (recursive && stateKey == QStringLiteral("hide_viewport")) {
                    QSet<quint64> processedObjects;
                    for (const LayerCollectionPath &path : paths) {
                        if (path.excluded) continue;
                        const QVector<ObjectId> objectIds =
                            objectsInCollections(
                                QVector<CollectionId>{path.id});
                        for (const ObjectId objectId : objectIds) {
                            if (processedObjects.contains(objectId.value())) {
                                continue;
                            }
                            processedObjects.insert(objectId.value());
                            const SceneObject *object =
                                session_.document().object(objectId);
                            if (object == nullptr ||
                                !object->libraryPath.isEmpty() ||
                                (object->viewLayerHidden.contains(viewLayerKey) &&
                                 object->viewLayerHidden.value(viewLayerKey)
                                         .toBool() == value)) {
                                continue;
                            }
                            changed = transaction.setObjectViewLayerHidden(
                                          objectId, viewLayerKey, value) || changed;
                        }
                    }
                }

                if (!changed || !session_.commitTransaction(transaction)) {
                    statusBar()->showMessage(
                        QStringLiteral("Could not update active view layer collection state"),
                        4000);
                    return;
                }
                pruneOutlinerSelection();
            };
        collectionsPanel_ = new CollectionsPanel(collectionCallbacks);

        return collectionsPanel_;
    }

    void applyTheme()
    {
        setStyleSheet(classicadThemeStyleSheet());
    }

    ApplicationSession session_;
    struct SavedViewLayerSelection {
        QVector<ObjectId> objectIds;
        ObjectId activeObjectId = ObjectId::invalid();
    };
    QHash<QString, SavedViewLayerSelection> viewLayerSelections_;
    QHash<QString, QString> activeViewLayerByScene_;
    ProjectController projectController_{session_};
    UpdateController updateController_;
    ViewportWidgetApi *viewport_ = nullptr;
    WorkbenchRenderResult lastWorkbenchRenderResult_;
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
    QActionGroup *workPlaneActionGroup_ = nullptr;
    CollectionsPanel *collectionsPanel_ = nullptr;
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
    QCheckBox *lineAutoWeldCheckBox_ = nullptr;
    QButtonGroup *componentModeButtonGroup_ = nullptr;
    QButtonGroup *shadingModeButtonGroup_ = nullptr;
    QComboBox *interactionModeComboBox_ = nullptr;
    QToolButton *xrayToggleButton_ = nullptr;
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
